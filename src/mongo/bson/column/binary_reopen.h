/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/bson/column/bsoncolumn_util.h"
#include "mongo/bson/column/simple8b.h"
#include "mongo/bson/column/simple8b_builder.h"

#include <boost/optional/optional.hpp>

namespace mongo::bsoncolumn::internal {

/**
 * Constant to indicate invalid index for overflow or pending RLE.
 */
static constexpr int kInvalidIndex = -1;

/**
 * Helper struct for a scanned control block.
 *
 * lastAtEndOfBlock and scaleIndex are only set for control blocks containing double data.
 */
struct ControlBlock {
    const char* control = nullptr;
    double lastAtEndOfBlock = 0.0;
    uint8_t scaleIndex = Simple8bTypeUtil::kMemoryAsInteger;
};

using ControlBlockContainer = std::vector<ControlBlock>;

/**
 * Helper range of control blocks to allow for range based for loops.
 */
class ControlBlockRange {
public:
    ControlBlockRange() = default;
    ControlBlockRange(ControlBlockContainer::const_iterator b,
                      ControlBlockContainer::const_iterator e)
        : _begin(b), _end(e) {}

    ControlBlockContainer::const_iterator begin() const {
        return _begin;
    }

    ControlBlockContainer::const_iterator end() const {
        return _end;
    }

private:
    ControlBlockContainer::const_iterator _begin;
    ControlBlockContainer::const_iterator _end;
};

/**
 * Calculated overflow point used to initialize the BSONColumnBuilder in the binary reopen
 * operation.
 */
template <typename T>
struct OverflowPoint {
    explicit OverflowPoint(boost::optional<T> val) : _last(val) {}

    /**
     * Calculated control byte for overflow/no-overflow.
     *
     * If overflow occurred, overflow() will return true and index() the position in this
     * control byte where we overflowd. Data before and including the overflow point needs to be
     * written to the buffer and data after this overflow point needs to be appended to the
     * pending state.
     *
     * If no overflow, overflow() will return false and index() return -1. No data should be
     * written to the buffer and all data in this control should be appended to the pending
     * state.
     */
    const char* control() const {
        return _control;
    }

    /**
     * Scale index for the control byte returned by control().
     */
    uint8_t scaleIndex() const {
        return _scaleIndex;
    }

    /**
     * Returns true if overflow occurred, false otherwise.
     */
    bool overflow() const {
        return overflowIndex != bsoncolumn::internal::kInvalidIndex;
    }
    /**
     * Returns index of the simple8b block where the overflow occurred, -1 if no overflow.
     */
    int index() const {
        return overflowIndex;
    }
    /**
     * Returns true if all values after the overflow point are identical to the value returned
     * by last().
     */
    bool allValuesIdentical() const {
        return _allIdentical;
    }

    /**
     * Last control byte of the binary prior to reopen.
     */
    uint8_t lastControl() const {
        return _lastControl;
    }
    /**
     * Offset to the last control byte of the binary prior to reopen.
     */
    uint16_t lastControlOffset() const {
        return _lastControlOffset;
    }

    /**
     * Range of control blocks after the overflow point that needs to be appended to the pending
     * state.
     */
    const ControlBlockRange& remaining() const {
        return _remaining;
    }

    /**
     * Last value at the overflow point. Used to decode values after overflow when they start
     * with RLE and to setup the pending state with RLE if necessary.
     */
    const boost::optional<T>& last() const {
        return _last;
    }

    /**
     * Internal helper used by OverflowState to set final OverflowPoint result.
     */
    void setControl(const char* ctrl,
                    uint8_t scale,
                    ControlBlockRange remain,
                    uint8_t lastControl,
                    uint16_t lastControlOffset) {
        _control = ctrl;
        _scaleIndex = scale;
        _remaining = remain;
        _lastControl = lastControl;
        _lastControlOffset = lastControlOffset;
    }
    /**
     * Internal helper to set last.
     */
    void setLast(boost::optional<T> value) {
        _last = value;
    }

