/* Copyright 2017-2018 All Rights Reserved.
 *  Gyeonghwan Hong (redcarrottt@gmail.com)
 *  Eunsoo Park (esevan.park@gmail.com)
 *
 * [Contact]
 *  Gyeonghwan Hong (redcarrottt@gmail.com)
 *
 * Licensed under the Apache License, Version 2.0(the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "../inc/SegmentManager.h"

#include "../inc/ProtocolManager.h"

#include "../../common/inc/DebugLog.h"

#include "../../configs/ExpConfig.h"

#include <list>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

using namespace sc;

/* Singleton */
SegmentManager *SegmentManager::sSingleton = NULL;

uint32_t SegmentManager::get_next_seq_no(SegSeqNumType seq_num_type,
                                         uint32_t num_segments) {
  uint32_t res;
  this->mNextSeqNoLock[seq_num_type].lock();
  res = this->mNextSeqNo[seq_num_type];
  this->mNextSeqNo[seq_num_type] += num_segments;
  this->mNextSeqNoLock[seq_num_type].unlock();
  return res;
}

void SegmentManager::serialize_segment_header(Segment *seg) {
  uint32_t net_seq_no = htonl(seg->seq_no);
  uint32_t net_len = htonl(seg->len);
  uint32_t net_flag = htonl(seg->flag);
  int32_t net_send_start_ts_sec = htonl(seg->send_start_ts_sec);
  int32_t net_send_start_ts_usec = htonl(seg->send_start_ts_usec);
  memcpy(seg->data, &net_seq_no, sizeof(uint32_t));
  memcpy(seg->data + 4, &net_len, sizeof(uint32_t));
  memcpy(seg->data + 8, &net_flag, sizeof(uint32_t));
  memcpy(seg->data + 12, &net_send_start_ts_sec, sizeof(int32_t));
  memcpy(seg->data + 16, &net_send_start_ts_usec, sizeof(int32_t));
}

int SegmentManager::send_to_segment_manager(uint8_t *data, size_t len,
                                            bool is_control) {
  assert(data != NULL && len > 0);

  uint32_t offset = 0;
  uint32_t num_of_segments = ((len + kSegSize - 1) / kSegSize);
  assert((len + kSegSize - 1) / kSegSize < UINT32_MAX);

  SegSeqNumType seq_num_type = (is_control) ? kSNControl : kSNData;

  /* Reserve sequence numbers to this thread */
  uint32_t allocated_seq_no = get_next_seq_no(seq_num_type, num_of_segments);

  struct timeval send_start_ts;
  gettimeofday(&send_start_ts, NULL);

  int seg_idx;
  for (seg_idx = 0; seg_idx < num_of_segments; seg_idx++) {
    uint32_t seg_len = (len - offset < kSegSize) ? len - offset : kSegSize;
    Segment *seg = this->pop_free_segment();

    /* Set segment sequence number */
    seg->seq_no = allocated_seq_no++;

    /* Set segment length */
    seg->len = seg_len;

    /* Set segment MF flag and/or control segment flag */
    uint32_t flag = 0;
    if (offset + seg_len < len)
      flag = flag | kSegFlagMF;
    if (is_control)
      flag = flag | kSegFlagControl;
    seg->flag = flag;

    /* Set segment enqueued timestamp */
    seg->send_start_ts_sec = (int)send_start_ts.tv_sec;
    seg->send_start_ts_usec = (int)send_start_ts.tv_usec;

    /* Set segment header to data */
    this->serialize_segment_header(seg);

    /* Set segment data */
    memcpy(seg->data + kSegHeaderSize, data + offset, seg_len);
    offset += seg_len;

#ifdef VERBOSE_ENQUEUE_SEND
    LOG_DEBUG(
        "Enqueue(control) IsControl: %d / SeqNo: %lu / len: %d / TS: %ld.%ld",
        is_control, seg->seq_no, len, send_start_ts.tv_sec,
        send_start_ts.tv_usec);
#endif
    sc::SegQueueType queueType = kSQSendData;
#ifdef EXP_CONTROL_SEGQUEUE
    if (is_control) {
      queueType = kSQSendControl;
    }
#endif

    this->enqueue(queueType, seg);
  }

  return 0;
}

