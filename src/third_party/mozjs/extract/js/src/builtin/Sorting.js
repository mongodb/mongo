/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// We use varying sorts across the self-hosted codebase. All sorts are
// consolidated here to avoid confusion and re-implementation of existing
// algorithms.

// For sorting small arrays.
function InsertionSort(array, from, to, comparefn) {
  let item, swap, i, j;
  for (i = from + 1; i <= to; i++) {
    item = array[i];
    for (j = i - 1; j >= from; j--) {
      swap = array[j];
      if (callContentFunction(comparefn, undefined, swap, item) <= 0) {
        break;
      }
      array[j + 1] = swap;
    }
    array[j + 1] = item;
  }
}

// A helper function for MergeSort.
//
// Merge comparefn-sorted slices list[start..<=mid] and list[mid+1..<=end],
// storing the merged sequence in out[start..<=end].
function Merge(list, out, start, mid, end, comparefn) {
  // Skip lopsided runs to avoid doing useless work.
  // Skip calling the comparator if the sub-list is already sorted.
  if (
    mid >= end ||
    callContentFunction(comparefn, undefined, list[mid], list[mid + 1]) <= 0
  ) {
    for (var i = start; i <= end; i++) {
      DefineDataProperty(out, i, list[i]);
    }
    return;
  }

  var i = start;
  var j = mid + 1;
  var k = start;
  while (i <= mid && j <= end) {
    var lvalue = list[i];
    var rvalue = list[j];
    if (callContentFunction(comparefn, undefined, lvalue, rvalue) <= 0) {
      DefineDataProperty(out, k++, lvalue);
      i++;
    } else {
      DefineDataProperty(out, k++, rvalue);
      j++;
    }
  }

  // Empty out any remaining elements.
  while (i <= mid) {
    DefineDataProperty(out, k++, list[i++]);
  }
  while (j <= end) {
    DefineDataProperty(out, k++, list[j++]);
  }
}

// Helper function for overwriting a sparse array with a
// dense array, filling remaining slots with holes.
function MoveHoles(sparse, sparseLen, dense, denseLen) {
  for (var i = 0; i < denseLen; i++) {
    sparse[i] = dense[i];
  }
  for (var j = denseLen; j < sparseLen; j++) {
    delete sparse[j];
  }
}

// Iterative, bottom up, mergesort.
function MergeSort(array, len, comparefn) {
  assert(IsPackedArray(array), "array is packed");
  assert(array.length === len, "length mismatch");
  assert(len > 0, "array should be non-empty");

  // Insertion sort for small arrays, where "small" is defined by performance
  // testing.
  if (len < 24) {
    InsertionSort(array, 0, len - 1, comparefn);
    return array;
  }

  // We do all of our allocating up front
  var lBuffer = array;
  var rBuffer = [];

  // Use insertion sort for initial ranges.
  var windowSize = 4;
  for (var start = 0; start < len - 1; start += windowSize) {
    var end = std_Math_min(start + windowSize - 1, len - 1);
    InsertionSort(lBuffer, start, end, comparefn);
  }

  for (; windowSize < len; windowSize = 2 * windowSize) {
    for (var start = 0; start < len; start += 2 * windowSize) {
      // The midpoint between the two subarrays.
      var mid = start + windowSize - 1;

      // To keep from going over the edge.
      var end = std_Math_min(start + 2 * windowSize - 1, len - 1);

      Merge(lBuffer, rBuffer, start, mid, end, comparefn);
    }

    // Swap both lists.
    var swap = lBuffer;
    lBuffer = rBuffer;
    rBuffer = swap;
  }
  return lBuffer;
}

// A helper function for MergeSortTypedArray.
//
// Merge comparefn-sorted slices list[start..<=mid] and list[mid+1..<=end],
// storing the merged sequence in out[start..<=end].
function MergeTypedArray(list, out, start, mid, end, comparefn) {
  // Skip lopsided runs to avoid doing useless work.
  // Skip calling the comparator if the sub-list is already sorted.
  if (
    mid >= end ||
    callContentFunction(comparefn, undefined, list[mid], list[mid + 1]) <= 0
  ) {
    for (var i = start; i <= end; i++) {
      out[i] = list[i];
    }
    return;
  }

  var i = start;
  var j = mid + 1;
  var k = start;
  while (i <= mid && j <= end) {
    var lvalue = list[i];
    var rvalue = list[j];
    if (callContentFunction(comparefn, undefined, lvalue, rvalue) <= 0) {
      out[k++] = lvalue;
      i++;
    } else {
      out[k++] = rvalue;
      j++;
    }
  }

  // Empty out any remaining elements.
  while (i <= mid) {
    out[k++] = list[i++];
  }
  while (j <= end) {
    out[k++] = list[j++];
  }
}

// Iterative, bottom up, mergesort. Optimized version for TypedArrays.
function MergeSortTypedArray(array, len, comparefn) {
  assert(
    IsPossiblyWrappedTypedArray(array),
    "MergeSortTypedArray works only with typed arrays."
  );

  // Use the same TypedArray kind for the buffer.
  var C = ConstructorForTypedArray(array);

  var lBuffer = new C(len);

  // Copy all elements into a temporary buffer, so that any modifications
  // when calling |comparefn| are ignored.
  for (var i = 0; i < len; i++) {
    lBuffer[i] = array[i];
  }

  // Insertion sort for small arrays, where "small" is defined by performance
  // testing.
  if (len < 8) {
    InsertionSort(lBuffer, 0, len - 1, comparefn);

    return lBuffer;
  }

  // We do all of our allocating up front.
  var rBuffer = new C(len);

  // Use insertion sort for the initial ranges.
  var windowSize = 4;
  for (var start = 0; start < len - 1; start += windowSize) {
    var end = std_Math_min(start + windowSize - 1, len - 1);
    InsertionSort(lBuffer, start, end, comparefn);
  }

  for (; windowSize < len; windowSize = 2 * windowSize) {
    for (var start = 0; start < len; start += 2 * windowSize) {
      // The midpoint between the two subarrays.
      var mid = start + windowSize - 1;

      // To keep from going over the edge.
      var end = std_Math_min(start + 2 * windowSize - 1, len - 1);

      MergeTypedArray(lBuffer, rBuffer, start, mid, end, comparefn);
    }

    // Swap both lists.
    var swap = lBuffer;
    lBuffer = rBuffer;
    rBuffer = swap;
  }

  return lBuffer;
}