    /**
     * Internal helper to mark that overflow occurred, at which position and if all values after
     * the overflow are identical or not.
     */
    void markOverflow(int index, bool allIdentical) {
        invariant(index != bsoncolumn::internal::kInvalidIndex);
        overflowIndex = index;
        _allIdentical = allIdentical;
    }
    /**
     * Internal helper to override overflow to set it back to no overflow.
     */
    void markNoOverflow() {
        overflowIndex = bsoncolumn::internal::kInvalidIndex;
    }
    /**
     * Internal helper to explicitly set that all values are identical.
     */
    void setAllIdentical() {
        _allIdentical = true;
    }

private:
    boost::optional<T> _last;
    const char* _control = nullptr;
    int overflowIndex = bsoncolumn::internal::kInvalidIndex;
    uint8_t _scaleIndex = kInvalidScaleIndex;
    uint8_t _lastControl = 0;
    uint16_t _lastControlOffset = 0;
    ControlBlockRange _remaining;
    bool _allIdentical = false;
};

/**
 * Helper to calculate how to re-initialize the compressor from a compressed binary.
 *
 * The main difficulty with re-initializing the compressor from a compressed binary is how to
 * undo the 'finalize()/intermediate()' call where pending values are flushed out to simple8b
 * blocks in the binary. We need to undo this operation by putting back these values back into
 * the pending state. The point in the binary where we need to do this undo is called the
 * overflow point.
 *
 * For this to be efficient we need to calculate this from the end of the binary rather than the
 * beginning. In the typical case we will use a dummy Simple8bBuilder where values are added in
 * the reverse order to observe when we can no longer add values without needing to write full
 * simple8b blocks.
 *
 * The two main sources of complexity in this algorithm is how to deal with RLE and double
 * values that have been rescaled. RLE in BSONColumn is defined as a repeat of the prior value
 * so we must be able to travserse past RLE to determine if the overflow point happens before or
 * after. For rescale we have a similar problem, either the rescale can be undone and put back
 * into pending or it is required due to an incompatible value.
 */
template <typename T>
class OverflowState {
public:
    // Initialize the overflow state from the last control block.
    OverflowState(ControlBlock cb);

    // Perform the overflow detection.
    const OverflowPoint<T>& detect(const ControlBlockContainer& controls);

private:
    // Helper function to handle the special case where the binary ends with RLE
    void _detectEndsRLE(ControlBlock cb);
    // Helper function to perform the regular detection logic. When overflow is detected it
    // returns a number of control blocks already processed that the overflow point refers to.
    // This can happen when we are processing RLE but discover that the RLE data belong prior to
    // the overflow point.
    int _detectRegular(ControlBlock cb);
    // Helper function when a rescale is detected. This automatically ends the overflow
    // detection but we need to calculate if the rescaled data should be included in the
    // overflow or not. Returns a binary offset to the last control byte in the case where the
    // rescaled data was only output because of the finalize/intermediate call.
    uint16_t _detectRescale(ControlBlockRange before, ControlBlockRange after);

