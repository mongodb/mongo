/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelementvalue.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/simple8b_builder.h"
#include "mongo/platform/int128.h"

namespace mongo {

/**
 * Class to build BSON Subtype 7 (Column) binaries.
 */
class BSONColumnBuilder {
public:
    BSONColumnBuilder();
    explicit BSONColumnBuilder(BufBuilder builder);

    /**
     * Appends a BSONElement to this BSONColumnBuilder.
     *
     * Value will be stored delta compressed if possible and uncompressed otherwise.
     *
     * The field name will be ignored.
     *
     * EOO is treated as 'skip'.
     *
     * Throws InvalidBSONType if MinKey or MaxKey is appended.
     */
    BSONColumnBuilder& append(BSONElement elem);

    /**
     * Appends a BSONObj to this BSONColumnBuilder.
     *
     * Like appending a BSONElement of type Object.
     */
    BSONColumnBuilder& append(const BSONObj& obj);

    /**
     * Appends a BSONArray to this BSONColumnBuilder.
     *
     * Like appending a BSONElement of type Array.
     */
    BSONColumnBuilder& append(const BSONArray& arr);

    /**
     * Appends an index skip to this BSONColumnBuilder.
     */
    BSONColumnBuilder& skip();

    /**
     * Returns a BSON Column binary and leaves the BSONColumnBuilder in a state where it is allowed
     * to continue append data to it. Less efficient than 'finalize'. Anchor is the point in the
     * returned binary that will not change when more data is appended to the BSONColumnBuilder.
     *
     * The BSONColumnBuilder must remain in scope for the returned buffer to be valid. Any call to
     * 'append' or 'skip' will invalidate the returned buffer.
     */
    BSONBinData intermediate(int* anchor = nullptr);

    /**
     * Finalizes the BSON Column and returns the BinData binary. Further data append is not allowed.
     *
     * The BSONColumnBuilder must remain in scope for the pointer to be valid.
     */
    BSONBinData finalize();

    /**
     * Detaches the buffer associated with this BSONColumnBuilder. Allows the memory to be reused
     * for building another BSONColumn.
     */
    BufBuilder detach();

    /**
     * Returns the number of interleaved start control bytes this BSONColumnBuilder has written.
     */
    int numInterleavedStartWritten() const;

private:
    using ControlBlockWriteFn = std::function<void(const char*, size_t)>;

    /**
     * Deconstructed BSONElement without type and fieldname in the contigous buffer.
     */
    struct Element {
        Element() : type(EOO), size(0) {}
        Element(BSONElement elem)
            : value(elem.value()), type(elem.type()), size(elem.valuesize()) {}
        Element(const BSONObj& obj, BSONType t)
            : value(obj.objdata()), type(t), size(obj.objsize()) {}
        Element(BSONType t, BSONElementValue v, int s) : value(v), type(t), size(s) {}

        // Performs binary memory compare
        bool operator==(const Element& rhs) const;

        BSONElementValue value;
        BSONType type;
        int size;
    };

    /**
     * State for encoding scalar BSONElement as BSONColumn using delta or delta-of-delta
     * compression. When compressing Objects one Encoding state is used per sub-field within the
     * object to compress.
     */
    struct EncodingState {
        EncodingState();

        // Initializes this encoding state. Must be called after construction and move.
        void init(BufBuilder* buffer, ControlBlockWriteFn controlBlockWriter);

        void append(Element elem);
        void skip();
        void flush();

        Element _previous() const;
        void _storePrevious(Element elem);
        void _writeLiteralFromPrevious();
        void _initializeFromPrevious();
        ptrdiff_t _incrementSimple8bCount();

        // Helper to append doubles to this Column builder. Returns true if append was successful
        // and false if the value needs to be stored uncompressed.
        bool _appendDouble(double value, double previous);

        // Tries to rescale current pending values + one additional value into a new
        // Simple8bBuilder. Returns the new Simple8bBuilder if rescaling was possible and none
        // otherwise.
        boost::optional<Simple8bBuilder<uint64_t>> _tryRescalePending(int64_t encoded,
                                                                      uint8_t newScaleIndex);

