// Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#pragma once

#include <deque>
#include <unordered_map>
#include "src/core/model_config.h"
#include "src/core/scheduler.h"
#include "src/core/server_status.h"

namespace nvidia { namespace inferenceserver {

using PendingBatchShapes =
    std::unordered_map<std::string, std::pair<DimsList, std::vector<int64_t>>>;

Status InitPendingShape(
    const int64_t runner_id, const Scheduler::Payload& payload,
    const std::unordered_map<std::string, bool>& enforce_equal_shape_tensors,
    const Scheduler::StandardShapeTensorPeekFunc& OnPeek,
    PendingBatchShapes* pending_batch_shapes);

bool CompareWithPendingShape(
    const int64_t runner_id, const Scheduler::Payload& payload,
    const Scheduler::StandardShapeTensorPeekFunc& OnPeek,
    const PendingBatchShapes& pending_batch_shapes);

using ModelQueuePolicyMap =
    ::google::protobuf::Map<::google::protobuf::uint32, ModelQueuePolicy>;

class PriorityQueue {
 public:
  PriorityQueue();

  PriorityQueue(
      const ModelQueuePolicy& default_queue_policy, uint32_t priority_levels,
      const ModelQueuePolicyMap queue_policy_map);

  Status Enqueue(uint32_t priority_level, Scheduler::Payload&& payload);

  Scheduler::Payload Dequeue();

  std::vector<std::deque<Scheduler::Payload>> ReleaseRejectedPayloads();

  size_t Size() { return size_; }

  bool Empty() { return Size() == 0; }

  // Reset the cursor such that it is pointing to the first request
  // in the queue. The resulting cursor will be invalid if the
  // queue is empty.
  void ResetCursor()
  {
    pending_cursor_ = Cursor(queues_.begin(), --queues_.end(), &size_);
  }

  // Record the current cursor. The cursor can be restored to recorded state
  // by invoking SetCursorToMark(). Note that Enqueue(), Dequeue(), and
  // ResetCursor() will invalidate the marker, it is the function caller's
  // responsibility to ensure the marker is valid before calling
  // SetCursorToMark().
  void MarkCursor() { current_mark_ = pending_cursor_; }

  // Apply the queue policy and alter the underlying queue accordingly. After
  // the function returns, the cursor may be at its end to indicate that
  // there no request after the pending batch.
  // Returns the total batch size of the newly rejected requests.
  size_t ApplyPolicyAtCursor();

  // Return the payload at cursor.
  Scheduler::Payload& PayloadAtCursor() { return pending_cursor_.GetItem(); }

  // Advance the cursor for pending batch. This function will not trigger the
  // queue policy. No effect if the cursor already reach the end of the queue.
  void AdvanceCursor() { pending_cursor_.Next(); }

  bool CursorEnd() { return pending_cursor_.pending_batch_count_ == size_; }


  void SetCursorToMark() { pending_cursor_ = current_mark_; }

  bool IsCursorValid();

  uint64_t OldestEnqueueTime()
  {
    return pending_cursor_.pending_batch_oldest_enqueue_time_ns_;
  }

 private:
  class PolicyQueue {
   public:
    PolicyQueue()
        : timeout_action_(ModelQueuePolicy::REJECT), default_timeout_ms_(0),
          allow_timeout_override_(false), max_queue_size_(0)
    {
    }

    PolicyQueue(const ModelQueuePolicy& policy)
        : timeout_action_(policy.timeout_action()),
          default_timeout_ms_(policy.default_timeout_microseconds()),
          allow_timeout_override_(policy.allow_timeout_override()),
          max_queue_size_(policy.max_queue_size())
    {
    }

    // Enqueue an payload and set up its timeout accordingly.
    Status Enqueue(Scheduler::Payload&& payload);

    // Dequeue the payload at the front of the queue.
    Scheduler::Payload Dequeue();

    // Apply the queue policy to payload at 'idx'.
    // 'rejected_count' will be incremented by the number of the newly rejected
    // requets after applying the policy.
    // 'rejected_batch_size' will be incremented by the total batch size of the
    // newly rejected requets after applying the policy.
    // Return true if the 'idx' still points to an payload after applying the
    // policy, false otherwise.
    bool ApplyPolicy(
        size_t idx, size_t* rejected_count, size_t* rejected_batch_size);

    // Return the rejected payloads held by the request queue.
    std::deque<Scheduler::Payload> ReleaseRejectedQueue();

    // Return the payload at 'idx'.
    Scheduler::Payload& At(size_t idx);

    // Return the timeout timestamp of the payload at 'idx', in ns. A value of 0
    // indicates that the payload doesn't specify a timeout.
    uint64_t TimeoutAt(size_t idx);

    // Return whether the queue is empty, rejected requests are not included.
    bool Empty() { return Size() == 0; }

    // Return the number of requests in the queue, rejected requests are not
    // included.
    size_t Size() { return queue_.size() + delayed_queue_.size(); }

   private:
    // Variables that define the policy for the queue
    const ModelQueuePolicy::TimeoutAction timeout_action_;
    const uint64_t default_timeout_ms_;
    const bool allow_timeout_override_;
    const uint32_t max_queue_size_;

    std::deque<uint64_t> timeout_timestamp_ns_;
    std::deque<Scheduler::Payload> queue_;
    std::deque<Scheduler::Payload> delayed_queue_;
    std::deque<Scheduler::Payload> rejected_queue_;
  };
  using PriorityQueues = std::map<uint32_t, PolicyQueue>;

  // Cursor which points to the item after the pending batch
  struct Cursor {
    Cursor() = default;
    Cursor(PriorityQueues::iterator start_it, PriorityQueues::iterator last_it,
    size_t* queue_size);

    Cursor(const Cursor& rhs)
        : queue_size_(rhs.queue_size_),
        curr_it_(rhs.curr_it_), last_it_(rhs.last_it_),
          queue_idx_(rhs.queue_idx_),
          pending_batch_closest_timeout_ns_(
              rhs.pending_batch_closest_timeout_ns_),
          pending_batch_oldest_enqueue_time_ns_(
              rhs.pending_batch_oldest_enqueue_time_ns_),
          valid_(rhs.valid_)
    {
    }

    Scheduler::Payload& GetItem() { return curr_it_->second.At(queue_idx_); }

    void Next();

    // Reference to the priority queue size, so that the cursor itself knows
    // whether it is at the end of the queue (pending batch covers the whole
    // queue)
    size_t* queue_size_;
    PriorityQueues::iterator curr_it_;
    PriorityQueues::iterator last_it_;
    size_t queue_idx_;
    uint64_t pending_batch_closest_timeout_ns_;
    uint64_t pending_batch_oldest_enqueue_time_ns_;
    size_t pending_batch_count_;
    bool valid_;
  };

  PriorityQueues queues_;
  size_t size_;

  Cursor pending_cursor_;
  Cursor current_mark_;
};

}}  // namespace nvidia::inferenceserver
