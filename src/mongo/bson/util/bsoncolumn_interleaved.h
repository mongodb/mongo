/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <algorithm>

#include "mongo/bson/util/bsoncolumn_helpers.h"

namespace mongo::bsoncolumn {

/**
 * We are often dealing with vectors of buffers below, but there is almost always only one buffer.
 */
template <typename T>
using BufferVector = boost::container::small_vector<T, 1>;

/**
 * Helper class that will append a sub-object to a buffer once it's complete.
 */
template <typename Buffer>
struct BlockBasedSubObjectFinisher {
    BlockBasedSubObjectFinisher(const BufferVector<Buffer*>& buffers) : _buffers(buffers) {}
    void finish(const char* elemBytes, int fieldNameSize, int totalSize);
    void finishMissing();

    const BufferVector<Buffer*>& _buffers;
};

/**
 * A helper class for block-based decompression of object data.
 */
class BlockBasedInterleavedDecompressor {
public:
    /**
     * One instance of this class will decompress an interleaved block that begins at "control."
     * Parameter "end" should point to the byte after the end of the BSONColumn data, used for
     * sanity checks.
     */
    BlockBasedInterleavedDecompressor(ElementStorage& allocator,
                                      const char* control,
                                      const char* end);

    /**
     * Decompresses interleaved data where data at a given path is sent to the corresonding buffer.
     * Returns a pointer to the next byte after the EOO that ends the interleaved data.
     */
    template <typename Path, typename Buffer>
    const char* decompress(std::vector<std::pair<Path, Buffer>>& paths);

private:
    /**
     * Decoding state for a stream of values corresponding to a scalar field.
     */
    struct DecodingState {
        DecodingState();

        /**
         * A tagged union type representing values decompressed from BSONColumn bytes. This can a
         * BSONElement if the value appeared uncompressed, or it can be an encoded representation
         * that was computed from a delta.
         */
        using Elem =
            std::variant<BSONElement, std::pair<BSONType, int64_t>, std::pair<BSONType, int128_t>>;

        /**
         * State when decoding deltas for 64-bit values.
         */
        struct Decoder64 {
            Decoder64();

            boost::optional<int64_t> lastEncodedValue;
            Simple8b<uint64_t>::Iterator pos;
            int64_t lastEncodedValueForDeltaOfDelta = 0;
            uint8_t scaleIndex;
            bool deltaOfDelta = false;
        };

        /**
         * State when decoding deltas for 128-bit values.
         */
        struct Decoder128 {
            boost::optional<int128_t> lastEncodedValue;
            Simple8b<uint128_t>::Iterator pos;
        };

        /**
         * Initializes a decoder given an uncompressed BSONElement in the BSONColumn bytes.
         */
        void loadUncompressed(const BSONElement& elem);

        struct LoadControlResult {
            Elem element;
            int size;
        };

        /**
         * Assuming that buffer points at the next control byte, takes the appropriate action:
         * - If the control byte begins an uncompressed literal: initializes a decoder, and returns
         *   the literal.
         * - If the control byte precedes blocks of deltas, applies the first delta and returns the
         *   new expanded element.
         * In both cases, the "size" field will contain the number of bytes to the next control
         * byte.
         */
        LoadControlResult loadControl(ElementStorage& allocator, const char* buffer);

        /**
         * Apply a delta or delta of delta to an encoded representation to get a new element value.
         * May also apply a 0 delta to an uncompressed literal, simply returning the literal.
         */
        Elem loadDelta(ElementStorage& allocator, Decoder64& d64);

        /**
         * Apply a delta to an encoded representation to get a new element value. May also apply a 0
         * delta to an uncompressed literal, simply returning the literal.
         */
        Elem loadDelta(ElementStorage& allocator, Decoder128& d128);

        /**
         * The last uncompressed literal from the BSONColumn bytes.
         */
        BSONElement _lastLiteral;

        /**
         * 64- or 128-bit specific state.
         */
        std::variant<Decoder64, Decoder128> decoder = Decoder64{};
    };

    template <typename Buffer>
    struct FastDecodingState;

    template <typename Buffer>
    const char* decompressGeneral(
        absl::flat_hash_map<const void*, BufferVector<Buffer*>>&& elemToBuffer);

    static bool moreData(DecodingState& ds, const char* control);