uint8_t *SegmentManager::recv_from_segment_manager(void *proc_data_handle,
                                                   bool is_control) {
  assert(proc_data_handle != NULL);

  ProtocolData *pd = reinterpret_cast<ProtocolData *>(proc_data_handle);
  uint8_t *serialized = NULL;
  uint16_t offset = 0;
  size_t data_size = 0;
  bool cont = false;
  bool dequeued = false;
  Segment *seg;

  while (dequeued == false) {
    if (is_control) {
      seg = dequeue(kDeqRecvControl);
    } else {
      seg = dequeue(kDeqRecvData);
    }
    if (seg) {
      dequeued = true;
    }
  }

  ProtocolManager::parse_header(&(seg->data[kSegHeaderSize]), pd);

  if (unlikely(pd->len == 0))
    return NULL;

  serialized = reinterpret_cast<uint8_t *>(calloc(pd->len, sizeof(uint8_t)));

  data_size = seg->len - kProtHeaderSize;
  memcpy(serialized + offset, &(seg->data[kSegHeaderSize]) + kProtHeaderSize,
         data_size);
  offset += data_size;

  cont = ((seg->flag & kSegFlagMF) != 0);

  add_free_segment_to_list(seg);

  while (cont) {
    if (is_control) {
      seg = dequeue(kDeqRecvControl);
    } else {
      seg = dequeue(kDeqRecvData);
    }
    data_size = seg->len;
    memcpy(serialized + offset, &(seg->data[kSegHeaderSize]), data_size);
    cont = ((seg->flag & kSegFlagMF) != 0);
    offset += data_size;
    add_free_segment_to_list(seg);
  }

  return serialized;
}

SegDequeueType
SegmentManager::queue_type_to_dequeue_type(SegQueueType queue_type) {
  SegDequeueType dequeue_type;
  switch (queue_type) {
  case SegQueueType::kSQRecvControl:
    dequeue_type = SegDequeueType::kDeqRecvControl;
    break;
  case SegQueueType::kSQRecvData:
    dequeue_type = SegDequeueType::kDeqRecvData;
    break;
  case SegQueueType::kSQSendControl:
  case SegQueueType::kSQSendData:
    dequeue_type = SegDequeueType::kDeqSendControlData;
    break;
  default:
    LOG_ERR("Unknown queue type: %d", queue_type);
    dequeue_type = SegDequeueType::kDeqUnknown;
    break;
  }
  return dequeue_type;
}

/*
 * This function is the end of the sending logic.
 * It enqeueus the data to the sending queue in order with the sequence number.
 */
void SegmentManager::enqueue(SegQueueType queue_type, Segment *seg) {
  assert(queue_type < kNumSQ);
  SegDequeueType dequeue_type = queue_type_to_dequeue_type(queue_type);
  assert(dequeue_type < kNumDeq);

  // Dequeue Lock
  std::unique_lock<std::mutex> dequeue_lock(this->mDequeueLock[dequeue_type]);

  // Enqueue the segment to the corresponding segment queue
  // If the segment has already been enqueued, the segment can be ignored.
  bool continuous_segment_enqueued =
      this->mSegmentQueues[queue_type].enqueue(seg);

  // If enqueue is successful, wake up threads that wait for dequeue
  if (continuous_segment_enqueued) {
    this->wake_up_dequeue_waiting(dequeue_type);
  }

  // Update statistics
  if (queue_type == kSQSendControl || queue_type == kSQSendData) {
    this->mSendRequest.add(SEGMENT_DATA_SIZE);
  }

  // Check if all the remaining segments are received
  // It is used for the pre-condition of disconnection
  this->check_receiving_done();
}

void SegmentManager::wait_for_dequeue_locked(
    SegDequeueType dequeue_type, std::unique_lock<std::mutex> &dequeue_lock) {
  /* If queue is empty, wait until some segment is enqueued */
  bool is_wait_required = false;
  switch (dequeue_type) {
  case SegDequeueType::kDeqSendControlData: {
    int send_control_queue_length =
        this->mSegmentQueues[SegQueueType::kSQSendControl].get_queue_length();
    int send_data_queue_length =
        this->mSegmentQueues[SegQueueType::kSQSendData].get_queue_length();
    is_wait_required =
        ((send_control_queue_length == 0) && (send_data_queue_length == 0));
    break;
  }
  case SegDequeueType::kDeqRecvControl: {
    int recv_control_queue_length =
        this->mSegmentQueues[SegQueueType::kSQRecvControl].get_queue_length();
    is_wait_required = (recv_control_queue_length == 0);
    break;
  }
  case SegDequeueType::kDeqRecvData: {
    int recv_data_queue_length =
        this->mSegmentQueues[SegQueueType::kSQRecvData].get_queue_length();
    is_wait_required = (recv_data_queue_length == 0);
    break;
  }
  }
  if (is_wait_required) {
#ifdef VERBOSE_SEGMENT_QUEUE_WAITING
    if (dequeue_type == kDeqSendControlData) {
      LOG_DEBUG("sending queue is empty. wait for another");
    } else {
      LOG_DEBUG("receiving queue is empty. wait for another");
    }
#endif

    this->mDequeueCond[dequeue_type].wait(dequeue_lock);
  }
}

