#include <atomic>
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
};

static moodycamel::BlockingConcurrentQueue<QueuedMessage> external_messages {};

void ultramodern::enqueue_external_message(PTR(OSMesgQueue) mq, OSMesg msg, bool jam, bool requeue_if_blocked) {
    external_messages.enqueue({mq, msg, jam, requeue_if_blocked});
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
        if (!do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false)) {
            if (to_send.requeue_if_blocked) {
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

void ultramodern::wait_for_external_message(RDRAM_ARG1) {
    QueuedMessage to_send;
    external_messages.wait_dequeue(to_send);
    if (!do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false) && to_send.requeue_if_blocked) {
        external_messages.enqueue(to_send);
    }
}

void ultramodern::wait_for_external_message_timed(RDRAM_ARG u32 millis) {
    QueuedMessage to_send;
    if (external_messages.wait_dequeue_timed(to_send, std::chrono::milliseconds{millis})) {
        if (!do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false) && to_send.requeue_if_blocked) {
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
        while (MQ_IS_FULL(mq)) {
            debug_printf("[Message Queue] Thread %d is blocked on send\n", TO_PTR(OSThread, ultramodern::this_thread())->id);
            ultramodern::thread_queue_insert(PASS_RDRAM GET_MEMBER(OSMesgQueue, mq_, blocked_on_send), ultramodern::this_thread());
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
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
        while (MQ_IS_EMPTY(mq)) {
            debug_printf("[Message Queue] Thread %d is blocked on receive\n", TO_PTR(OSThread, ultramodern::this_thread())->id);
            ultramodern::thread_queue_insert(PASS_RDRAM GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv), ultramodern::this_thread());
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
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
