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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/simple8b.h"
#include "mongo/platform/int128.h"

#include <deque>
#include <memory>
#include <vector>

namespace mongo {

/**
 * Class to build BSON Subtype 7 (Column) binaries.
 */
class BSONColumnBuilder {
public:
    BSONColumnBuilder(StringData fieldName);
    BSONColumnBuilder(StringData fieldName, BufBuilder&& builder);

    /**
     * Appends a BSONElement to this BSONColumnBuilder.
     *
     * Value will be stored delta compressed if possible and uncompressed otherwise.
     *
     * The field name will be ignored.
     *
     * Throws InvalidBSONType if MinKey or MaxKey is appended.
     */
    BSONColumnBuilder& append(BSONElement elem);

    /**
     * Appends an index skip to this BSONColumnBuilder.
     */
    BSONColumnBuilder& skip();

    /**
     * Returns the field name this BSONColumnBuilder was created with.
     */
    StringData fieldName() const {
        return _fieldName;
    }

    /**
     * Finalizes the BSON Column and returns the BinData binary.
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
    /**
     * State for encoding scalar BSONElement as BSONColumn using delta or delta-of-delta
     * compression. When compressing Objects one Encoding state is used per sub-field within the
     * object to compress.
     */
    struct EncodingState {
        EncodingState(BufBuilder* bufBuilder,
                      std::function<void(const char*, size_t)> controlBlockWriter);
        EncodingState(EncodingState&& other);
        EncodingState& operator=(EncodingState&& rhs);

        void append(BSONElement elem);
        void skip();
        void flush();

        BSONElement _previous() const;
        void _storePrevious(BSONElement elem);
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

        // Storage for the previously appended BSONElement
        std::unique_ptr<char[]> _prev;
        int _prevSize = 0;
        int _prevCapacity = 0;
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
        std::function<void(const char*, size_t)> _controlBlockWriter;
    };

    // Append Object for sub-object compression when in mode kSubObjAppending
    bool _appendSubElements(const BSONObj& obj);

    // Transition into kSubObjDeterminingReference mode
    void _startDetermineSubObjReference(const BSONObj& obj);

    // Transition from kSubObjDeterminingReference into kSubObjAppending
    void _finishDetermineSubObjReference();

    // Transition from kSubObjDeterminingReference or kSubObjAppending back into kRegular.
    void _flushSubObjMode();

    // Encoding state for kRegular mode
    EncodingState _state;

    // Intermediate BufBuilder and offsets to written control blocks for sub-object compression
    std::deque<std::pair<BufBuilder, std::deque<std::pair<ptrdiff_t, size_t>>>> _subobjBuffers;

    // Encoding states when in sub-object compression mode. There should be one encoding state per
    // scalar field in '_referenceSubObj'.
    std::deque<EncodingState> _subobjStates;

    // Reference object that is used to match object hierarchy to encoding states. Appending objects
    // for sub-object compression need to check their hierarchy against this object.
    BSONObj _referenceSubObj;

    // Buffered BSONObj when determining reference object. Will be compressed when this is complete
    // and we transition into kSubObjAppending.
    std::vector<BSONObj> _bufferedObjElements;

    // Helper to flatten Object to compress to match _subobjStates
    std::vector<BSONElement> _flattenedAppendedObj;

    // Buffer for the BSON Column binary
    BufBuilder _bufBuilder;

    enum class Mode { kRegular, kSubObjDeterminingReference, kSubObjAppending };
    Mode _mode = Mode::kRegular;

    std::string _fieldName;
    int _numInterleavedStartWritten = 0;
};

}  // namespace mongo