SegQueueType
SegmentManager::get_target_queue_type(SegDequeueType dequeue_type) {
  SegQueueType target_queue_type = SegQueueType::kSQUnknown;
  switch (dequeue_type) {
  case SegDequeueType::kDeqSendControlData: {
    if (this->mSegmentQueues[SegQueueType::kSQSendControl].get_queue_length() !=
        0) {
      // Priority 1. Dequeue send control queue
      target_queue_type = kSQSendControl;
    } else if (this->mSegmentQueues[SegQueueType::kSQSendData]
                   .get_queue_length() != 0) {
      // Priority 2. Dequeue send data queue
      target_queue_type = kSQSendData;
    } else {
      // No segments in both two send queues
      break;
    }
    break;
  }
  case SegDequeueType::kDeqRecvControl: {
    target_queue_type = kSQRecvControl;
    break;
  }
  case SegDequeueType::kDeqRecvData: {
    target_queue_type = kSQRecvData;
    break;
  }
  default: {
    LOG_ERR("Invalid dequeue type (dequeue=%d)", dequeue_type);
    break;
  }
  }
  return target_queue_type;
}

/*
 * Dequeue the segment from the queue.
 * Note that this function is used for sending & receiving queue.
 */
Segment *SegmentManager::dequeue(SegDequeueType dequeue_type) {
  assert(dequeue_type < kNumDeq);
  std::unique_lock<std::mutex> dequeue_lock(this->mDequeueLock[dequeue_type]);

  // Wait for dequeue
  this->wait_for_dequeue_locked(dequeue_type, dequeue_lock);

  // Dequeue from queue
  SegQueueType target_queue_type = get_target_queue_type(dequeue_type);
  if (target_queue_type >= kNumSQ) {
    // It is not failure situation.
    return NULL;
  }

  // Check queue type
  if (target_queue_type >= SegQueueType::kNumSQ) {
    LOG_ERR("Dequeue failed: invalid queue type (dequeue=%d, queue=%d)",
            dequeue_type, target_queue_type);
    return NULL;
  }

  // Check the dequeued segment
  Segment *segment_dequeued = this->mSegmentQueues[target_queue_type].dequeue();
  if (segment_dequeued == NULL) {
    LOG_DEBUG("Dequeue interrupted: empty queue (queue=%d, dequeue=%d)",
              target_queue_type, dequeue_type);
  }

  return segment_dequeued;
}

/*  This function returns a free segment from the list of free segments.
 *  If there is no one available, allocate new one
 */
Segment *SegmentManager::pop_free_segment(void) {
  std::unique_lock<std::mutex> lck(this->mFreeSegmentListLock);
  Segment *ret = NULL;
  if (this->mFreeSegmentListSize == 0) {
    ret = reinterpret_cast<Segment *>(calloc(1, sizeof(Segment)));
  } else {
    ret = this->mFreeList.front();
    this->mFreeList.pop_front();
    this->mFreeSegmentListSize--;
  }

  ret->seq_no = 0;
  ret->len = 0;
  ret->flag = 0;
  assert(ret != NULL);
  return ret;
}

void SegmentManager::shrink_free_segment_list(uint32_t threshold) {
  while (this->mFreeSegmentListSize > threshold) {
    Segment *to_free = this->mFreeList.front();
    this->mFreeList.pop_front();
    free(to_free);
    this->mFreeSegmentListSize--;
  }
}

/*  This function releases the segment which is not further used.
 */
void SegmentManager::add_free_segment_to_list(Segment *seg) {
  std::unique_lock<std::mutex> lck(this->mFreeSegmentListLock);
  this->mFreeList.push_front(seg);
  this->mFreeSegmentListSize++;

  // If the free list is big enough, release the half elements of the list
  if (unlikely(this->mFreeSegmentListSize > kSegFreeThreshold)) {
    shrink_free_segment_list(kSegFreeThreshold / 2);
  }
}

void SegmentManager::deallocate_all_the_free_segments(void) {
  std::unique_lock<std::mutex> lck(this->mFreeSegmentListLock);
  shrink_free_segment_list(0);
}

/* Sending-failed segment handling */
void SegmentManager::add_failed_segment_to_list(Segment *seg) {
  std::unique_lock<std::mutex> lck(this->mFailedSegmentListLock);
  this->mFailedSegmentList.push_back(seg);
  this->mFailedSegmentListLength.increase();
}

Segment *SegmentManager::pop_failed_segment(void) {
  std::unique_lock<std::mutex> lck(this->mFailedSegmentListLock);
  if (this->mFailedSegmentList.size() == 0)
    return NULL;

  Segment *res = this->mFailedSegmentList.front();
  this->mFailedSegmentList.pop_front();
  this->mFailedSegmentListLength.decrease();

  return res;
}