    template <typename T, typename Encoding, class Buffer, typename Materialize, typename Finish>
    static void decompressAllDelta(const char* ptr,
                                   const char* end,
                                   Buffer& buffer,
                                   Encoding last,
                                   const BSONElement& reference,
                                   const Materialize& materialize,
                                   const Finish& finish);

    template <typename Buffer>
    const char* decompressFast(
        absl::flat_hash_map<const void*, BufferVector<Buffer*>>&& elemToBuffer);

    void writeToElementStorage(DecodingState::Elem elem,
                               BSONElement lastLiteral,
                               StringData fieldName);

    template <class Buffer>
    static void appendToBuffers(BufferVector<Buffer*>& buffers,
                                DecodingState::Elem elem,
                                BSONElement lastLiteral);

    template <typename Buffer, typename T>
    static void appendEncodedToBuffers(BufferVector<Buffer*>& buffers, T v) {
        for (auto&& b : buffers) {
            b->append(v);
        }
    }

    /*
     * If the 'Container' needs the decompressor to collect position information, this function will
     * append the position information of the decompressed document to the buffer, and clear the
     * position information stored in the map. This function is a no-op otherwise.
     */
    template <class Buffer>
    static void flushPositionsToBuffers(absl::flat_hash_map<Buffer*, int32_t>& bufferToPositions);

