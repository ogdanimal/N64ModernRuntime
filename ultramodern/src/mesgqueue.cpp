#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "blockingconcurrentqueue.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

struct QueuedMessage {
    PTR(OSMesgQueue) mq;
    OSMesg mesg;
    bool jam;
    bool requeue_if_blocked;
    // When the producing host thread enqueued this message. Delivery latency --
    // this timestamp against the moment a guest thread dequeues it -- is the
    // time an event (retrace, SP done, DP done) sat invisible to the guest
    // because no guest thread reached a scheduling point. See
    // debug_external_delivery_window in ultramodern.hpp for why that number is
    // load-bearing for the audio-cadence investigation.
    std::chrono::steady_clock::time_point enqueued_at;
};

static moodycamel::BlockingConcurrentQueue<QueuedMessage> external_messages {};

// Delivery-latency accounting, reset by each debug_external_delivery_window
// read. One clock read per message on each side, at an external-message rate of
// ~100-200/s -- orders of magnitude below anything this path could perturb.
static std::atomic<uint64_t> delivery_count{0};
static std::atomic<uint64_t> delivery_total_us{0};
static std::atomic<uint64_t> delivery_max_us{0};

static void note_delivery(const QueuedMessage& msg) {
    const uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - msg.enqueued_at).count();
    delivery_count.fetch_add(1, std::memory_order_relaxed);
    delivery_total_us.fetch_add(us, std::memory_order_relaxed);
    uint64_t prev = delivery_max_us.load(std::memory_order_relaxed);
    while (us > prev && !delivery_max_us.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {
    }
}

void ultramodern::debug_external_delivery_window(uint64_t* count, uint64_t* total_us, uint64_t* max_us) {
    *count = delivery_count.exchange(0, std::memory_order_relaxed);
    *total_us = delivery_total_us.exchange(0, std::memory_order_relaxed);
    *max_us = delivery_max_us.exchange(0, std::memory_order_relaxed);
}

void ultramodern::enqueue_external_message(PTR(OSMesgQueue) mq, OSMesg msg, bool jam, bool requeue_if_blocked) {
    external_messages.enqueue({mq, msg, jam, requeue_if_blocked, std::chrono::steady_clock::now()});
}

size_t ultramodern::debug_external_message_count() {
    return external_messages.size_approx();
}

// Cumulative count of external messages that were DISCARDED because the guest's
// queue was full and the sender asked not to requeue.
//
// debug_external_message_count() cannot answer this and was once cited as though
// it could. A message is popped by try_dequeue BEFORE do_send is attempted, so a
// drop leaves the queue exactly as empty as a delivery does -- a count of 0 is
// equally consistent with delivering everything and with dropping everything.
// This counter is incremented at the only place a message is actually lost.
static std::atomic<uint64_t> external_message_drops{0};

uint64_t ultramodern::debug_external_message_drops() {
    return external_message_drops.load(std::memory_order_relaxed);
}

bool do_send(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, bool jam, bool block);

// See the increment site in osSendMesg.
static std::atomic<uint64_t> guest_send_failures{0};

uint64_t ultramodern::debug_guest_send_failures() {
    return guest_send_failures.load(std::memory_order_relaxed);
}

