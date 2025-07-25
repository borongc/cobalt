// Copyright 2017 The Cobalt Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "media/starboard/decoder_buffer_allocator.h"

#include <algorithm>

#include "base/feature_list.h"
#include "base/logging.h"
#include "media/base/media_switches.h"
#include "media/base/video_codecs.h"
#include "media/starboard/decoder_buffer_allocator_strategy.h"
#include "media/starboard/starboard_utils.h"
#include "starboard/common/allocator.h"
#include "starboard/common/in_place_reuse_allocator_base.h"
#include "starboard/common/log.h"
#include "starboard/common/reuse_allocator_base.h"
#include "starboard/configuration.h"
#include "starboard/media.h"

namespace media {

DecoderBufferAllocator::DecoderBufferAllocator(Type type /*= Type::kGlobal*/)
    : DecoderBufferAllocator(type,
                             SbMediaIsBufferPoolAllocateOnDemand(),
                             SbMediaGetInitialBufferCapacity(),
                             SbMediaGetBufferAllocationUnit()) {}

DecoderBufferAllocator::DecoderBufferAllocator(
    Type type,
    bool is_memory_pool_allocated_on_demand,
    int initial_capacity,
    int allocation_unit)
    : type_(type),
      is_memory_pool_allocated_on_demand_(is_memory_pool_allocated_on_demand),
      initial_capacity_(initial_capacity),
      allocation_unit_(allocation_unit) {
  DCHECK_GE(initial_capacity_, 0);
  DCHECK_GE(allocation_unit_, 0);

  if (is_memory_pool_allocated_on_demand_) {
    LOG(INFO) << "Allocated media buffer pool on demand.";
    if (type_ == Type::kGlobal) {
      Allocator::Set(this);
    }
    return;
  }

  base::AutoLock scoped_lock(mutex_);
  EnsureStrategyIsCreated();
  if (type_ == Type::kGlobal) {
    Allocator::Set(this);
  }
}

DecoderBufferAllocator::~DecoderBufferAllocator() {
  if (type_ == Type::kGlobal) {
    Allocator::Set(nullptr);
  }

  base::AutoLock scoped_lock(mutex_);

  if (strategy_) {
    DCHECK_EQ(strategy_->GetAllocated(), 0u);
    strategy_.reset();
  }
}

void DecoderBufferAllocator::Suspend() {
  if (is_memory_pool_allocated_on_demand_) {
    return;
  }

  base::AutoLock scoped_lock(mutex_);

  if (strategy_ && strategy_->GetAllocated() == 0) {
    LOG(INFO) << "Freed " << strategy_->GetCapacity()
              << " bytes of media buffer pool `on suspend`.";
    strategy_.reset();
  }
}

void DecoderBufferAllocator::Resume() {
  if (is_memory_pool_allocated_on_demand_) {
    return;
  }

  base::AutoLock scoped_lock(mutex_);
  EnsureStrategyIsCreated();
}

void* DecoderBufferAllocator::Allocate(DemuxerStream::Type type,
                                       size_t size,
                                       size_t alignment) {
  base::AutoLock scoped_lock(mutex_);

  EnsureStrategyIsCreated();

  void* p = strategy_->Allocate(type, size, alignment);
  CHECK(p);

#if !defined(COBALT_BUILD_TYPE_GOLD)
  if (starboard::common::Allocator::ExtraLogLevel() >= 2) {
    ++pending_allocation_operations_count_;
    pending_allocation_operations_ << " a " << p << " " << type << " " << size
                                   << " " << alignment;
    TryFlushAllocationLog_Locked();
  }
#endif  // !defined(COBALT_BUILD_TYPE_GOLD)

  return p;
}

void DecoderBufferAllocator::Free(void* p, size_t size) {
  if (p == nullptr) {
    DCHECK_EQ(size, 0u);
    return;
  }

  base::AutoLock scoped_lock(mutex_);

  DCHECK(strategy_);

  // TODO: b/369245553 - Cobalt: Refactor to pass a valid stream type.
  strategy_->Free(DemuxerStream::UNKNOWN, p);

#if !defined(COBALT_BUILD_TYPE_GOLD)
  if (starboard::common::Allocator::ExtraLogLevel() >= 2) {
    ++pending_allocation_operations_count_;
    pending_allocation_operations_ << " f " << p;
    TryFlushAllocationLog_Locked();
  }
#endif  // !defined(COBALT_BUILD_TYPE_GOLD)

  if (is_memory_pool_allocated_on_demand_ && strategy_->GetAllocated() == 0) {
    LOG(INFO) << "Freed " << strategy_->GetCapacity()
              << " bytes of media buffer pool `on demand`.";
    // `strategy_->PrintAllocations()` will be called inside the dtor.
    strategy_.reset();
  }
}

int DecoderBufferAllocator::GetAudioBufferBudget() const {
  return SbMediaGetAudioBufferBudget();
}

int DecoderBufferAllocator::GetBufferAlignment() const {
  return sizeof(void*);
}

int DecoderBufferAllocator::GetBufferPadding() const {
  return SbMediaGetBufferPadding();
}

base::TimeDelta
DecoderBufferAllocator::GetBufferGarbageCollectionDurationThreshold() const {
  return base::Microseconds(
      SbMediaGetBufferGarbageCollectionDurationThreshold());
}

int DecoderBufferAllocator::GetProgressiveBufferBudget(
    VideoCodec codec,
    int resolution_width,
    int resolution_height,
    int bits_per_pixel) const {
  return SbMediaGetProgressiveBufferBudget(
      MediaVideoCodecToSbMediaVideoCodec(codec), resolution_width,
      resolution_height, bits_per_pixel);
}

int DecoderBufferAllocator::GetVideoBufferBudget(VideoCodec codec,
                                                 int resolution_width,
                                                 int resolution_height,
                                                 int bits_per_pixel) const {
  return SbMediaGetVideoBufferBudget(MediaVideoCodecToSbMediaVideoCodec(codec),
                                     resolution_width, resolution_height,
                                     bits_per_pixel);
}

size_t DecoderBufferAllocator::GetAllocatedMemory() const {
  base::AutoLock scoped_lock(mutex_);
  return strategy_ ? strategy_->GetAllocated() : 0;
}

size_t DecoderBufferAllocator::GetCurrentMemoryCapacity() const {
  base::AutoLock scoped_lock(mutex_);
  return strategy_ ? strategy_->GetCapacity() : 0;
}

size_t DecoderBufferAllocator::GetMaximumMemoryCapacity() const {
  // Always returns 0, as we no longer cap the capacity since Cobalt 25.
  //
  // base::AutoLock scoped_lock(mutex_);
  return 0;
}

void DecoderBufferAllocator::EnsureStrategyIsCreated() {
  mutex_.AssertAcquired();
  if (strategy_) {
    return;
  }

  if (base::FeatureList::IsEnabled(
          kCobaltDecoderBufferAllocatorWithInPlaceMetadata)) {
    strategy_.reset(new BidirectionalFitDecoderBufferAllocatorStrategy<
                    starboard::common::InPlaceReuseAllocatorBase>(
        initial_capacity_, allocation_unit_));
    LOG(INFO) << "DecoderBufferAllocator is using InPlaceReuseAllocatorBase.";
  } else {
    strategy_.reset(new BidirectionalFitDecoderBufferAllocatorStrategy<
                    starboard::common::ReuseAllocatorBase>(initial_capacity_,
                                                           allocation_unit_));
    LOG(INFO) << "DecoderBufferAllocator is using ReuseAllocatorBase.";
  }

  LOG(INFO) << "Allocated " << initial_capacity_
            << " bytes for media buffer pool.";
}

#if !defined(COBALT_BUILD_TYPE_GOLD)
void DecoderBufferAllocator::TryFlushAllocationLog_Locked() {
  const int kMaxOperationsPerLog = 80;

  mutex_.AssertAcquired();

  // The allocation operations may generate a few hundred log lines per second
  // and lead to missing entries on some platforms.  Grouping them helps avoid
  // missing entries, and the log index will be verified in the processing
  // script.
  if ((allocation_operation_index_ + pending_allocation_operations_count_) %
              kMaxOperationsPerLog ==
          0 ||
      strategy_->GetAllocated() == 0) {
    SB_LOG(INFO) << " Media Allocation Log: " << allocation_operation_index_
                 << pending_allocation_operations_.str();

    allocation_operation_index_ += pending_allocation_operations_count_;
    pending_allocation_operations_count_ = 0;
    // Reset the pending log
    pending_allocation_operations_.str("");
    pending_allocation_operations_.clear();
  }
}
#endif  // !defined(COBALT_BUILD_TYPE_GOLD)

}  // namespace media