    ElementStorage& _allocator;
    const char* const _control;
    const char* const _end;
    const BSONType _rootType;
    const bool _traverseArrays;
};

// Avoid GCC/Clang compiler issues
inline BlockBasedInterleavedDecompressor::DecodingState::DecodingState() = default;
inline BlockBasedInterleavedDecompressor::DecodingState::Decoder64::Decoder64() = default;

template <typename Buffer>
void BlockBasedSubObjectFinisher<Buffer>::finish(const char* elemBytes,
                                                 int fieldNameSize,
                                                 int totalSize) {
    BSONElement elem{elemBytes, fieldNameSize, totalSize, BSONElement::TrustedInitTag{}};
    for (auto&& buffer : _buffers) {
        // use preallocated method here to indicate that the element does not need to be
        // copied to longer-lived memory.
        buffer->appendPreallocated(elem);
    }
}

template <typename Buffer>
void BlockBasedSubObjectFinisher<Buffer>::finishMissing() {
    for (auto&& buffer : _buffers) {
        buffer->appendMissing();
    }
}

/**
 * Decompresses interleaved data where data at a given path is sent to the corresonding buffer.
 * Returns a pointer to the next byte after the EOO that ends the interleaved data.
 */
template <typename Path, typename Buffer>
const char* BlockBasedInterleavedDecompressor::decompress(
    std::vector<std::pair<Path, Buffer>>& paths) {

    // The reference object will appear right after the control byte that starts interleaved
    // mode.
    BSONObj refObj{_control + 1};

    // find all the scalar elements in the reference object.
    absl::flat_hash_set<const void*> scalarElems;
    {
        BSONObjTraversal findScalar{
            _traverseArrays,
            _rootType,
            [](StringData fieldName, const BSONObj& obj, BSONType type) { return true; },
            [&scalarElems](const BSONElement& elem) {
                scalarElems.insert(elem.value());
                // keep traversing to find every scalar field.
                return true;
            }};
        findScalar.traverse(refObj);
    }

    // For each path, we can use a fast implementation if it just decompresses a single scalar field
    // to a buffer. Paths that don't match any elements in the reference object will just get a
    // bunch of missing values appended, and can take the fast path as well.
    absl::flat_hash_map<const void*, BufferVector<Buffer*>> elemToBufferFast;
    absl::flat_hash_map<const void*, BufferVector<Buffer*>> elemToBufferGeneral;
    for (auto&& path : paths) {
        auto elems = path.first.elementsToMaterialize(refObj);
        if (elems.empty()) {
            // Use nullptr as a sentinel for paths that don't map to any elements.
            elemToBufferFast[nullptr].push_back(&path.second);
        } else if (elems.size() == 1 && scalarElems.contains(elems[0])) {
            elemToBufferFast[elems[0]].push_back(&path.second);
        } else {
            for (const void* valueAddr : elems) {
                elemToBufferGeneral[valueAddr].push_back(&path.second);
            }
        }
    }

    // If there were any paths that needed to use the general pass, then do that now.
    const char* newGeneralControl = nullptr;
    if (!elemToBufferGeneral.empty()) {
        newGeneralControl = decompressGeneral(std::move(elemToBufferGeneral));
    }

    // There are now a couple possibilities:
    // - There are paths that use the fast implementation. In that case, do so.
    // - All the paths produce zero elements for this reference object (i.e., paths requesting a
    //   field that does not exist). In that case call decompressFast() with the empty hash map
    //   purely to advance to the next control byte.
    const char* newFastControl = nullptr;
    if (!elemToBufferFast.empty() || newGeneralControl == nullptr) {
        newFastControl = decompressFast(std::move(elemToBufferFast));
    }

    // We need to have taken either the general or the fast path, in order to tell the caller where
    // the interleaved data ends.
    invariant(newGeneralControl != nullptr || newFastControl != nullptr,
              "either the general or fast impl must have been used");

    // Ensure that if we had paths for both the fast and general case that the location of the new
    // control byte is the same.
    invariant(newGeneralControl == nullptr || newFastControl == nullptr ||
                  newGeneralControl == newFastControl,
              "fast impl and general impl control byte location does not agree");

    // In either case, we return a pointer to the byte after the EOO that ends interleaved mode.
    return newFastControl == nullptr ? newGeneralControl : newFastControl;
}

/**
 * Decompresses interleaved data that starts at "control", with data at a given path sent to the
 * corresonding buffer. Returns a pointer to the next byte after the interleaved data.
 */
template <typename Buffer>
const char* BlockBasedInterleavedDecompressor::decompressGeneral(
    absl::flat_hash_map<const void*, BufferVector<Buffer*>>&& elemToBuffer) {
    const char* control = _control;

    // The reference object will appear right after the control byte that starts interleaved
    // mode.
    BSONObj refObj{control + 1};

    // A vector that maps the ordinal position of the pre-order traversal of the reference
    // object to the buffers where that element should be materialized. The length of the vector
    // will be the same as the number of elements in the reference object, with empty vectors
    // for those elements that aren't being materialized.
    //
    // Use BufferVector, which is optimized for one element, because there will almost always be
    // just one buffer.
    std::vector<BufferVector<Buffer*>> posToBuffers;

    // Decoding states for each scalar field appearing in the reference object, in pre-order
    // traversal order.
    std::vector<DecodingState> decoderStates;

    // A map from the buffer address to the position information of the values decompressed in that
    // buffer. Each buffer must have its own position information. Each value in the map represents
    // the number of values decompressed for that path for one document.
    absl::flat_hash_map<Buffer*, int32_t> bufferToPositions;

    {
        BSONObjTraversal trInit{
            _traverseArrays,
            _rootType,
            [&](StringData fieldName, const BSONObj& obj, BSONType type) {
                if (auto it = elemToBuffer.find(obj.objdata()); it != elemToBuffer.end()) {
                    if constexpr (Buffer::kCollectsPositionInfo) {
                        for (auto&& buf : it->second) {
                            bufferToPositions[buf] = 0;
                        }
                    }
                    posToBuffers.push_back(std::move(it->second));
                } else {
                    // An empty list to indicate that this element isn't being materialized.
                    posToBuffers.push_back({});
                }

                return true;
            },
            [&](const BSONElement& elem) {
                decoderStates.emplace_back();
                decoderStates.back().loadUncompressed(elem);
                if (auto it = elemToBuffer.find(elem.value()); it != elemToBuffer.end()) {
                    posToBuffers.push_back(std::move(it->second));
                } else {
                    // An empty list to indicate that this element isn't being materialized.
                    posToBuffers.push_back({});
                }
                return true;
            }};
        trInit.traverse(refObj);
    }

    // Advance past the reference object to the compressed data of the first field.
    control += refObj.objsize() + 1;
    uassert(8625732, "Invalid BSON Column encoding", _control < _end);

    using SOAlloc = SubObjectAllocator<BlockBasedSubObjectFinisher<Buffer>>;
    using OptionalSOAlloc = boost::optional<SOAlloc>;
    static_assert(std::is_move_constructible<OptionalSOAlloc>::value,
                  "must be able to move a sub-object allocator to ensure that RAII properties "
                  "are followed");

    /*
     * Each traversal of the reference object can potentially produce a value for each path
     * passed in by the caller. For the root object or sub-objects that are to be materialized,
     * we create an instance of SubObjectAllocator to create the object.
     */
    int scalarIdx = 0;
    int nodeIdx = 0;
    BSONObjTraversal trDecompress{
        _traverseArrays,
        _rootType,
        [&](StringData fieldName, const BSONObj& obj, BSONType type) -> OptionalSOAlloc {
            auto& buffers = posToBuffers[nodeIdx];
            ++nodeIdx;

            if (!buffers.empty() || _allocator.contiguousEnabled()) {
                // If we have already entered contiguous mode, but there are buffers
                // corresponding to this subobject, that means caller has requested nested
                // paths, e.g., "a" and "a.b".
                //
                // TODO(SERVER-86220): Nested paths dosn't seem like they would be common, but
                // we should be able to handle it.
                invariant(buffers.empty() || !_allocator.contiguousEnabled(),
                          "decompressing paths with a nested relationship is not yet supported");

                // Either caller requested that this sub-object be materialized to a
                // container, or we are already materializing this object because it is
                // contained by such a sub-object.
                //
                // Only increment the position information if the 'Container' requests position
                // information, and if we are materializing the top-level object, not an object
                // contained by another sub-object.
                if constexpr (Buffer::kCollectsPositionInfo) {
                    if (!_allocator.contiguousEnabled()) {
                        for (auto&& buffer : buffers) {
                            bufferToPositions[buffer]++;
                        }
                    }
                }

                return SOAlloc(
                    _allocator, fieldName, obj, type, BlockBasedSubObjectFinisher{buffers});
            }

            return boost::none;
        },
        [&](const BSONElement& referenceField) {
            auto& state = decoderStates[scalarIdx];
            ++scalarIdx;

            auto& buffers = posToBuffers[nodeIdx];
            ++nodeIdx;

            // Get the next element for this scalar field.
            DecodingState::Elem decodingStateElem;
            if (auto d64 = get_if<DecodingState::Decoder64>(&state.decoder);
                d64 && d64->pos.valid() && d64->pos.more()) {
                // We have an iterator into a block of deltas
                decodingStateElem = state.loadDelta(_allocator, *d64);
                ++d64->pos;
            } else if (auto d128 = get_if<DecodingState::Decoder128>(&state.decoder);
                       d128 && d128->pos.valid() && d128->pos.more()) {
                // We have an iterator into a block of deltas
                decodingStateElem = state.loadDelta(_allocator, *d128);
                ++d128->pos;
            } else {
                invariant(*control != EOO, "moreData() should ensure we terminate loop at EOO");
                // No more deltas for this scalar field. The next control byte is guaranteed
                // to belong to this scalar field, since traversal order is fixed.
                auto result = state.loadControl(_allocator, control);
                control += result.size;
                uassert(8625731, "Invalid BSON Column encoding", _control < _end);
                decodingStateElem = result.element;
            }

            // If caller has requested materialization of this field, do it.
            if (_allocator.contiguousEnabled()) {
                // TODO(SERVER-86220): Nested paths dosn't seem like they would be common, but
                // we should be able to handle it.
                invariant(buffers.empty(),
                          "decompressing paths with a nested relationship is not yet supported");

                // We must write a BSONElement to ElementStorage since this scalar is part
                // of an object being materialized.
                writeToElementStorage(
                    decodingStateElem, state._lastLiteral, referenceField.fieldNameStringData());
            } else if (buffers.size() > 0) {
                // This scalar is not part of an object being materialized. Increment the position
                // counter if the 'Container' is requesting position information.
                if constexpr (Buffer::kCollectsPositionInfo) {
                    for (auto&& buffer : buffers) {
                        bufferToPositions[buffer]++;
                    }
                }
                appendToBuffers(buffers, decodingStateElem, state._lastLiteral);
            }

            return true;
        }};

    while (moreData(decoderStates[0], control)) {
        scalarIdx = 0;
        nodeIdx = 0;

        // In each iteration of the traverse we produce one document. After decompressing a single
        // document, we will push how many elements were materialized for the specific path if the
        // 'Container' requests the position information to be collected.
        trDecompress.traverse(refObj);
        flushPositionsToBuffers(bufferToPositions);
    }

    invariant(*control == EOO, "expected EOO that ends interleaved mode");

    // Advance past the EOO that ends interleaved mode.
    ++control;
    return control;
}

inline bool BlockBasedInterleavedDecompressor::moreData(DecodingState& ds, const char* control) {
    return visit(OverloadedVisitor{[&](typename DecodingState::Decoder64& d64) {
                                       if (!d64.pos.valid() || !d64.pos.more()) {
                                           // We need to load the next control byte. Is interleaved
                                           // mode continuing?
                                           return *control != EOO;
                                       }

                                       return true;
                                   },
                                   [&](typename DecodingState::Decoder128& d128) {
                                       if (!d128.pos.valid() || !d128.pos.more()) {
                                           // We need to load the next control byte. Is interleaved
                                           // mode continuing?
                                           return *control != EOO;
                                       }

                                       return true;
                                   }},
                 ds.decoder);
}

/**
 * TODO: This code cloned and modified from bsoncolumn.inl. We should be sharing this code.
 */
template <typename T, typename Encoding, class Buffer, typename Materialize, typename Finish>
void BlockBasedInterleavedDecompressor::decompressAllDelta(const char* ptr,
                                                           const char* end,
                                                           Buffer& buffer,
                                                           Encoding last,
                                                           const BSONElement& reference,
                                                           const Materialize& materialize,
                                                           const Finish& finish) {
    size_t elemCount = 0;
    uint8_t size = numSimple8bBlocksForControlByte(*ptr) * sizeof(uint64_t);
    Simple8b<make_unsigned_t<Encoding>> s8b(ptr + 1, size);

    auto it = s8b.begin();
    // process all copies of the reference object efficiently
    // this can otherwise get more complicated on string/binary types
    for (; it != s8b.end(); ++it) {
        const auto& delta = *it;
        if (delta) {
            if (*delta == 0) {
                buffer.template append<T>(reference);
                ++elemCount;
            } else {
                break;
            }
        } else {
            buffer.appendMissing();
            ++elemCount;
        }
    }

    for (; it != s8b.end(); ++it) {
        const auto& delta = *it;
        if (delta) {
            last =
                expandDelta(last, Simple8bTypeUtil::decodeInt<make_unsigned_t<Encoding>>(*delta));
            materialize(last, reference, buffer);
            ++elemCount;
        } else {
            buffer.appendMissing();
            ++elemCount;
        }
    }

    finish(elemCount, last);
}

/**
 * A data structure that tracks the state of a stream of scalars that appears in interleaved
 * BSONColumn data. This structure is used with a min heap to understand which bits of compressed
 * data belong to which stream.
 */
template <typename Buffer>
struct BlockBasedInterleavedDecompressor::FastDecodingState {