void dequeue_external_messages(RDRAM_ARG1) {
    QueuedMessage to_send;
    std::vector<QueuedMessage> requeued_messages{};
    while (external_messages.try_dequeue(to_send)) {
        note_delivery(to_send);
        if (!do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false)) {
            if (to_send.requeue_if_blocked) {
                // Restamp so a message that waits out several requeue rounds
                // reports per-attempt latency, not its cumulative age.
                to_send.enqueued_at = std::chrono::steady_clock::now();
                requeued_messages.push_back(to_send);
            }
            else {
                // The message is gone here, and nothing else records it.
                external_message_drops.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    for (QueuedMessage& cur_mesg : requeued_messages) {
        external_messages.enqueue(cur_mesg);
    }
}

// --- osGetTime as a scheduling point. ON BY DEFAULT. See ultramodern.hpp. ---

// N64 CPU counter ticks per millisecond, matching timer.cpp's counter_per_ms.
// The guest's own conversion (cycles * 64 / 3000) agrees with this, so a
// threshold expressed here in microseconds means the same thing to both sides.
static constexpr uint64_t osgettime_counter_per_ms = 46'875;

// The shipped throttle. 1000us was chosen by sweeping 50us - 5ms: the ENTIRE
// range is equally effective (2-3 residual spikes against 352 with the feature
// off) while cost varies 93x, so this value is a cost decision and NOT a
// correctness one. Do not "tune" it chasing a regression -- if something breaks,
// the throttle is almost certainly not the variable. At 1000us it costs ~890-990
// engagements/s and still delivers external messages in ~0.3ms, ~56x inside the
// 16.7ms VI period that actually constrains it.
static constexpr long osgettime_yield_default_us = 1000;

// Throttle interval in counter ticks; 0 disables the feature entirely.
//
// `HH_OSGETTIME_YIELD_US` unset -> the default above (feature ON).
// `HH_OSGETTIME_YIELD_US=0`     -> OFF, restoring the pre-fix behaviour exactly.
// `HH_OSGETTIME_YIELD_US=<n>`   -> n microseconds.
//
// The 0 case exists so the old behaviour stays reachable on a shipped binary:
// this is the A/B for the whole fix, and without it the two coupled changes
// (delivery and yielding) could only be tested by rebuilding.
//
// Read once via a function-local static so the getenv happens exactly once and
// thread-safely, rather than on a path taken ~19M times a second.
static uint64_t osgettime_yield_interval() {
    static const uint64_t interval = [] () -> uint64_t {
        const char* setting = getenv("HH_OSGETTIME_YIELD_US");
        long us = osgettime_yield_default_us;
        if (setting != nullptr) {
            us = strtol(setting, nullptr, 10);
        }
        if (us <= 0) {
            return 0;
        }
        return (static_cast<uint64_t>(us) * osgettime_counter_per_ms) / 1000;
    }();
    return interval;
}

static std::atomic<uint64_t> osgettime_last_yield{0};
static std::atomic<uint64_t> osgettime_yields{0};

uint64_t ultramodern::debug_osgettime_yields() {
    return osgettime_yields.load(std::memory_order_relaxed);
}

void ultramodern::osgettime_scheduling_point(RDRAM_ARG uint64_t now_counts) {
    const uint64_t interval = osgettime_yield_interval();
    if (interval == 0) {
        return;
    }
    // Externals may only be delivered from a game thread -- do_send touches
    // guest queue state that non-game threads deliberately never touch (see the
    // is_game_thread guard in osSendMesg).
    if (!ultramodern::is_game_thread()) {
        return;
    }

    uint64_t last = osgettime_last_yield.load(std::memory_order_relaxed);
    // Unsigned wrap is not a concern: the counter starts at 0 at program start
    // and 64 bits of 46.875MHz is ~12000 years.
    if (now_counts - last < interval) {
        return;
    }
    // One winner per interval; a loser this round simply tries again next call,
    // microseconds later. Not a correctness question, only a rate limit.
    if (!osgettime_last_yield.compare_exchange_strong(last, now_counts, std::memory_order_relaxed)) {
        return;
    }

    osgettime_yields.fetch_add(1, std::memory_order_relaxed);
    dequeue_external_messages(PASS_RDRAM1);
    // Delivering is not enough on its own. ultramodern emulates N64 priority
    // scheduling, so a higher-priority thread made runnable by the delivery
    // above does not run until the current thread yields to it -- and the
    // spinning main thread is the lowest priority thread in the game.
    ultramodern::check_running_queue(PASS_RDRAM1);
}

void ultramodern::wait_for_external_message(RDRAM_ARG1) {
    QueuedMessage to_send;
    external_messages.wait_dequeue(to_send);
    note_delivery(to_send);
    if (!do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false) && to_send.requeue_if_blocked) {
        to_send.enqueued_at = std::chrono::steady_clock::now();
        external_messages.enqueue(to_send);
    }
}

void ultramodern::wait_for_external_message_timed(RDRAM_ARG u32 millis) {
    QueuedMessage to_send;
    if (external_messages.wait_dequeue_timed(to_send, std::chrono::milliseconds{millis})) {
        note_delivery(to_send);
        if (!do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false) && to_send.requeue_if_blocked) {
            to_send.enqueued_at = std::chrono::steady_clock::now();
            external_messages.enqueue(to_send);
        }
    }
}

extern "C" void osCreateMesgQueue(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg, s32 count) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    mq->blocked_on_recv = NULLPTR;
    mq->blocked_on_send = NULLPTR;
    mq->msgCount = count;
    mq->msg = msg;
    mq->validCount = 0;
    mq->first = 0;
}

