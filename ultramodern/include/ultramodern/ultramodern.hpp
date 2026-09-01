#ifndef __ultramodern_HPP__
#define __ultramodern_HPP__

#include <thread>
#include <cassert>
#include <stdexcept>
#include <span>
#include <chrono>
#include <filesystem>

#undef MOODYCAMEL_DELETE_FUNCTION
#define MOODYCAMEL_DELETE_FUNCTION = delete
#include "lightweightsemaphore.h"

#include "ultra64.h"

#include "ultramodern/error_handling.hpp"
#include "ultramodern/events.hpp"
#include "ultramodern/input.hpp"
#include "ultramodern/renderer_context.hpp"
#include "ultramodern/rsp.hpp"
#include "ultramodern/threads.hpp"

struct UltraThreadContext {
    std::thread host_thread;
    moodycamel::LightweightSemaphore running;
    moodycamel::LightweightSemaphore initialized;
};

namespace ultramodern {

constexpr uint32_t save_size = 1024 * 1024 / 8; // Maximum save size, 1Mbit for flash

// Initialization.
void preinit(RDRAM_ARG renderer::WindowHandle window_handle);
void init_saving(RDRAM_ARG1);
void init_events(RDRAM_ARG renderer::WindowHandle window_handle);
void init_timers(RDRAM_ARG1);
void init_thread_cleanup();

// Saving
void change_save_file(const std::u8string& subfolder, const std::u8string& name);
std::filesystem::path get_save_file_path();

// Where a game's save file WOULD live, without requiring init_saving() to have
// run. get_save_file_path() reports the live path and is empty until the game
// starts, so anything that touches save files before then -- importing one, for
// instance -- needs this instead. Single source of truth for the layout: the
// live path is built from it too.
std::filesystem::path get_save_file_path_for(const std::u8string& subfolder, const std::u8string& name);

// Save observation hooks.
//
// Both are optional, default to unset, and carry no policy: librecomp reports
// what happened to the save data and the game decides what it means. Set them
// during startup, before init_saving() spawns the saving thread; they are not
// designed to be swapped while the game is running.

// Invoked whenever the guest mutates the save buffer, with the affected buffer
// offset and byte count. Runs on the GUEST thread, after the mutation has been
// published to the save buffer but BEFORE the saving thread is signalled --
// deliberately, so that any state the callback sets is already visible to the
// first flush that can contain this write. Signalling first would let that
// flush race ahead of the callback. Fired by every mutating path uniformly --
// pak/sram writes, flash page writes, and flash erases -- so "the guest changed
// the save data" is the whole contract.
using save_write_callback_t = void (*)(uint32_t offset, uint32_t count);
void set_save_write_callback(save_write_callback_t callback);

// Invoked after the save file has been written and finalized on disk, and only
// when that succeeded. Runs on the SAVING thread; `path` is the file just
// finalized, so it is safe to read or copy from within the callback.
using save_flush_callback_t = void (*)(const std::filesystem::path& path);
void set_save_flush_callback(save_flush_callback_t callback);

// Thread queues.
constexpr PTR(PTR(OSThread)) running_queue = (PTR(PTR(OSThread)))-1;

void thread_queue_insert(RDRAM_ARG PTR(PTR(OSThread)) queue, PTR(OSThread) toadd);
PTR(OSThread) thread_queue_pop(RDRAM_ARG PTR(PTR(OSThread)) queue);
bool thread_queue_remove(RDRAM_ARG PTR(PTR(OSThread)) queue_, PTR(OSThread) t_);
bool thread_queue_empty(RDRAM_ARG PTR(PTR(OSThread)) queue);
PTR(OSThread) thread_queue_peek(RDRAM_ARG PTR(PTR(OSThread)) queue);

// Message queues.
void enqueue_external_message(PTR(OSMesgQueue) mq, OSMesg msg, bool jam, bool requeue_if_blocked);
void wait_for_external_message(RDRAM_ARG1);
void wait_for_external_message_timed(RDRAM_ARG1, u32 millis);

// Debug: approximate depths of the three unbounded queues. Each is a
// BlockingConcurrentQueue with no ceiling, so a consumer that stops draining
// turns one of them into a memory leak. Read by the port's HH_TRACE_MEM
// reporter to tell which -- if any -- is the one growing.
size_t debug_external_message_count();
// Cumulative external messages discarded because the guest queue was full and the
// sender did not ask for a requeue. See mesgqueue.cpp -- the count above cannot
// see these, because a message is popped before delivery is attempted.
uint64_t debug_external_message_drops();
// Cumulative VI retrace messages actually SENT to the guest -- gated by
// retrace_count, so not the same as the number of VI iterations.
uint64_t debug_retrace_messages_sent();
// Cumulative osSendMesg calls FROM THE GUEST that failed because the target queue
// was full. The guest just gets -1 back and usually ignores it.
uint64_t debug_guest_send_failures();
// Cumulative osSpTaskYield calls. The only caller in Hybrid Heaven is the guest's
// audio-dispatch thread (func_80000A5C_165C), which yields ONLY when it finds a
// gfx task in flight on the RSP (sc+0x88C non-null) at the moment an audio task
// arrives. The rate of this counter against the audio task rate is therefore a
// direct measurement of how often the audio pipeline takes the yield/handoff
// path -- the path where, with yields ignored (librecomp/sp.cpp), the audio
// dispatcher consumes the gfx task's SP-done off the shared sc+0xE8 queue and
// re-posts a substitute after its own task completes.
void debug_note_sp_task_yield();
uint64_t debug_sp_task_yields();
// External-message delivery latency over a window: messages delivered, total and
// max microseconds between enqueue (on the VI/gfx/task host thread) and the
// guest-side dequeue that actually attempts do_send. Reading RESETS the window.
//
// This is the number that tests the cooperative-delivery hypothesis: external
// messages are only delivered when a running guest thread reaches a scheduling
// point (osSendMesg/osRecvMesg) or when every guest thread is blocked and the
// idle path pumps. If delivery latency regularly spans a large fraction of a
// frame during gameplay, the guest's audio chain -- which needs one retrace
// delivery and one SP-done delivery per audio frame -- is quantized to the main
// thread's scheduling-point cadence, and that quantization is the trough.
void debug_external_delivery_window(uint64_t* count, uint64_t* total_us, uint64_t* max_us);
// Cumulative osGetTime calls -- see the counter in timer.cpp. Differencing this
// across a frame interval separates "stalled spinning on the clock" (tens of
// thousands) from "stalled doing work" (a handful).
uint64_t debug_osgettime_calls();

// ON BY DEFAULT at 1000us (HH_OSGETTIME_YIELD_US=<us> overrides; =0 disables and
// restores the pre-fix behaviour exactly): turn osGetTime into a throttled
// scheduling point.
//
// This is the fix for BOTH the periodic frametime hitch and the audio
// oscillation, measured in gameplay and confirmed by ear: 843 periodic 67ms
// spikes -> 0, audio a flat 60.0 tasks/s at `consumed` 1.000 with zero drops and
// zero empty-queue windows. It supersedes HH_AUDIO_INLINE_RSP, which addressed
// only the audio half and is now redundant.
//
// Hybrid Heaven's frame governor busy-waits on osGetTime for most of every frame
// without making a single blocking call, so the main thread -- the LOWEST
// priority guest thread -- holds execution while external messages (retrace,
// SP-done, DP-done) sit undelivered. Measured in gameplay: 7.93ms mean, 33.45ms
// worst. A retrace delayed that long delays the gfx dispatcher, which leaves the
// previous frame's task in flight, and the game skips its render when it polls
// sc+0x89C and finds it still set. Delivering (and yielding) from inside the
// spin is the direct test of that chain.
//
// Throttled because the spin calls osGetTime ~19M times/second; `now_counts` is
// the value osGetTime already computed, so the gate costs no extra clock read.
// May block: check_running_queue can swap to a higher-priority guest thread,
// exactly as osSendMesg/osRecvMesg already can.
void osgettime_scheduling_point(RDRAM_ARG uint64_t now_counts);
// How many times the above actually delivered, so a run can prove the switch
// engaged rather than inferring it from the env var being set.
uint64_t debug_osgettime_yields();

size_t debug_gfx_action_count();
size_t debug_timer_action_count();
size_t debug_sp_task_count();
size_t debug_deleted_thread_count();

// Thread scheduling.
void check_running_queue(RDRAM_ARG1);
void run_next_thread_and_wait(RDRAM_ARG1);
void resume_thread_and_wait(RDRAM_ARG OSThread* t);
void schedule_running_thread(RDRAM_ARG PTR(OSThread) t);
void cleanup_thread(UltraThreadContext* thread_context);
struct thread_terminated : std::exception {};

enum class ThreadPriority {
    Low,
    Normal,
    High,
    VeryHigh,
    Critical
};

void set_native_thread_name(const std::string& name);
void set_native_thread_priority(ThreadPriority pri);
PTR(OSThread) this_thread();
void set_main_thread();
bool is_game_thread();
void submit_rsp_task(RDRAM_ARG PTR(OSTask) task);
void send_si_message();
uint32_t get_speed_multiplier();

// Time
std::chrono::high_resolution_clock::time_point get_start();
std::chrono::high_resolution_clock::duration time_since_start();
void measure_input_latency();
void sleep_milliseconds(uint32_t millis);
void sleep_until(const std::chrono::high_resolution_clock::time_point& time_point);

// Graphics
uint32_t get_target_framerate(uint32_t original);
uint32_t get_display_refresh_rate();
float get_resolution_scale();
void trigger_config_action();

// Audio
void init_audio();
void set_audio_frequency(uint32_t freq);
void queue_audio_buffer(RDRAM_ARG PTR(s16) audio_data, uint32_t byte_count);
uint32_t get_remaining_audio_bytes();

struct audio_callbacks_t {
    using queue_samples_t = void(int16_t*, size_t);
    using get_samples_remaining_t = size_t();
    using set_frequency_t = void(uint32_t);
    queue_samples_t* queue_samples;
    get_samples_remaining_t* get_frames_remaining;
    set_frequency_t* set_frequency;
};

// TODO: Most of the members of this struct are not used by ultramodern. Should we move them to librecomp instead?
struct gfx_callbacks_t {
    using gfx_data_t = void*;
    using create_gfx_t = gfx_data_t();
    using create_window_t = renderer::WindowHandle(gfx_data_t);
    using update_gfx_t = void(gfx_data_t);