    FastDecodingState(size_t fieldPos,
                      const BSONElement& refElem,
                      BufferVector<Buffer*>&& buffers = {})
        : _valueCount(0), _fieldPos(fieldPos), _refElem(refElem), _buffers(std::move(buffers)) {}


    // The number of values seen so far by this stream.
    size_t _valueCount;

    // The ordinal position in the reference object to which this stream corresponds.
    size_t _fieldPos;

    // The most recent uncompressed element for this stream.
    BSONElement _refElem;

    // The list of buffers to which this stream must be materialized. For streams that aren't
    // materialized, this will be empty.
    BufferVector<Buffer*> _buffers;

    // The last uncompressed value for this stream. The delta is applied against this to compute a
    // new uncompressed value.
    std::variant<int64_t, int128_t> _lastValue;

    // Given the current reference element, set _lastValue.
    void setLastValueFromBSONElem() {
        switch (_refElem.type()) {
            case Bool:
                _lastValue.emplace<int64_t>(_refElem.boolean());
                break;
            case NumberInt:
                _lastValue.emplace<int64_t>(_refElem._numberInt());
                break;
            case NumberLong:
                _lastValue.emplace<int64_t>(_refElem._numberLong());
                break;
            default:
                invariant(false, "unsupported type");
        }
    }

