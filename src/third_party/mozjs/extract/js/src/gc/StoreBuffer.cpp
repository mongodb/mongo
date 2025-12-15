/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/StoreBuffer-inl.h"

#include "mozilla/Assertions.h"

#include "gc/Statistics.h"
#include "vm/MutexIDs.h"
#include "vm/Runtime.h"

using namespace js;
using namespace js::gc;

#ifdef DEBUG
void StoreBuffer::checkAccess() const {
  // The GC runs tasks that may access the storebuffer in parallel and so must
  // take a lock. The mutator may only access the storebuffer from the main
  // thread.
  if (runtime_->heapState() != JS::HeapState::Idle &&
      runtime_->heapState() != JS::HeapState::MinorCollecting) {
    MOZ_ASSERT(!CurrentThreadIsGCMarking());
    runtime_->gc.assertCurrentThreadHasLockedStoreBuffer();
  } else {
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime_));
  }
}
#endif

bool StoreBuffer::WholeCellBuffer::init() {
  MOZ_ASSERT(!sweepHead_);
  if (!storage_) {
    storage_ = MakeUnique<LifoAlloc>(LifoAllocBlockSize, js::MallocArena);
    if (!storage_) {
      return false;
    }
  }

  // This prevents LifoAlloc::Enum from crashing with a release
  // assertion if we ever allocate one entry larger than
  // LifoAllocBlockSize.
  storage_->disableOversize();

  clear();
  return true;
}

bool StoreBuffer::GenericBuffer::init() {
  if (!storage_) {
    storage_ = MakeUnique<LifoAlloc>(LifoAllocBlockSize, js::MallocArena);
  }
  clear();
  return bool(storage_);
}

void StoreBuffer::GenericBuffer::trace(JSTracer* trc, StoreBuffer* owner) {
  mozilla::ReentrancyGuard g(*owner);
  MOZ_ASSERT(owner->isEnabled());
  if (!storage_) {
    return;
  }

  for (LifoAlloc::Enum e(*storage_); !e.empty();) {
    unsigned size = *e.read<unsigned>();
    BufferableRef* edge = e.read<BufferableRef>(size);
    edge->trace(trc);
  }
}

StoreBuffer::StoreBuffer(JSRuntime* rt)
    : runtime_(rt),
      nursery_(rt->gc.nursery()),
      aboutToOverflow_(false),
      enabled_(false),
      mayHavePointersToDeadCells_(false)
#ifdef DEBUG
      ,
      mEntered(false)
#endif
{
}

StoreBuffer::StoreBuffer(StoreBuffer&& other)
    : bufferVal(std::move(other.bufferVal)),
      bufStrCell(std::move(other.bufStrCell)),
      bufBigIntCell(std::move(other.bufBigIntCell)),
      bufObjCell(std::move(other.bufObjCell)),
      bufferSlot(std::move(other.bufferSlot)),
      bufferWasmAnyRef(std::move(other.bufferWasmAnyRef)),
      bufferWholeCell(std::move(other.bufferWholeCell)),
      bufferGeneric(std::move(other.bufferGeneric)),
      runtime_(other.runtime_),
      nursery_(other.nursery_),
      aboutToOverflow_(other.aboutToOverflow_),
      enabled_(other.enabled_),
      mayHavePointersToDeadCells_(other.mayHavePointersToDeadCells_)
#ifdef DEBUG
      ,
      mEntered(other.mEntered)
#endif
{
  MOZ_ASSERT(enabled_);
  MOZ_ASSERT(!mEntered);
  other.disable();
}

StoreBuffer& StoreBuffer::operator=(StoreBuffer&& other) {
  if (&other != this) {
    this->~StoreBuffer();
    new (this) StoreBuffer(std::move(other));
  }
  return *this;
}

void StoreBuffer::checkEmpty() const { MOZ_ASSERT(isEmpty()); }

bool StoreBuffer::isEmpty() const {
  return bufferVal.isEmpty() && bufStrCell.isEmpty() &&
         bufBigIntCell.isEmpty() && bufObjCell.isEmpty() &&
         bufferSlot.isEmpty() && bufferWasmAnyRef.isEmpty() &&
         bufferWholeCell.isEmpty() && bufferGeneric.isEmpty();
}

bool StoreBuffer::enable() {
  if (enabled_) {
    return true;
  }

  checkEmpty();

  if (!bufferWholeCell.init() || !bufferGeneric.init()) {
    return false;
  }

  enabled_ = true;
  return true;
}