        Simple8bWriteFn _createBufferWriter();

        /**
         * Copyable memory buffer
         */
        struct CloneableBuffer {
            CloneableBuffer() = default;

            CloneableBuffer(CloneableBuffer&&) = default;
            CloneableBuffer(const CloneableBuffer&);

            CloneableBuffer& operator=(CloneableBuffer&&) = default;
            CloneableBuffer& operator=(const CloneableBuffer&);

            std::unique_ptr<char[]> buffer;
            int size = 0;
            int capacity = 0;
        };

        // Storage for the previously appended BSONElement
        CloneableBuffer _prev;

        // This is only used for types that use delta of delta.
        int64_t _prevDelta = 0;

        // Simple-8b builder for storing compressed deltas
        Simple8bBuilder<uint64_t> _simple8bBuilder64;
        Simple8bBuilder<uint128_t> _simple8bBuilder128;

        // Chose whether to use 128 or 64 Simple-8b builder
        bool _storeWith128 = false;

        // Offset to last Simple-8b control byte
        std::ptrdiff_t _controlByteOffset;

        // Additional variables needed for previous state
        int64_t _prevEncoded64 = 0;
        boost::optional<int128_t> _prevEncoded128;
        double _lastValueInPrevBlock = 0;
        uint8_t _scaleIndex;

        BufBuilder* _bufBuilder;
        ControlBlockWriteFn _controlBlockWriter;
    };

    /**
     * Internal mode this BSONColumnBuilder is in.
     */
    enum class Mode {
        // Regular mode without interleaving. Appended elements are treated as scalars.
        kRegular,
        // Interleaved mode where the reference object is being determined. New sub fields are
        // attempted to be merged in to the existing reference object candidate.
        kSubObjDeterminingReference,
        // Interleaved mode with a fixed reference object. Any incompatible sub fields in appended
        // objects must exit interleaved mode.
        kSubObjAppending
    };

    /**
     * Internal state of the BSONColumnBuilder. Can be copied to restore a previous state after
     * finalize.
     */
    struct InternalState {
        Mode mode = Mode::kRegular;

        // Encoding state for kRegular mode
        EncodingState regular;

        struct SubObjState {
            SubObjState();
            SubObjState(SubObjState&&);
            SubObjState(const SubObjState&);

            SubObjState& operator=(SubObjState&&);
            SubObjState& operator=(const SubObjState&);

            EncodingState state;
            BufBuilder buffer;
            std::deque<std::pair<ptrdiff_t, size_t>> controlBlocks;

            ControlBlockWriteFn controlBlockWriter();
        };

        // Encoding states when in sub-object compression mode. There should be one encoding state
        // per scalar field in '_referenceSubObj'.
        std::deque<SubObjState> subobjStates;

        // Reference object that is used to match object hierarchy to encoding states. Appending
        // objects for sub-object compression need to check their hierarchy against this object.
        BSONObj referenceSubObj;
        BSONType referenceSubObjType;

        // Buffered BSONObj when determining reference object. Will be compressed when this is
        // complete and we transition into kSubObjAppending.
        std::vector<BSONObj> bufferedObjElements;

        // Helper to flatten Object to compress to match _subobjStates
        std::vector<BSONElement> flattenedAppendedObj;
    };

    // Append helper for appending a BSONObj
    BSONColumnBuilder& _appendObj(Element elem);

    // Append Object for sub-object compression when in mode kSubObjAppending
    bool _appendSubElements(const BSONObj& obj);

    // Transition into kSubObjDeterminingReference mode
    void _startDetermineSubObjReference(const BSONObj& obj, BSONType type);

    // Transition from kSubObjDeterminingReference into kSubObjAppending
    void _finishDetermineSubObjReference();

    // Transition from kSubObjDeterminingReference or kSubObjAppending back into kRegular.
    void _flushSubObjMode();

    InternalState _is;

    // Buffer for the BSON Column binary
    BufBuilder _bufBuilder;

    int _numInterleavedStartWritten = 0;

    bool _finalized = false;
};

}  // namespace mongo
