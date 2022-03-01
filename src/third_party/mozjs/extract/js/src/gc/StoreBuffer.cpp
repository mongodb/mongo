/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/StoreBuffer-inl.h"

#include "mozilla/Assertions.h"

#include "gc/Statistics.h"
#include "vm/ArgumentsObject.h"
#include "vm/JSContext.h"
#include "vm/MutexIDs.h"
#include "vm/Runtime.h"

#include "gc/GC-inl.h"

using namespace js;
using namespace js::gc;

JS_PUBLIC_API void js::gc::LockStoreBuffer(StoreBuffer* sb) {
  MOZ_ASSERT(sb);
  sb->lock();
}

JS_PUBLIC_API void js::gc::UnlockStoreBuffer(StoreBuffer* sb) {
  MOZ_ASSERT(sb);
  sb->unlock();
}

bool StoreBuffer::WholeCellBuffer::init() {
  MOZ_ASSERT(!stringHead_);
  MOZ_ASSERT(!nonStringHead_);
  if (!storage_) {
    storage_ = MakeUnique<LifoAlloc>(LifoAllocBlockSize);
    // This prevents LifoAlloc::Enum from crashing with a release
    // assertion if we ever allocate one entry larger than
    // LifoAllocBlockSize.
    if (storage_) {
      storage_->disableOversize();
    }
  }
  clear();
  return bool(storage_);
}

bool StoreBuffer::GenericBuffer::init() {
  if (!storage_) {
    storage_ = MakeUnique<LifoAlloc>(LifoAllocBlockSize);
  }
  clear();
  return bool(storage_);
}

void StoreBuffer::GenericBuffer::trace(JSTracer* trc) {
  mozilla::ReentrancyGuard g(*owner_);
  MOZ_ASSERT(owner_->isEnabled());
  if (!storage_) {
    return;
  }

  for (LifoAlloc::Enum e(*storage_); !e.empty();) {
    unsigned size = *e.read<unsigned>();
    BufferableRef* edge = e.read<BufferableRef>(size);
    edge->trace(trc);
  }
}

StoreBuffer::StoreBuffer(JSRuntime* rt, const Nursery& nursery)
    : lock_(mutexid::StoreBuffer),
      bufferVal(this, JS::GCReason::FULL_VALUE_BUFFER),
      bufStrCell(this, JS::GCReason::FULL_CELL_PTR_STR_BUFFER),
      bufBigIntCell(this, JS::GCReason::FULL_CELL_PTR_BIGINT_BUFFER),
      bufObjCell(this, JS::GCReason::FULL_CELL_PTR_OBJ_BUFFER),
      bufferSlot(this, JS::GCReason::FULL_SLOT_BUFFER),
      bufferWholeCell(this),
      bufferGeneric(this),
      runtime_(rt),
      nursery_(nursery),
      aboutToOverflow_(false),
      enabled_(false),
      mayHavePointersToDeadCells_(false)
#ifdef DEBUG
      ,
      mEntered(false),
      markingNondeduplicatable(false)
#endif
{
}

void StoreBuffer::checkEmpty() const { MOZ_ASSERT(isEmpty()); }

bool StoreBuffer::isEmpty() const {
  return bufferVal.isEmpty() && bufStrCell.isEmpty() &&
         bufBigIntCell.isEmpty() && bufObjCell.isEmpty() &&
         bufferSlot.isEmpty() && bufferWholeCell.isEmpty() &&
         bufferGeneric.isEmpty();
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
  sizes->storeBufferWholeCells +=
      bufferWholeCell.sizeOfExcludingThis(mallocSizeOf);
  sizes->storeBufferGenerics += bufferGeneric.sizeOfExcludingThis(mallocSizeOf);
}

ArenaCellSet ArenaCellSet::Empty;

ArenaCellSet::ArenaCellSet(Arena* arena, ArenaCellSet* next)
    : arena(arena),
      next(next)
#ifdef DEBUG
      ,
      minorGCNumberAtCreation(
          arena->zone->runtimeFromMainThread()->gc.minorGCCount())
#endif
{
  MOZ_ASSERT(arena);
  MOZ_ASSERT(bits.isAllClear());
}

ArenaCellSet* StoreBuffer::WholeCellBuffer::allocateCellSet(Arena* arena) {
  Zone* zone = arena->zone;
  JSRuntime* rt = zone->runtimeFromMainThread();
  if (!rt->gc.nursery().isEnabled()) {
    return nullptr;
  }

  // Maintain separate lists for strings and non-strings, so that all buffered
  // string whole cells will be processed before anything else (to prevent them
  // from being deduplicated when their chars are used by a tenured string.)
  bool isString =
      MapAllocToTraceKind(arena->getAllocKind()) == JS::TraceKind::String;

  AutoEnterOOMUnsafeRegion oomUnsafe;
  ArenaCellSet*& head = isString ? stringHead_ : nonStringHead_;
  auto cells = storage_->new_<ArenaCellSet>(arena, head);
  if (!cells) {
    oomUnsafe.crash("Failed to allocate ArenaCellSet");
  }

  arena->bufferedCells() = cells;
  head = cells;

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
  for (auto** headPtr : {&stringHead_, &nonStringHead_}) {
    for (auto* set = *headPtr; set; set = set->next) {
      set->arena->bufferedCells() = &ArenaCellSet::Empty;
    }
    *headPtr = nullptr;
  }

  if (storage_) {
    storage_->used() ? storage_->releaseAll() : storage_->freeAll();
  }
}

template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::ValueEdge>;
template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::SlotsEdge>;

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