s32 MQ_GET_COUNT(OSMesgQueue *mq) {
    return mq->validCount;
}

s32 MQ_IS_EMPTY(OSMesgQueue *mq) {
    return mq->validCount == 0;
}

s32 MQ_IS_FULL(OSMesgQueue* mq) {
    return MQ_GET_COUNT(mq) >= mq->msgCount;
}

// HH_TRACE_MESG_BLOCK=1 logs every blocking transition on a guest message queue
// with a steady-clock timestamp and the guest thread id. The point is stall
// forensics: a periodic frametime spike whose FrameProbe decomposition is all
// `idle` means the guest stopped submitting, and this log shows -- at the
// moment of the stall -- which thread was blocked on which queue and which
// message broke the stall. ~hundreds of lines a second, so gated and off by
// default; analyze offline around the gaps.
static bool trace_mesg_block() {
    static const bool on = getenv("HH_TRACE_MESG_BLOCK") != nullptr;
    return on;
}

static double trace_secs() {
    static const auto epoch = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch).count();
}

bool do_send(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, bool jam, bool block) {
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, mq_);
    if (!block) {
        // If non-blocking, fail if the queue is full.
        if (MQ_IS_FULL(mq)) {
            return false;
        }
    }
    else {
        // Otherwise, yield this thread until the queue has room.
        bool blocked = false;
        while (MQ_IS_FULL(mq)) {
            if (trace_mesg_block() && !blocked) {
                blocked = true;
                fprintf(stderr, "[mq] t=%9.4f thr %2d blocks SEND on %08X\n",
                        trace_secs(), TO_PTR(OSThread, ultramodern::this_thread())->id, (uint32_t)mq_);
            }
            debug_printf("[Message Queue] Thread %d is blocked on send\n", TO_PTR(OSThread, ultramodern::this_thread())->id);
            ultramodern::thread_queue_insert(PASS_RDRAM GET_MEMBER(OSMesgQueue, mq_, blocked_on_send), ultramodern::this_thread());
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
        }
        if (trace_mesg_block() && blocked) {
            fprintf(stderr, "[mq] t=%9.4f thr %2d wakes  SEND on %08X\n",
                    trace_secs(), TO_PTR(OSThread, ultramodern::this_thread())->id, (uint32_t)mq_);
        }
    }
    
    if (jam) {
        // Jams insert at the head of the message queue's buffer.
        mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
        TO_PTR(OSMesg, mq->msg)[mq->first] = msg;
        mq->validCount++;
    }
    else {
        // Sends insert at the tail of the message queue's buffer.
        s32 last = (mq->first + mq->validCount) % mq->msgCount;
        TO_PTR(OSMesg, mq->msg)[last] = msg;
        mq->validCount++;
    }

    // If any threads were blocked on receiving from this message queue, pop the first one and schedule it.
    PTR(PTR(OSThread)) blocked_queue = GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv);
    if (!ultramodern::thread_queue_empty(PASS_RDRAM blocked_queue)) {
        ultramodern::schedule_running_thread(PASS_RDRAM ultramodern::thread_queue_pop(PASS_RDRAM blocked_queue));
    }
    
    return true;
}

bool do_recv(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg_, bool block) {
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, mq_);
    if (!block) {
        // If non-blocking, fail if the queue is empty
        if (MQ_IS_EMPTY(mq)) {
            return false;
        }
    } else {
        // Otherwise, yield this thread in a loop until the queue is no longer full
        bool blocked = false;
        while (MQ_IS_EMPTY(mq)) {
            if (trace_mesg_block() && !blocked) {
                blocked = true;
                fprintf(stderr, "[mq] t=%9.4f thr %2d blocks RECV on %08X\n",
                        trace_secs(), TO_PTR(OSThread, ultramodern::this_thread())->id, (uint32_t)mq_);
            }
            debug_printf("[Message Queue] Thread %d is blocked on receive\n", TO_PTR(OSThread, ultramodern::this_thread())->id);
            ultramodern::thread_queue_insert(PASS_RDRAM GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv), ultramodern::this_thread());
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
        }
        if (trace_mesg_block() && blocked) {
            fprintf(stderr, "[mq] t=%9.4f thr %2d wakes  RECV on %08X msg %08X\n",
                    trace_secs(), TO_PTR(OSThread, ultramodern::this_thread())->id, (uint32_t)mq_,
                    (uint32_t)(uintptr_t)TO_PTR(OSMesg, mq->msg)[mq->first]);
        }
    }

    if (msg_ != NULLPTR) {
        *TO_PTR(OSMesg, msg_) = TO_PTR(OSMesg, mq->msg)[mq->first];
    }
    
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;

    // If any threads were blocked on sending to this message queue, pop the first one and schedule it.
    PTR(PTR(OSThread)) blocked_queue = GET_MEMBER(OSMesgQueue, mq_, blocked_on_send);
    if (!ultramodern::thread_queue_empty(PASS_RDRAM blocked_queue)) {
        ultramodern::schedule_running_thread(PASS_RDRAM ultramodern::thread_queue_pop(PASS_RDRAM blocked_queue));
    }

    return true;
}

