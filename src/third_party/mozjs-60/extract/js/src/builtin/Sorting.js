/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// We use varying sorts across the self-hosted codebase. All sorts are
// consolidated here to avoid confusion and re-implementation of existing
// algorithms.

// For sorting values with limited range; uint8 and int8.
function CountingSort(array, len, signed, comparefn) {
    assert(IsPossiblyWrappedTypedArray(array), "CountingSort works only with typed arrays.");

    // Determined by performance testing.
    if (len < 128) {
        QuickSort(array, len, comparefn);
        return array;
    }

    // Map int8 values onto the uint8 range when storing in buffer.
    var min = 0;
    if (signed) {
        min = -128;
    }

    /* eslint-disable comma-spacing */
    // 32 * 8 = 256 entries.
    var buffer = [
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    ];
    /* eslint-enable comma-spacing */

    // Populate the buffer
    for (var i = 0; i < len; i++) {
        var val = array[i];
        buffer[val - min]++;
    }

    // Traverse the buffer in order and write back elements to array
    var val = -1;
    for (var i = 0; i < len;) {
        // Invariant: sum(buffer[val:]) == len-i
        var j;
        do {
            j = buffer[++val];
        } while (j === 0);

        for (; j > 0; j--)
            array[i++] = val + min;
    }
    return array;
}

// Helper for RadixSort
function ByteAtCol(x, pos) {
    return (x >> (pos * 8)) & 0xFF;
}

function SortByColumn(array, len, aux, col, counts) {
    const R = 256;

    // |counts| is used to compute the starting index position for each key.
    // Letting counts[0] always be 0, simplifies the transform step below.
    // Example:
    //
    // Computing frequency counts for the input [1 2 1] gives:
    //      0 1 2 3 ... (keys)
    //      0 0 2 1     (frequencies)
    //
    // Transforming frequencies to indexes gives:
    //      0 1 2 3 ... (keys)
    //      0 0 2 3     (indexes)
    assert(counts.length === R + 1, "counts has |R| + 1 entries");

    // Initialize all entries to zero.
    for (let r = 0; r < R + 1; r++) {
        counts[r] = 0;
    }

    // Compute frequency counts
    for (let i = 0; i < len; i++) {
        let val = array[i];
        let b = ByteAtCol(val, col);
        counts[b + 1]++;
    }

    // Transform counts to indices.
    for (let r = 0; r < R; r++) {
        counts[r + 1] += counts[r];
    }

    // Distribute
    for (let i = 0; i < len; i++) {
        let val = array[i];
        let b = ByteAtCol(val, col);
        aux[counts[b]++] = val;
    }

    // Copy back
    for (let i = 0; i < len; i++) {
        array[i] = aux[i];
    }
}

// Sorts integers and float32. |signed| is true for int16 and int32, |floating|
// is true for float32.
function RadixSort(array, len, buffer, nbytes, signed, floating, comparefn) {
    assert(IsPossiblyWrappedTypedArray(array), "RadixSort works only with typed arrays.");

    // Determined by performance testing.
    if (len < 512) {
        QuickSort(array, len, comparefn);
        return array;
    }

    let aux = [];
    for (let i = 0; i < len; i++)
        _DefineDataProperty(aux, i, 0);

    let view = array;
    let signMask = 1 << nbytes * 8 - 1;

    // Preprocess
    if (floating) {
        // Acquire a buffer if the array was previously using inline storage.
        if (buffer === null) {
            buffer = callFunction(std_TypedArray_buffer, array);

            assert(buffer !== null, "Attached data buffer should be reified");
        }

        // |array| is a possibly cross-compartment wrapped typed array.
        let offset = IsTypedArray(array)
                     ? TypedArrayByteOffset(array)
                     : callFunction(CallTypedArrayMethodIfWrapped, array, array,
                                    "TypedArrayByteOffset");

        view = new Int32Array(buffer, offset, len);

        // Flip sign bit for positive numbers; flip all bits for negative
        // numbers, except negative NaNs.
        for (let i = 0; i < len; i++) {
            if (view[i] & signMask) {
                if ((view[i] & 0x7F800000) !== 0x7F800000 || (view[i] & 0x007FFFFF) === 0) {
                    view[i] ^= 0xFFFFFFFF;
                }
            } else {
                view[i] ^= signMask;
            }
        }
    } else if (signed) {
        // Flip sign bit
        for (let i = 0; i < len; i++) {
            view[i] ^= signMask;
        }
    }

    /* eslint-disable comma-spacing */
    // 32 * 8 + 1 = 256 + 1 entries.
    let counts = [
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,
    ];
    /* eslint-enable comma-spacing */

    // Sort
    for (let col = 0; col < nbytes; col++) {
        SortByColumn(view, len, aux, col, counts);
    }

    // Restore original bit representation
    if (floating) {
        for (let i = 0; i < len; i++) {
            if (view[i] & signMask) {
                view[i] ^= signMask;
            } else {
                view[i] ^= 0xFFFFFFFF;
            }
        }
    } else if (signed) {
        for (let i = 0; i < len; i++) {
            view[i] ^= signMask;
        }
    }
    return array;
}


// For sorting small arrays.
function InsertionSort(array, from, to, comparefn) {
    let item, swap, i, j;
    for (i = from + 1; i <= to; i++) {
        item = array[i];
        for (j = i - 1; j >= from; j--) {
            swap = array[j];
            if (comparefn(swap, item) <= 0)
                break;
            array[j + 1] = swap;
        }
        array[j + 1] = item;
    }
}

