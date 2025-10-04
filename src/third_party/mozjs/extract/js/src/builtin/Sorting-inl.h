/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_Sorting_inl_h
#define builtin_Sorting_inl_h

#include "builtin/Sorting.h"

#include "js/Conversions.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"

namespace js {

void ArraySortData::init(JSObject* obj, JSObject* comparator, ValueVector&& vec,
                         uint32_t length, uint32_t denseLen) {
  MOZ_ASSERT(!vec.empty(), "must have items to sort");
  MOZ_ASSERT(denseLen <= length);

  obj_ = obj;
  comparator_ = comparator;

  this->length = length;
  this->denseLen = denseLen;
  this->vec = std::move(vec);

  auto getComparatorKind = [](JSContext* cx, JSObject* comparator) {
    if (!comparator->is<JSFunction>()) {
      return ComparatorKind::Unoptimized;
    }
    JSFunction* fun = &comparator->as<JSFunction>();
    if (!fun->hasJitEntry() || fun->isClassConstructor()) {
      return ComparatorKind::Unoptimized;
    }
    if (fun->realm() == cx->realm() && fun->nargs() <= ComparatorActualArgs) {
      return ComparatorKind::JSSameRealmNoRectifier;
    }
    return ComparatorKind::JS;
  };
  comparatorKind_ = getComparatorKind(cx(), comparator);
}

ArraySortData::ArraySortData(JSContext* cx) : cx_(cx) {
#ifdef DEBUG
  cx_->liveArraySortDataInstances++;
#endif
}

void ArraySortData::freeMallocData() {
  vec.clearAndFree();
#ifdef DEBUG
  MOZ_ASSERT(cx_->liveArraySortDataInstances > 0);
  cx_->liveArraySortDataInstances--;
#endif
}

template <ArraySortKind Kind>
static MOZ_ALWAYS_INLINE ArraySortResult
MaybeYieldToComparator(ArraySortData* d, const Value& x, const Value& y) {
  if constexpr (Kind == ArraySortKind::Array) {
    // https://tc39.es/ecma262/#sec-comparearrayelements
    // 23.1.3.30.2 CompareArrayElements ( x, y, comparefn )

    // Steps 1-2.
    if (x.isUndefined()) {
      d->setComparatorReturnValue(Int32Value(y.isUndefined() ? 0 : 1));
      return ArraySortResult::Done;
    }

    // Step 3.
    if (y.isUndefined()) {
      d->setComparatorReturnValue(Int32Value(-1));
      return ArraySortResult::Done;
    }
  } else {
    // https://tc39.es/ecma262/#sec-comparetypedarrayelements
    // 23.2.4.7 CompareTypedArrayElements ( x, y, comparefn )

    // Step 1.
    MOZ_ASSERT((x.isNumber() && y.isNumber()) ||
               (x.isBigInt() && y.isBigInt()));
  }

  // Yield to the JIT trampoline (or js::array_sort) if the comparator is a JS
  // function we can call more efficiently from JIT code.
  auto kind = d->comparatorKind();
  if (MOZ_LIKELY(kind != ArraySortData::ComparatorKind::Unoptimized)) {
    d->setComparatorArgs(x, y);
    return (kind == ArraySortData::ComparatorKind::JSSameRealmNoRectifier)
               ? ArraySortResult::CallJSSameRealmNoRectifier
               : ArraySortResult::CallJS;
  }
  return CallComparatorSlow(d, x, y);
}

static MOZ_ALWAYS_INLINE bool RvalIsLessOrEqual(ArraySortData* data,
                                                bool* lessOrEqual) {
  // https://tc39.es/ecma262/#sec-comparearrayelements
  // 23.1.3.30.2 CompareArrayElements ( x, y, comparefn )
  //
  // https://tc39.es/ecma262/#sec-comparetypedarrayelements
  // 23.2.4.7 CompareTypedArrayElements ( x, y, comparefn )
  //
  // Note: CompareTypedArrayElements step 2 is identical to CompareArrayElements
  // step 4.

  // Fast path for int32 return values.
  Value rval = data->comparatorReturnValue();
  if (MOZ_LIKELY(rval.isInt32())) {
    *lessOrEqual = rval.toInt32() <= 0;
    return true;
  }

  // Step 4.a.
  Rooted<Value> rvalRoot(data->cx(), rval);
  double d;
  if (MOZ_UNLIKELY(!ToNumber(data->cx(), rvalRoot, &d))) {
    return false;
  }

  // Step 4.b-c.
  *lessOrEqual = std::isnan(d) ? true : (d <= 0);
  return true;
}

static MOZ_ALWAYS_INLINE void CopyValues(Value* out, const Value* list,
                                         uint32_t start, uint32_t end) {
  for (uint32_t i = start; i <= end; i++) {
    out[i] = list[i];
  }
}

// static
template <ArraySortKind Kind>
ArraySortResult ArraySortData::sortWithComparatorShared(ArraySortData* d) {
  auto& vec = d->vec;

  // This function is like a generator that is called repeatedly from the JIT
  // trampoline or js::array_sort. Resume the sorting algorithm where we left
  // off before calling the comparator.
  switch (d->state) {
    case State::Initial:
      break;
    case State::InsertionSortCall1:
      goto insertion_sort_call1;
    case State::InsertionSortCall2:
      goto insertion_sort_call2;
    case State::MergeSortCall1:
      goto merge_sort_call1;
    case State::MergeSortCall2:
      goto merge_sort_call2;
  }

  d->list = vec.begin();

  // Use insertion sort for small arrays.
  if (d->denseLen <= InsertionSortMaxLength) {
    for (d->i = 1; d->i < d->denseLen; d->i++) {
      d->item = vec[d->i];
      d->j = d->i - 1;
      do {
        {
          ArraySortResult res =
              MaybeYieldToComparator<Kind>(d, vec[d->j], d->item);
          if (res != ArraySortResult::Done) {
            d->state = State::InsertionSortCall1;
            return res;
          }
        }
      insertion_sort_call1:
        bool lessOrEqual;
        if (!RvalIsLessOrEqual(d, &lessOrEqual)) {
          return ArraySortResult::Failure;
        }
        if (lessOrEqual) {
          break;
        }
        vec[d->j + 1] = vec[d->j];
      } while (d->j-- > 0);
      vec[d->j + 1] = d->item;
    }
  } else {
    static constexpr size_t InitialWindowSize = 4;

    // Use insertion sort for initial ranges.
    for (d->start = 0; d->start < d->denseLen - 1;
         d->start += InitialWindowSize) {
      d->end =
          std::min<uint32_t>(d->start + InitialWindowSize - 1, d->denseLen - 1);
      for (d->i = d->start + 1; d->i <= d->end; d->i++) {
        d->item = vec[d->i];
        d->j = d->i - 1;
        do {
          {
            ArraySortResult res =
                MaybeYieldToComparator<Kind>(d, vec[d->j], d->item);
            if (res != ArraySortResult::Done) {
              d->state = State::InsertionSortCall2;
              return res;
            }
          }
        insertion_sort_call2:
          bool lessOrEqual;
          if (!RvalIsLessOrEqual(d, &lessOrEqual)) {
            return ArraySortResult::Failure;
          }
          if (lessOrEqual) {
            break;
          }
          vec[d->j + 1] = vec[d->j];
        } while (d->j-- > d->start);
        vec[d->j + 1] = d->item;
      }
    }

    // Merge sort. Set d->out to scratch space initially.
    d->out = vec.begin() + d->denseLen;
    for (d->windowSize = InitialWindowSize; d->windowSize < d->denseLen;
         d->windowSize *= 2) {
      for (d->start = 0; d->start < d->denseLen;
           d->start += 2 * d->windowSize) {
        // The midpoint between the two subarrays.
        d->mid = d->start + d->windowSize - 1;

        // To keep from going over the edge.
        d->end = std::min<uint32_t>(d->start + 2 * d->windowSize - 1,
                                    d->denseLen - 1);

        // Merge comparator-sorted slices list[start..<=mid] and
        // list[mid+1..<=end], storing the merged sequence in out[start..<=end].

        // Skip lopsided runs to avoid doing useless work.
        if (d->mid >= d->end) {
          CopyValues(d->out, d->list, d->start, d->end);
          continue;
        }

        // Skip calling the comparator if the sub-list is already sorted.
        {
          ArraySortResult res = MaybeYieldToComparator<Kind>(
              d, d->list[d->mid], d->list[d->mid + 1]);
          if (res != ArraySortResult::Done) {
            d->state = State::MergeSortCall1;
            return res;
          }
        }
      merge_sort_call1:
        bool lessOrEqual;
        if (!RvalIsLessOrEqual(d, &lessOrEqual)) {
          return ArraySortResult::Failure;
        }
        if (lessOrEqual) {
          CopyValues(d->out, d->list, d->start, d->end);
          continue;
        }

        d->i = d->start;
        d->j = d->mid + 1;
        d->k = d->start;

        while (d->i <= d->mid && d->j <= d->end) {
          {
            ArraySortResult res =
                MaybeYieldToComparator<Kind>(d, d->list[d->i], d->list[d->j]);
            if (res != ArraySortResult::Done) {
              d->state = State::MergeSortCall2;
              return res;
            }
          }
        merge_sort_call2:
          bool lessOrEqual;
          if (!RvalIsLessOrEqual(d, &lessOrEqual)) {
            return ArraySortResult::Failure;
          }
          d->out[d->k++] = lessOrEqual ? d->list[d->i++] : d->list[d->j++];
        }

        // Empty out any remaining elements. Use local variables to let the
        // compiler generate more efficient code.
        Value* out = d->out;
        Value* list = d->list;
        uint32_t k = d->k;
        uint32_t mid = d->mid;
        uint32_t end = d->end;
        for (uint32_t i = d->i; i <= mid; i++) {
          out[k++] = list[i];
        }
        for (uint32_t j = d->j; j <= end; j++) {
          out[k++] = list[j];
        }
      }

      // Swap both lists.
      std::swap(d->list, d->out);
    }
  }

  return ArraySortResult::Done;
}

}  // namespace js

#endif /* builtin_Sorting_inl_h */