    create_gfx_t* create_gfx;
    create_window_t* create_window;
    update_gfx_t* update_gfx;
};

bool is_game_started();
void quit();

// App lifecycle pause gate (used on Android for background/foreground).
//
// set_app_paused(true) is called from the platform event pump when the app is
// backgrounded; the VI thread (the emulation heartbeat) blocks at a safe point
// so no further VI/AI messages are produced and the whole emulation quiesces.
// set_app_paused(false) wakes it. is_app_paused() is consulted by threads that
// must not run against a torn-down surface while backgrounded.
//
// quit() clears the pause and wakes any parked thread so shutdown never
// deadlocks on a paused VI thread.
void set_app_paused(bool paused);
bool is_app_paused();

// Set the global `exited` flag and wake the pause gate. quit() must use this
// (not a bare exited.store) so `exited` is written under the same mutex the
// pause predicate reads it under -- see set_exited_and_wake() in events.cpp.
void set_exited_and_wake();

void join_event_threads();
void join_thread_cleaner_thread();
void join_saving_thread();

void set_audio_callbacks(const audio_callbacks_t& callbacks);

/**
 * Register all the callbacks used by `ultramodern`, most of them being optional.
 *
 * The following arguments contain mandatory callbacks that need to be registered (i.e., can't be `nullptr`):
 * - `rsp_callbacks`
 * - `renderer_callbacks`
 *
 * It must be called only once and it must be called before `ultramodern::preinit`.
 */
void set_callbacks(
    const rsp::callbacks_t& rsp_callbacks,
    const renderer::callbacks_t& renderer_callbacks,
    const audio_callbacks_t& audio_callbacks,
    const input::callbacks_t& input_callbacks,
    const gfx_callbacks_t& gfx_callbacks,
    const events::callbacks_t& events_callbacks,
    const error_handling::callbacks_t& error_handling_callbacks,
    const threads::callbacks_t& threads_callbacks
);
} // namespace ultramodern

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define debug_printf(...)
//#define debug_printf(...) printf(__VA_ARGS__);

#endif