    bool operator>(const FastDecodingState& other) {
        return std::tie(_valueCount, _fieldPos) > std::tie(other._valueCount, other._fieldPos);
    }
};

/**
 * The fast path for those paths that are only materializing a single scalar field.
 */
template <typename Buffer>
const char* BlockBasedInterleavedDecompressor::decompressFast(
    absl::flat_hash_map<const void*, BufferVector<Buffer*>>&& elemToBuffer) {
    const char* control = _control;

    // The reference object will appear right after the control byte that starts interleaved
    // mode.
    BSONObj refObj{control + 1};
    control += refObj.objsize() + 1;
    uassert(8625730, "Invalid BSON Column encoding", _control < _end);

    /**
     * The code below uses std::make_heap(), etc such that the element at the top of the heap always
     * represents the stream assigned to the next control byte. The stream that has processed the
     * fewest number of elements will be at the top, with the stream's ordinal position in the
     * reference object used to break ties.
     *
     * For streams that are being materialized to a buffer, we materialize uncompressed elements as
     * well as expanded elements produced by simple8b blocks.
     *
     * For streams that are not being materialized, when we encounter simpl8b blocks, we create a
     * simple8b iterator and advance it in a tight loop just to count the number of elements in the
     * stream.
     */

    // Initialize a vector of states.
    std::vector<FastDecodingState<Buffer>> heap;
    size_t scalarIdx = 0;
    BSONObjTraversal trInit{
        _traverseArrays,
        _rootType,
        [&](StringData fieldName, const BSONObj& obj, BSONType type) { return true; },
        [&](const BSONElement& elem) {
            if (auto it = elemToBuffer.find(elem.value()); it != elemToBuffer.end()) {
                heap.emplace_back(scalarIdx, elem, std::move(it->second));
            } else {
                heap.emplace_back(scalarIdx, elem);
            }
            heap.back().setLastValueFromBSONElem();
            ++scalarIdx;
            return true;
        },
    };
    trInit.traverse(refObj);

    // Use greater() so that we have a min heap, i.e., the streams that have processed the fewest
    // elements are at the top.
    std::make_heap(heap.begin(), heap.end(), std::greater<>());

    // Iterate over the control bytes that appear in this section of interleaved data.
    while (*control != EOO) {
        std::pop_heap(heap.begin(), heap.end(), std::greater<>());
        FastDecodingState<Buffer>& state = heap.back();
        if (isUncompressedLiteralControlByte(*control)) {
            state._refElem = BSONElement{control, 1, -1};
            for (auto&& b : state._buffers) {
                b->template append<BSONElement>(state._refElem);
            }
            state.setLastValueFromBSONElem();
            ++state._valueCount;
            control += state._refElem.size();
        } else {
            uint8_t size = numSimple8bBlocksForControlByte(*control) * sizeof(uint64_t);
            if (state._buffers.empty()) {
                // simple8b blocks for a stream that we are not materializing. Just skip over the
                // deltas, keeping track of how many elements there were.
                state._valueCount += numElemsForControlByte(control);
            } else {
                // simple8b blocks for a stream that we are materializing.
                auto finish64 = [&state](size_t valueCount, int64_t lastValue) {
                    state._valueCount += valueCount;
                    state._lastValue.template emplace<int64_t>(lastValue);
                };
                switch (state._refElem.type()) {
                    case Bool:
                        for (auto&& buffer : state._buffers) {
                            decompressAllDelta<bool, int64_t, Buffer>(
                                control,
                                control + size + 1,
                                *buffer,
                                std::get<int64_t>(state._lastValue),
                                state._refElem,
                                [](const int64_t v, const BSONElement& ref, Buffer& buffer) {
                                    buffer.append(static_cast<bool>(v));
                                },
                                finish64);
                        }
                        break;
                    case NumberInt:
                        for (auto&& buffer : state._buffers) {
                            decompressAllDelta<int32_t, int64_t, Buffer>(
                                control,
                                control + size + 1,
                                *buffer,
                                std::get<int64_t>(state._lastValue),
                                state._refElem,
                                [](const int64_t v, const BSONElement& ref, Buffer& buffer) {
                                    buffer.append(static_cast<int32_t>(v));
                                },
                                finish64);
                        }
                        break;
                    case NumberLong:
                        for (auto&& buffer : state._buffers) {
                            decompressAllDelta<int64_t, int64_t, Buffer>(
                                control,
                                control + size + 1,
                                *buffer,
                                std::get<int64_t>(state._lastValue),
                                state._refElem,
                                [](const int64_t v, const BSONElement& ref, Buffer& buffer) {
                                    buffer.append(v);
                                },
                                finish64);
                        }
                        break;
                    default:
                        invariant(false, "unsupported type");
                }
            }
            control += (1 + size);
        }
        std::push_heap(heap.begin(), heap.end(), std::greater<>());
    }

    // If there were paths that don't match anything, call appendMissing().
    if (auto it = elemToBuffer.find(nullptr); it != elemToBuffer.end()) {
        // All the decoder states should have the same value count.
        auto valueCount = heap[0]._valueCount;
        for (auto&& buffer : it->second) {
            for (size_t i = 0; i < valueCount; ++i) {
                buffer->appendMissing();
                if constexpr (Buffer::kCollectsPositionInfo) {
                    buffer->appendPositionInfo(1);
                }
            }
        }
    }

    // Since we are guaranteed to decompress scalars or missing, we can append 1 to the position
    // information for all of the buffers for all of the values if the 'Container' requested to
    // collect the position information.
    if constexpr (Buffer::kCollectsPositionInfo) {
        for (auto&& elem : heap) {
            for (auto&& buffer : elem._buffers) {
                for (size_t i = 0; i < elem._valueCount; ++i) {
                    buffer->appendPositionInfo(1);
                }
            }
        }
    }

    // Advance past the EOO that ends interleaved mode.
    ++control;
    return control;
}

template <class Buffer>
void BlockBasedInterleavedDecompressor::appendToBuffers(BufferVector<Buffer*>& buffers,
                                                        DecodingState::Elem elem,
                                                        BSONElement lastLiteral) {
    visit(OverloadedVisitor{
              [&](BSONElement& bsonElem) {
                  if (bsonElem.eoo()) {
                      for (auto&& b : buffers) {
                          b->appendMissing();
                      }
                  } else {
                      for (auto&& b : buffers) {
                          b->template append<BSONElement>(bsonElem);
                      }
                  }
              },
              [&](std::pair<BSONType, int64_t>& encoded) {
                  switch (encoded.first) {
                      case Date:
                          appendEncodedToBuffers<Buffer, Date_t>(
                              buffers, Date_t::fromMillisSinceEpoch(encoded.second));
                          break;
                      case NumberLong:
                          appendEncodedToBuffers<Buffer, int64_t>(buffers, encoded.second);
                          break;
                      case NumberInt:
                          appendEncodedToBuffers<Buffer, int32_t>(buffers, encoded.second);
                          break;
                      case Bool:
                          appendEncodedToBuffers<Buffer, bool>(buffers, encoded.second);
                          break;
                      case jstOID:
                          appendEncodedToBuffers<Buffer, OID>(
                              buffers,
                              Simple8bTypeUtil::decodeObjectId(
                                  encoded.second, lastLiteral.__oid().getInstanceUnique()));
                          break;
                      case bsonTimestamp:
                          appendEncodedToBuffers<Buffer, Timestamp>(
                              buffers, static_cast<Timestamp>(encoded.second));
                          break;
                      default:
                          invariant(false, "unsupported encoded data type");
                  }
              },
              [&](std::pair<BSONType, int128_t>& encoded) {
                  switch (encoded.first) {
                      case String: {
                          auto string = Simple8bTypeUtil::decodeString(encoded.second);
                          appendEncodedToBuffers<Buffer, StringData>(
                              buffers, StringData((const char*)string.str.data(), string.size));
                      } break;
                      case Code: {
                          auto string = Simple8bTypeUtil::decodeString(encoded.second);
                          appendEncodedToBuffers<Buffer, BSONCode>(
                              buffers,
                              BSONCode(StringData((const char*)string.str.data(), string.size)));
                      } break;
                      case BinData: {
                          char data[16];
                          size_t size = lastLiteral.valuestrsize();
                          Simple8bTypeUtil::decodeBinary(encoded.second, data, size);
                          appendEncodedToBuffers<Buffer, BSONBinData>(
                              buffers, BSONBinData(data, size, lastLiteral.binDataType()));
                      } break;
                      case NumberDecimal:
                          appendEncodedToBuffers<Buffer, Decimal128>(
                              buffers, Simple8bTypeUtil::decodeDecimal128(encoded.second));
                          break;
                      default:
                          invariant(false, "unsupported encoded data type");
                  }
              },
          },
          elem);
}

template <class Buffer>
void BlockBasedInterleavedDecompressor::flushPositionsToBuffers(
    absl::flat_hash_map<Buffer*, int32_t>& bufferToPositions) {
    if constexpr (Buffer::kCollectsPositionInfo) {
        for (auto&& bufferAndPos : bufferToPositions) {
            bufferAndPos.first->appendPositionInfo(bufferAndPos.second);
            bufferAndPos.second = 0;
        }
    }
}
}  // namespace mongo::bsoncolumn