    OverflowPoint<T> _op;
    Simple8bBuilder<T> _overflowDetector;
    int _pendingRle = bsoncolumn::internal::kInvalidIndex;
    int _pendingRleBlocks = 0;
};

/**
 * Result from the 'findOverflow' call.
 *
 * lastValue is the last value in the simple8b causing overflow. If no overflow was detected it is
 * set to the previously known last value.
 *
 * overflowIndex is the index position of the simple8b block in the control that caused overflow.
 * Invalid if no overflow was detected.
 *
 * pendingRLEindex is the index position of the first non-RLE simple8b block when the control begins
 * with RLE and no overflow was detected.
 */
template <typename T>
struct OverflowResult {
    boost::optional<T> lastValue;
    int overflowIndex;
    int pendingRLEindex;
};

/**
 * Result from the 'findLastNonRLE' call.
 *
 * lastValue is the last value in the last non-RLE simple8b. Only applicable when
 * 'index' is set to a non-invalid index.
 *
 * index is the index position of the last non-RLE simple8b in this control
 */
template <typename T>
struct LastNonRLEResult {
    boost::optional<T> lastValue;
    int index;
};

/**
 * Helper to get a simple8b block at index from a control block
 */
const char* s8b(const char* control, int index);

/**
 * Helper to determine if the provided simple8b block is an RLE block.
 */
bool isRLE(const char* s8b);

/**
 * Estimates the last non-skip value in a control block.
 *
 * If the last block is RLE, 0 is returned.
 * If no non-skip value can be found within what could fit in a non-RLE block, 'none' is returned.
 */
template <typename T>
boost::optional<T> estimateLastValue(const char* control);

/**
 * Finds the last non-skip value in a control block. The last block must NOT be RLE.
 *
 * If no non-skip value can be found within what could fit in a non-RLE block, 'none' is returned.
 */
template <typename T>
boost::optional<T> findLastNonSkip(const char* control, int numBlocks);

/**
 * Finds which simple8b block in the provided control block causes overflow, searches in reverse
 * order.
 *
 * 'lastValForRLE' indicates how any encountered RLE blocks should be interpreted.
 *
 * 'overflowDetector' is appended to internally, overflow is detected when it needs to write a
 * simple8b block. The same detector may be used in multiple calls for finding overflow.
 */
template <typename T>
OverflowResult<T> findOverflow(const char* control,
                               boost::optional<T> lastValForRLE,
                               Simple8bBuilder<T>& overflowDetector);

/**
 * Finds the last non-RLE simple8b block in the provided control, returns its index position and
 * last value.
 *
 * 'index' indicates position to start search that is performed in reverse order.
 */
template <typename T>
LastNonRLEResult<T> findLastNonRLE(const char* control);
template <typename T>
LastNonRLEResult<T> findLastNonRLE(const char* control, int index);


template <typename T>
OverflowState<T>::OverflowState(ControlBlock cb)
    : _op(bsoncolumn::internal::estimateLastValue<T>(cb.control)),
      _overflowDetector(_op.last(), 0) {}

template <typename T>
const OverflowPoint<T>& OverflowState<T>::detect(const ControlBlockContainer& controls) {
    using namespace bsoncolumn::internal;

    // This function must be called with at least one control block
    invariant(!controls.empty());

    // Setup reverse iteration.
    auto begin = controls.rbegin();
    auto it = begin;
    auto end = controls.rend();

    // Setup some internal state, the algorithm is different if the last block is RLE.
    uint16_t lastControlOffset = 0;
    bool endsWithRLE = isRLE(s8b(controls.back().control,
                                 numSimple8bBlocksForControlByte(*controls.back().control) - 1));

    // Search backwards for the overflow point.
    for (; it != end; ++it) {
        if (it->scaleIndex == controls.back().scaleIndex) {
            if (endsWithRLE) {
                // If we end with RLE, we simply search backwards for the first non-RLE value. This
                // will be the last non-RLE value in the binary and that will be our overflow point.
                _detectEndsRLE(*it);
            } else {
                // Regular case where we don't end with RLE. If RLE is encountered during the
                // iteration we need to continue to search until the next non-RLE value is
                // encountered. Depending on its value we might have to go back to where we were
                // prior to the RLE and assign that as our overflow point. _detectRegular will
                // return how many blocks we need to undo if this is the case.
                it = std::prev(it, _detectRegular(*it));
            }

            // _detectEndsRLE or _detectRegular will internally mark for overflow if it happened. If
            // this is the case, break out of the iteration as we are done.
            if (_op.overflow()) {
                break;
            }
        } else {
            // Special case for the double type when a control block of a different scale was
            // detected. We have a special algorithm to determine if the overflow happened in this
            // rescaled control or prior. _detectRescale will return an offset to the last
            // (rescaled) control if it can be undone and all values put back to pending.
            if constexpr (std::is_same_v<T, uint64_t>) {
                lastControlOffset =
                    _detectRescale({controls.begin(), it.base()}, {it.base(), controls.end()});
                break;
            } else {
                // This cannot happen as scan() has already verified this.
                MONGO_UNREACHABLE;
            }
        }
    }

    // Check if we've finished the iteration without finding an overflow
    if (it == end) {
        if (_pendingRle != bsoncolumn::internal::kInvalidIndex) {
            // We are in pending RLE without finding an overflow. We can put everything back in
            // pending if the pending RLE value is 0 which is the only allowed form of RLE in the
            // beginning of the binary.
            if (_op.last() == T{0}) {
                _pendingRleBlocks = 0;
                _op.setAllIdentical();
            } else {
                // Our pending RLE value is non-zero which means that the RLE cannot be put in
                // pending and the overflow happened after the RLE. Restore the state to this point.
                _op.markOverflow(_pendingRle, false);
                it = std::prev(it, _pendingRleBlocks + 1);
                _pendingRleBlocks = 0;
            }
        }
        // As we got to the beginning, set last to 0 which is how RLE in the beginning of the binary
        // must be interpreted.
        _op.setLast(T{0});
        // If we end with RLE but never detect overflow, all values are identical to 0.
        if (endsWithRLE) {
            _op.setAllIdentical();
        }
    }

    // If we have found an overflow that happened in the last simple8b block of a control, we can
    // transform this to a non-overflow at the beginning of the control after (moving the iterator
    // will be done in the next if-statement).
    if (_op.overflow() && it != begin && _op.index() == kMaxNumSimple8bPerControl - 1) {
        _op.markNoOverflow();
    }

    // If no overflow occurred, go back to the previous control as we should not add data from the
    // current control.
    if (!_op.overflow() && it != begin && lastControlOffset == 0) {
        it = std::prev(it, _pendingRleBlocks + 1);
    }

    // Record final calculatetion
    _op.setControl(it->control,
                   it->scaleIndex,
                   {it.base(), controls.end()},
                   // If lastControlOffset is non-zero we're in the special rescale case where we
                   // need to report the final control byte from the binary.
                   lastControlOffset == 0 ? *it->control : *controls.back().control,
                   lastControlOffset);
    return _op;
}

template <typename T>
void OverflowState<T>::_detectEndsRLE(ControlBlock cb) {
    // If the last block ends with RLE we just need to look for the last non-RLE block to
    // discover the overflow point.
    using namespace bsoncolumn::internal;
    LastNonRLEResult<T> res = findLastNonRLE<T>(cb.control);
    _op.setLast(res.lastValue);
    if (res.index != kInvalidIndex) {
        _op.markOverflow(res.index, true);
    }
}

template <typename T>
int OverflowState<T>::_detectRegular(ControlBlock cb) {
    using namespace bsoncolumn::internal;
    if (_pendingRle == kInvalidIndex) {
        // If we haven't encountered an RLE block in the beginning of a control block yet then
        // continue with the regular overflow detection.
        OverflowResult<T> res = findOverflow<T>(cb.control, _op.last(), _overflowDetector);
        _op.setLast(res.lastValue);
        // If this block begins with RLE we need to remember the index position after this RLE.
        _pendingRle = res.pendingRLEindex;
        if (res.overflowIndex != kInvalidIndex) {
            _op.markOverflow(res.overflowIndex, /* allIdentical= */ _pendingRle != kInvalidIndex);
        }

    } else {
        // When we've encountered RLE in the beginning of a control block we need to continue to
        // search for the next non-RLE block to determine where the overflow point is.
        LastNonRLEResult<T> res = findLastNonRLE<T>(cb.control);
        if (res.index == kInvalidIndex) {
            // Still no overflow, increment how many control blocks we've consumed in this state.
            ++_pendingRleBlocks;
        } else if (res.lastValue == _op.last()) {
            // Last value prior to RLE matches our RLE state after RLE. We then overflow in
            // the block prior to RLE. Reset pending blocks and mark the overflow with all identical
            // values.
            _pendingRleBlocks = 0;
            _op.markOverflow(res.index, /* allIdentical= */ true);
        } else {
            // Values to not match, so the overflow happened in the pending block after the RLE,
            // we've saved this position in _pendingRle.
            _op.markOverflow(_pendingRle, /* allIdentical= */ false);
            _op.setLast(res.lastValue);
            // Return how many control blocks ago the overflow position refers to.
            auto ret = _pendingRleBlocks + 1;
            _pendingRleBlocks = 0;
            return ret;
        }
    }


    return 0;
}

template <typename T>
uint16_t OverflowState<T>::_detectRescale(ControlBlockRange before, ControlBlockRange after) {
    using namespace bsoncolumn::internal;

    // Calculate last value before the rescaling event. Search backwards for the last non-RLE block
    // and get the last value from it.
    auto it = std::make_reverse_iterator(before.end());
    auto end = std::make_reverse_iterator(before.begin());
    auto blockWithOldScale = *it;
    auto blocks = numSimple8bBlocksForControlByte(*blockWithOldScale.control);

    for (; it != end; ++it) {
        LastNonRLEResult<T> res = findLastNonRLE<T>(it->control);
        // kInvalidIndex index means that all blocks were RLE and we need to continue to next block.
        if (res.index != kInvalidIndex) {
            _op.setLast(res.lastValue);
            break;
        }
    }
    // Nothing found, 0 is used as last when the stream begins with RLE.
    if (it == end) {
        _op.setLast(T{0});
    }


    // If this rescaled block is full, we know that we can treat this as a no-overflow in the next
    // control as nothing more can fit in this one anyway.
    if (blocks == kMaxNumSimple8bPerControl) {
        // If we're in pending RLE, we can additionally mark all values as identical.
        if (_pendingRle != kInvalidIndex) {
            _op.setAllIdentical();
            _pendingRleBlocks = 0;
        }
        return 0;
    }

    // Based on this actual last value, re-calculate if we will overflow with the data in
    // the control blocks we've already processed. Previously we used an estimated last.
    Simple8bBuilder<uint64_t> s8bBuilder(_op.last(), 0);
    for (auto&& cb : after) {
        OverflowResult<T> res = findOverflow<T>(cb.control, _op.last(), s8bBuilder);
        // If overflow is detected, we treat this as a non-overflow in the next control block. This
        // is signalled by not marking for overflow and returning 0 offset to the final control
        // block. Everything remaining will be put back into pending.
        if (res.overflowIndex != kInvalidIndex) {
            return 0;
        }

        // RLE detected, we then know that all values are identical.
        if (res.pendingRLEindex != kInvalidIndex) {
            _op.setAllIdentical();
            break;
        }
    }

    // Next we need to see if the first value stored in the future control blocks (with a different
    // scale) can be scaled using this scale factor that we've now encountered. First, take the next
    // control block (the range is guaranteed to be non-empty).
    const auto& next = *after.begin();

    // Encode the last value using next scale factor, this is needed to expand future deltas. This
    // is guaranteed to succeed as the scan() function has already validated this.
    auto encoded =
        Simple8bTypeUtil::encodeDouble(blockWithOldScale.lastAtEndOfBlock, next.scaleIndex);

    // Extract the first value from the next control block. Simple8b cannot be empty, so we can
    // dereference the begin iterator without further checking.
    boost::optional<T> nextVal =
        *Simple8b<uint64_t>(next.control + 1, sizeof(uint64_t), _op.last()).begin();

    // Skipped values can be always be scaled with any scale factor
    if (nextVal) {
        // Calculate the encoded delta of the next value and then try to encode it using our new
        // scale factor
        encoded = expandDelta(*encoded, Simple8bTypeUtil::decodeInt64(*nextVal));
        if (!Simple8bTypeUtil::encodeDouble(
                Simple8bTypeUtil::decodeDouble(*encoded, next.scaleIndex),
                blockWithOldScale.scaleIndex)) {
            // Not possible to scale this value using the last scale factor. We return 0 to signal
            // this as non-overflow in the block after the rescale, which effectively discards
            // everything before the rescale as they will never be needed.
            return 0;
        }
    }

    // Rescaling was possible, all the rescaled values will then need to be written back as pending
    // values. This is signalled as an overflow in the last position of this control block. We also
    // return an offset to the last control byte of the actual rescaled control block in the binary.
    _op.markOverflow(blocks - 1, false);
    return blocks * sizeof(uint64_t) + 1 +
        std::distance(after.begin(), after.end() - 1) *
        (kMaxNumSimple8bPerControl * sizeof(uint64_t) + 1);
}

inline const char* s8b(const char* control, int index) {
    return control +
        /* offset to block at index */ index * /* simple8b block size */ sizeof(uint64_t) +
        /* skip control byte*/ 1;
}

inline bool isRLE(const char* s8b) {
    // Read simple8b block and mask out the selector
    return (ConstDataView(s8b).read<LittleEndian<uint64_t>>() &
            simple8b_internal::kBaseSelectorMask) == simple8b_internal::kRleSelector;
}

template <typename T>
boost::optional<T> estimateLastValue(const char* control) {
    auto numBlocks = numSimple8bBlocksForControlByte(*control);
    if (isRLE(s8b(control, numBlocks - 1))) {
        return T{0};
    }

    // Assume that the last value in Simple8b blocks is the same as the one before the
    // first. This assumption will hold if all values are equal and RLE is eligible. If it
    // turns out to be incorrect the Simple8bBuilder will internally reset and disregard
    // RLE.
    return findLastNonSkip<T>(control, numBlocks);
}

template <typename T>
boost::optional<T> findLastNonSkip(const char* control, int numBlocks) {
    // Limit the search for a non-skip value. If we go above 60 without overflow then we consider
    // skip to be the last value for RLE as it would be the only one eligible for RLE.
    constexpr int kMaxNumSkipInNonRLEBlock = 60;
    for (int index = numBlocks - 1, numSkips = 0; index >= 0 && numSkips < kMaxNumSkipInNonRLEBlock;
         --index) {
        const char* block = s8b(control, index);
        // Abort this operation when an RLE block is found, they are handled in a separate code
        // path.
        if (isRLE(block)) {
            break;
        }
        Simple8b<T> s8b(block, sizeof(uint64_t));
        for (auto it = s8b.begin(), end = s8b.end();
             it != end && numSkips < kMaxNumSkipInNonRLEBlock;
             ++it) {
            const auto& elem = *it;
            if (elem) {
                // We do not need to use the actual last value for RLE when determining overflow
                // point later. We can use the first value we discover when performing this
                // iteration. For a RLE block to be undone and put back into the pending state all
                // values need to be the same. So if a value later in this Simple8b block is
                // different from this value we cannot undo all these containing a RLE. If the
                // values are not all the same we will not fit 120 zeros in pending and the RLE
                // block will be left as-is.
                return elem;
            }
            ++numSkips;
        }
    }
    // We did not find any value, so use skip as RLE. It is important that we use 'none' to
    // interpret RLE blocks going forward so we can properly undo simple8b blocks containing all
    // skip and RLE blocks.
    return boost::none;
}

template <typename T>
OverflowResult<T> findOverflow(const char* control,
                               boost::optional<T> lastValForRLE,
                               Simple8bBuilder<T>& overflowDetector) {
    // Search is performed in reverse order
    int index = numSimple8bBlocksForControlByte(*control) - 1;
    for (; index >= 0; --index) {
        // Get pointer to Simple8b block at this index position
        const char* block = s8b(control, index);

        // If this is an RLE block and if the overflow detector is in RLE mode, we need to skip to
        // the next non-RLE block and compare its last value against the values after RLE.
        if (isRLE(block)) {
            // If we are not in RLE mode then we know that overflow occurred in this RLE block,
            // return its position.
            if (!overflowDetector.rlePossible()) {
                return {lastValForRLE, index, kInvalidIndex};
            }

            // Search for the next non-RLE block and get the last value from it.
            LastNonRLEResult<T> res = findLastNonRLE<T>(control, index - 1);
            if (res.index == kInvalidIndex) {
                // We exhausted this control block without determining where the overflow point
                // is. Return pending RLE index so we can continue this operation in the prior
                // control block. If the value we find prior to the RLE is different, then the
                // overflow happened at this 'pending RLE' index.
                return {lastValForRLE, kInvalidIndex, index};
            } else if (res.lastValue == lastValForRLE) {
                // Last value prior to RLE matches our RLE state after RLE. We then overflow in
                // the block prior to RLE.
                return {lastValForRLE, res.index, kInvalidIndex};
            }

            // Last value prior to RLE does not match our RLE state after RLE. We then overflow in
            // the RLE block with the previous value set to the actual RLE value from the block
            // prior to RLE.
            return {res.lastValue, index, kInvalidIndex};
        }

        // Regular non-RLE block. We extract all values and append it to our overflow detector to
        // see if they cause overflow.
        Simple8b<T> s8b(block,
                        /* one block at a time */ sizeof(uint64_t),
                        lastValForRLE);
        boost::optional<T> last;

        bool overflow = false;
        auto writeFn = [&overflow](uint64_t block) mutable {
            overflow = true;
        };
        for (auto&& elem : s8b) {
            last = elem;
            if (elem) {
                overflowDetector.append(*last, writeFn);
            } else {
                overflowDetector.skip(writeFn);
            }
        }

        // If overflow point detected, we return this index position and its calculated last value.
        if (overflow) {
            return {last, index, kInvalidIndex};
        }
    }

    // We have depleated this control block without finding an overflow position, return invalid
    // index positions.
    return {lastValForRLE, kInvalidIndex, kInvalidIndex};
}

template <typename T>
LastNonRLEResult<T> findLastNonRLE(const char* control) {
    return findLastNonRLE<T>(control, numSimple8bBlocksForControlByte(*control) - 1);
}

template <typename T>
LastNonRLEResult<T> findLastNonRLE(const char* control, int index) {
    // Search is performed in reverse order
    for (; index >= 0; --index) {
        const char* block = s8b(control, index);
        if (isRLE(block)) {
            continue;
        }

        // Non-RLE block found, calculate its last value and return. Last value for RLE is unused as
        // we already know that this is not an RLE block.
        uint64_t unused = simple8b::kInvalidSimple8b;
        boost::optional<T> last = simple8b::last<T>(block, sizeof(uint64_t), unused);

        return {last, index};
    }

    return {T{}, index};
}

}  // namespace mongo::bsoncolumn::internal