function SwapArrayElements(array, i, j) {
    var swap = array[i];
    array[i] = array[j];
    array[j] = swap;
}

// A helper function for MergeSort.
function Merge(list, start, mid, end, lBuffer, rBuffer, comparefn) {
    var i, j, k;

    var sizeLeft = mid - start + 1;
    var sizeRight =  end - mid;

    // Copy our virtual lists into separate buffers.
    for (i = 0; i < sizeLeft; i++)
        lBuffer[i] = list[start + i];

    for (j = 0; j < sizeRight; j++)
        rBuffer[j] = list[mid + 1 + j];


    i = 0;
    j = 0;
    k = start;
    while (i < sizeLeft && j < sizeRight) {
        if (comparefn(lBuffer[i], rBuffer[j]) <= 0) {
            list[k] = lBuffer[i];
            i++;
        } else {
            list[k] = rBuffer[j];
            j++;
        }
        k++;
    }

    // Empty out any remaining elements in the buffer.
    while (i < sizeLeft) {
        list[k] = lBuffer[i];
        i++;
        k++;
    }

    while (j < sizeRight) {
        list[k] = rBuffer[j];
        j++;
        k++;
    }
}

// Helper function for overwriting a sparse array with a
// dense array, filling remaining slots with holes.
function MoveHoles(sparse, sparseLen, dense, denseLen) {
    for (var i = 0; i < denseLen; i++)
        sparse[i] = dense[i];
    for (var j = denseLen; j < sparseLen; j++)
        delete sparse[j];
}

// Iterative, bottom up, mergesort.
function MergeSort(array, len, comparefn) {
    // To save effort we will do all of our work on a dense list,
    // then create holes at the end.
    var denseList = [];
    var denseLen = 0;

    for (var i = 0; i < len; i++) {
        if (i in array)
            _DefineDataProperty(denseList, denseLen++, array[i]);
    }

    if (denseLen < 1)
        return array;

    // Insertion sort for small arrays, where "small" is defined by performance
    // testing.
    if (denseLen < 24) {
        InsertionSort(denseList, 0, denseLen - 1, comparefn);
        MoveHoles(array, len, denseList, denseLen);
        return array;
    }

    // We do all of our allocating up front
    var lBuffer = new List();
    var rBuffer = new List();

    var mid, end;
    for (var windowSize = 1; windowSize < denseLen; windowSize = 2 * windowSize) {
        for (var start = 0; start < denseLen - 1; start += 2 * windowSize) {
            assert(windowSize < denseLen, "The window size is larger than the array denseLength!");
            // The midpoint between the two subarrays.
            mid = start + windowSize - 1;
            // To keep from going over the edge.
            end = start + 2 * windowSize - 1;
            end = end < denseLen - 1 ? end : denseLen - 1;
            // Skip lopsided runs to avoid doing useless work
            if (mid > end)
                continue;
            Merge(denseList, start, mid, end, lBuffer, rBuffer, comparefn);
        }
    }
    MoveHoles(array, len, denseList, denseLen);
    return array;
}

// Rearranges the elements in array[from:to + 1] and returns an index j such that:
// - from < j < to
// - each element in array[from:j] is less than or equal to array[j]
// - each element in array[j + 1:to + 1] greater than or equal to array[j].
function Partition(array, from, to, comparefn) {
    assert(to - from >= 3, "Partition will not work with less than three elements");

    var medianIndex = from + ((to - from) >> 1);

    var i = from + 1;
    var j = to;

    SwapArrayElements(array, medianIndex, i);

    // Median of three pivot selection.
    if (comparefn(array[from], array[to]) > 0)
        SwapArrayElements(array, from, to);

    if (comparefn(array[i], array[to]) > 0)
        SwapArrayElements(array, i, to);

    if (comparefn(array[from], array[i]) > 0)
        SwapArrayElements(array, from, i);

    var pivotIndex = i;

    // Hoare partition method.
    for (;;) {
        do i++; while (comparefn(array[i], array[pivotIndex]) < 0);
        do j--; while (comparefn(array[j], array[pivotIndex]) > 0);
        if (i > j)
            break;
        SwapArrayElements(array, i, j);
    }

    SwapArrayElements(array, pivotIndex, j);
    return j;
}

// In-place QuickSort.
function QuickSort(array, len, comparefn) {
    assert(0 <= len && len <= 0x7FFFFFFF, "length is a positive int32 value");

    // Managing the stack ourselves seems to provide a small performance boost.
    var stack = new List();
    var top = 0;

    var start = 0;
    var end   = len - 1;

    var pivotIndex, leftLen, rightLen;

    for (;;) {
        // Insertion sort for the first N elements where N is some value
        // determined by performance testing.
        if (end - start <= 23) {
            InsertionSort(array, start, end, comparefn);
            if (top < 1)
                break;
            end   = stack[--top];
            start = stack[--top];
        } else {
            pivotIndex = Partition(array, start, end, comparefn);

            // Calculate the left and right sub-array lengths and save
            // stack space by directly modifying start/end so that
            // we sort the longest of the two during the next iteration.
            // This reduces the maximum stack size to log2(len).
            leftLen = (pivotIndex - 1) - start;
            rightLen = end - (pivotIndex + 1);

            if (rightLen > leftLen) {
                stack[top++] = start;
                stack[top++] = pivotIndex - 1;
                start = pivotIndex + 1;
            } else {
                stack[top++] = pivotIndex + 1;
                stack[top++] = end;
                end = pivotIndex - 1;
            }

        }
    }
    return array;
}