extern "C" s32 osSendMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    bool jam = false;
    
    // Don't directly send to the message queue if this isn't a game thread to avoid contention.
    if (!ultramodern::is_game_thread()) {
        ultramodern::enqueue_external_message(mq_, msg, jam, false);
        return 0;
    }
    
    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // Try to send the message.
    bool sent = do_send(PASS_RDRAM mq_, msg, jam, flags == OS_MESG_BLOCK);

    // A guest osSendMesg that FAILS is the guest losing its own message, which
    // is invisible everywhere else -- the game just gets -1 back and, in the
    // OS_MESG_NOBLOCK case, usually ignores it. It is the last place an event
    // can vanish between a retrace the port delivered and an audio frame the
    // game never ran, so it is counted rather than inferred.
    if (!sent) {
        const uint64_t n = guest_send_failures.fetch_add(1, std::memory_order_relaxed) + 1;
        // Which queue is rejecting matters as much as how many: the count alone
        // cannot tell an audio-client notification being lost from any other
        // guest send failing. Logged on powers of two so a standing condition is
        // visible without burying the run, and only when asked.
        if (getenv("HH_TRACE_SEND_FAIL") != nullptr && (n & (n - 1)) == 0) {
            fprintf(stderr, "[mesg] guest osSendMesg FAILED #%llu: queue %08X full (%d/%d), msg %08X\n",
                    (unsigned long long)n, (uint32_t)mq_, mq->validCount, mq->msgCount,
                    (uint32_t)(uintptr_t)msg);
        }
    }
    
    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return sent ? 0 : -1;
}

extern "C" s32 osJamMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    bool jam = true;
    
    // Don't directly send to the message queue if this isn't a game thread to avoid contention.
    if (!ultramodern::is_game_thread()) {
        ultramodern::enqueue_external_message(mq_, msg, jam, false);
        return 0;
    }
    
    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // Try to send the message.
    bool sent = do_send(PASS_RDRAM mq_, msg, jam, flags == OS_MESG_BLOCK);

    // A guest osSendMesg that FAILS is the guest losing its own message, which
    // is invisible everywhere else -- the game just gets -1 back and, in the
    // OS_MESG_NOBLOCK case, usually ignores it. It is the last place an event
    // can vanish between a retrace the port delivered and an audio frame the
    // game never ran, so it is counted rather than inferred.
    if (!sent) {
        const uint64_t n = guest_send_failures.fetch_add(1, std::memory_order_relaxed) + 1;
        // Which queue is rejecting matters as much as how many: the count alone
        // cannot tell an audio-client notification being lost from any other
        // guest send failing. Logged on powers of two so a standing condition is
        // visible without burying the run, and only when asked.
        if (getenv("HH_TRACE_SEND_FAIL") != nullptr && (n & (n - 1)) == 0) {
            fprintf(stderr, "[mesg] guest osSendMesg FAILED #%llu: queue %08X full (%d/%d), msg %08X\n",
                    (unsigned long long)n, (uint32_t)mq_, mq->validCount, mq->msgCount,
                    (uint32_t)(uintptr_t)msg);
        }
    }
    
    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return sent ? 0 : -1;
}

extern "C" s32 osRecvMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg_, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    
    assert(ultramodern::is_game_thread() && "RecvMesg not allowed outside of game threads.");
    
    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // Try to receive a message.
    bool received = do_recv(PASS_RDRAM mq_, msg_, flags == OS_MESG_BLOCK);
    
    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return received ? 0 : -1;
}