void SegmentManager::deallocate_sent_segments_by_peer(
    uint32_t last_seq_no_control, uint32_t last_seq_no_data) {
  int dealloc_control_num = 0;
  int dealloc_data_num = 0;
  // Control segment list
  if (last_seq_no_control >= 0) {
    std::unique_lock<std::mutex> lck(this->mSentSegmentListLocks[kSNControl]);
    std::list<Segment *>::iterator it =
        this->mSentSegmentLists[kSNControl].begin();
    while (it != this->mSentSegmentLists[kSNControl].end()) {
      Segment *segment = (*it);
      int seq_no = segment->seq_no;
      if (seq_no <= last_seq_no_control) {
        this->add_free_segment_to_list(segment);
        it = this->mSentSegmentLists[kSNControl].erase(it);
        dealloc_control_num++;
      } else {
        it++;
      }
    }
  }

  // Data segment list
  if (last_seq_no_data >= 0) {
    std::unique_lock<std::mutex> lck(this->mSentSegmentListLocks[kSNData]);
    std::list<Segment *>::iterator it =
        this->mSentSegmentLists[kSNData].begin();
    while (it != this->mSentSegmentLists[kSNData].end()) {
      Segment *segment = (*it);
      int seq_no = segment->seq_no;
      if (seq_no <= last_seq_no_data) {
        this->add_free_segment_to_list(segment);
        it = this->mSentSegmentLists[kSNData].erase(it);
        dealloc_data_num++;
      } else {
        it++;
      }
    }
  }

  // If the free list is big enough, release the half elements of the list
  if (unlikely(this->mFreeSegmentListSize > kSegFreeThreshold)) {
    shrink_free_segment_list(kSegFreeThreshold / 2);
  }

#ifdef VERBOSE_DEALLOCATE_SENT_SEGMENTS
  LOG_DEBUG("Deallocated sent segments: %d ctrl segs + %d data segs",
            dealloc_control_num, dealloc_data_num);
#endif
}

/* Retransmission */
void SegmentManager::request_retransmit_missing_segments(void) {
  // Not implemented yet
}

void SegmentManager::retransmit_missing_segments_by_peer(int seg_type,
                                                         uint32_t seq_no_start,
                                                         uint32_t seq_no_end) {
  if (seg_type < 0 || seg_type >= kNumSN) {
    // Invalid seg_type value
    return;
  }

  int num_retransmitted_segments = 0;
  {
    std::unique_lock<std::mutex> lck(this->mSentSegmentListLocks[seg_type]);
    std::list<Segment *> &sent_segment_list = this->mSentSegmentLists[seg_type];
    for (std::list<Segment *>::iterator it = sent_segment_list.begin();
         it != sent_segment_list.end(); it++) {
      Segment *segment = (*it);
      int seq_no = segment->seq_no;
      if (seq_no >= seq_no_start && seq_no <= seq_no_end) {
        num_retransmitted_segments++;
        this->add_failed_segment_to_list(segment);
      }
    }
  }
  int num_requested_segments = seq_no_end - seq_no_start + 1;
  if (num_retransmitted_segments != num_requested_segments) {
    LOG_WARN("Failed to retransmit segments: sent %d != requested "
             "%d(seq_no:%lu~%lu)",
             num_retransmitted_segments, num_requested_segments, seq_no_start,
             seq_no_end);
  } else {
    LOG_ERR("Retransmit segments: requested %d(seq_no:%lu~%lu)",
            num_retransmitted_segments, seq_no_start, seq_no_end);
  }
}

void SegmentManager::add_sent_segment_to_list(int seg_type,
                                              Segment *seg_to_add) {
  std::unique_lock<std::mutex> lck(this->mSentSegmentListLocks[seg_type]);

  std::list<Segment *> &sent_segment_list = this->mSentSegmentLists[seg_type];
  std::list<Segment *>::iterator it = sent_segment_list.end();
  if (it == sent_segment_list.begin()) {
    // No elements: insert here
    sent_segment_list.insert(it, seg_to_add);
    return;
  } else {
    while (it != sent_segment_list.begin()) {
      it--;
      Segment *segment = (*it);
      if (segment->seq_no < seg_to_add->seq_no) {
        // Insert here!
        it++;
        sent_segment_list.insert(it, seg_to_add);
        return;
      } else if (segment->seq_no > seg_to_add->seq_no) {
        // Skip it!
        continue;
      } else {
        // Duplicated segment
        return;
      }
    }
    // Insert to first
    sent_segment_list.insert(it, seg_to_add);
  }
}