void StoreBuffer::disable() {
  checkEmpty();

  if (!enabled_) {
    return;
  }

  aboutToOverflow_ = false;

  enabled_ = false;
}

void StoreBuffer::clear() {
  if (!enabled_) {
    return;
  }

  aboutToOverflow_ = false;
  mayHavePointersToDeadCells_ = false;

  bufferVal.clear();
  bufStrCell.clear();
  bufBigIntCell.clear();
  bufObjCell.clear();
  bufferSlot.clear();
  bufferWasmAnyRef.clear();
  bufferWholeCell.clear();
  bufferGeneric.clear();
}

void StoreBuffer::setAboutToOverflow(JS::GCReason reason) {
  if (!aboutToOverflow_) {
    aboutToOverflow_ = true;
    runtime_->gc.stats().count(gcstats::COUNT_STOREBUFFER_OVERFLOW);
  }
  nursery_.requestMinorGC(reason);
}

void StoreBuffer::addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf,
                                         JS::GCSizes* sizes) {
  sizes->storeBufferVals += bufferVal.sizeOfExcludingThis(mallocSizeOf);
  sizes->storeBufferCells += bufStrCell.sizeOfExcludingThis(mallocSizeOf) +
                             bufBigIntCell.sizeOfExcludingThis(mallocSizeOf) +
                             bufObjCell.sizeOfExcludingThis(mallocSizeOf);
  sizes->storeBufferSlots += bufferSlot.sizeOfExcludingThis(mallocSizeOf);
  sizes->storeBufferWasmAnyRefs +=
      bufferWasmAnyRef.sizeOfExcludingThis(mallocSizeOf);
  sizes->storeBufferWholeCells +=
      bufferWholeCell.sizeOfExcludingThis(mallocSizeOf);
  sizes->storeBufferGenerics += bufferGeneric.sizeOfExcludingThis(mallocSizeOf);
}

ArenaCellSet ArenaCellSet::Empty;

ArenaCellSet::ArenaCellSet(Arena* arena)
    : arena(arena)
#ifdef DEBUG
      ,
      minorGCNumberAtCreation(
          arena->zone()->runtimeFromMainThread()->gc.minorGCCount())
#endif
{
  MOZ_ASSERT(arena);
  MOZ_ASSERT(bits.isAllClear());
}

ArenaCellSet* StoreBuffer::WholeCellBuffer::allocateCellSet(Arena* arena) {
  MOZ_ASSERT(arena->bufferedCells() == &ArenaCellSet::Empty);

  Zone* zone = arena->zone();
  JSRuntime* rt = zone->runtimeFromMainThread();
  if (!rt->gc.nursery().isEnabled()) {
    return nullptr;
  }

  AutoEnterOOMUnsafeRegion oomUnsafe;
  auto* cells = storage_->new_<ArenaCellSet>(arena);
  if (!cells) {
    oomUnsafe.crash("Failed to allocate ArenaCellSet");
  }

  arena->bufferedCells() = cells;

  if (isAboutToOverflow()) {
    rt->gc.storeBuffer().setAboutToOverflow(
        JS::GCReason::FULL_WHOLE_CELL_BUFFER);
  }

  return cells;
}

void gc::CellHeaderPostWriteBarrier(JSObject** ptr, JSObject* prev,
                                    JSObject* next) {
  InternalBarrierMethods<JSObject*>::postBarrier(ptr, prev, next);
}

void StoreBuffer::WholeCellBuffer::clear() {
  for (LifoAlloc::Enum e(*storage_); !e.empty();) {
    ArenaCellSet* cellSet = e.read<ArenaCellSet>();
    cellSet->arena->bufferedCells() = &ArenaCellSet::Empty;
  }
  sweepHead_ = nullptr;

  if (storage_) {
    storage_->used() ? storage_->releaseAll() : storage_->freeAll();
  }

  last_ = nullptr;
}

template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::ValueEdge>;
template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::SlotsEdge>;
template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::WasmAnyRefEdge>;

void js::gc::PostWriteBarrierCell(Cell* cell, Cell* prev, Cell* next) {
  if (!next || !cell->isTenured()) {
    return;
  }

  StoreBuffer* buffer = next->storeBuffer();
  if (!buffer || (prev && prev->storeBuffer())) {
    return;
  }

  buffer->putWholeCell(cell);
}
