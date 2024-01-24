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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/util/bsoncolumn_util.h"
#include "mongo/bson/util/simple8b.h"
#include "mongo/bson/util/simple8b_type_util.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo {

class ElementStorage;

namespace bsoncolumn {

/**
 * Helper class to perform recursion over a BSONObj. Two functions are provided:
 *
 * EnterSubObjFunc is called before recursing deeper, it may output an RAII object to perform logic
 * when entering/exiting a subobject.
 *
 * ElementFunc is called for every non-object element encountered during the recursion.
 */
template <typename EnterSubObjFunc, typename ElementFunc>
class BSONObjTraversal {
public:
    BSONObjTraversal(bool recurseIntoArrays,
                     BSONType rootType,
                     EnterSubObjFunc enterFunc,
                     ElementFunc elemFunc)
        : _enterFunc(std::move(enterFunc)),
          _elemFunc(std::move(elemFunc)),
          _recurseIntoArrays(recurseIntoArrays),
          _rootType(rootType) {}

    bool traverse(const BSONObj& obj) {
        if (_recurseIntoArrays) {
            return _traverseIntoArrays(""_sd, obj, _rootType);
        } else {
            return _traverseNoArrays(""_sd, obj, _rootType);
        }
    }

private:
    bool _traverseNoArrays(StringData fieldName, const BSONObj& obj, BSONType type) {
        [[maybe_unused]] auto raii = _enterFunc(fieldName, obj, type);

        return std::all_of(obj.begin(), obj.end(), [this, &fieldName](auto&& elem) {
            return elem.type() == Object
                ? _traverseNoArrays(elem.fieldNameStringData(), elem.Obj(), Object)
                : _elemFunc(elem);
        });
    }

    bool _traverseIntoArrays(StringData fieldName, const BSONObj& obj, BSONType type) {
        [[maybe_unused]] auto raii = _enterFunc(fieldName, obj, type);

        return std::all_of(obj.begin(), obj.end(), [this, &fieldName](auto&& elem) {
            return elem.type() == Object || elem.type() == Array
                ? _traverseIntoArrays(elem.fieldNameStringData(), elem.Obj(), elem.type())
                : _elemFunc(elem);
        });
    }

    EnterSubObjFunc _enterFunc;
    ElementFunc _elemFunc;
    bool _recurseIntoArrays;
    BSONType _rootType;
};

/**
 * A helper class for block-based decompression of object data.
 */
template <class CMaterializer>
class BlockBasedInterleavedDecompressor {
public:
    /**
     * Decompresses interleaved data that starts at "control", with data at a given path sent to the
     * corresonding buffer. Returns a pointer to the next byte after the interleaved data.
     */
    template <typename Path, typename Buffer>
    static const char* decompress(ElementStorage& allocator,
                                  const char* control,
                                  const char* end,
                                  std::vector<std::pair<Path, Buffer>>& paths) {
        invariant(bsoncolumn::isInterleavedStartControlByte(*control),
                  "request to do interleaved decompression on non-interleaved data");
        BSONType rootType =
            *control == bsoncolumn::kInterleavedStartArrayRootControlByte ? Array : Object;
        bool traverseArrays = *control == bsoncolumn::kInterleavedStartControlByte ||
            *control == bsoncolumn::kInterleavedStartArrayRootControlByte;

        // Create a map of fields in the reference object to the buffers to where those fields are
        // to be decompressed.
        BSONObj refObj{control + 1};
        absl::flat_hash_map<const char*, std::vector<Buffer>> elemToBuffer;
        for (auto&& path : paths) {
            for (const char* valueAddr : path.first.elementsToMaterialize(refObj)) {
                elemToBuffer[valueAddr].push_back(path.second);
            }
        }

        // Initialize decoding state for each scalar field of the reference object.
        std::vector<DecodingState> states;
        BSONObjTraversal trInit{
            traverseArrays,
            rootType,
            [&elemToBuffer](StringData fieldName, const BSONObj& obj, BSONType type) {
                invariant(!elemToBuffer.contains(obj.objdata()),
                          "materializing non-scalars not unsupported yet");
                return true;
            },
            [&elemToBuffer, &states](const BSONElement& elem) {
                states.emplace_back();
                states.back().loadUncompressed(elem);
                return true;
            }};
        trInit.traverse(refObj);

        // Advance past the reference object to the compressed data of the first field.
        control += refObj.objsize() + 1;
        invariant(control < end);

        int idx = 0;
        BSONObjTraversal trDecompress{
            traverseArrays,
            rootType,
            [](StringData fieldName, const BSONObj& obj, BSONType type) { return true; },
            [&](const BSONElement& referenceField) {
                auto& state = states[idx];
                ++idx;
                invariant(
                    (std::holds_alternative<typename DecodingState::Decoder64>(state.decoder)),
                    "only supporting 64-bit encoding for now");
                auto& d64 = std::get<typename DecodingState::Decoder64>(state.decoder);

                // Get the next element for this scalar field.
                typename DecodingState::Elem elem;
                if (d64.pos.valid() && (++d64.pos).more()) {
                    // We have an iterator into a block of deltas
                    elem = state.loadDelta(allocator, d64);
                } else if (*control == EOO) {
                    // End of interleaved mode. Stop object traversal early by returning false.
                    return false;
                } else {
                    // No more deltas for this scalar field. The next control byte is guaranteed to
                    // belong to this scalar field, since traversal order is fixed.
                    auto result = state.loadControl(allocator, control);
                    control += result.size;
                    invariant(control < end);
                    elem = result.element;
                }

                // If caller has requested materialization of this field, do it.
                if (auto it = elemToBuffer.find(referenceField.value()); it != elemToBuffer.end()) {
                    auto& buffers = it->second;
                    appendToBuffers(buffers, elem);
                }

                return true;
            }};

        bool more = true;
        while (more || *control != EOO) {
            idx = 0;
            more = trDecompress.traverse(refObj);
        }

        // Advance past the EOO that ends interleaved mode.
        ++control;
        return control;
    }

private:
    struct DecodingState;

    template <class Buffer>
    static void appendToBuffers(std::vector<Buffer>& buffers, typename DecodingState::Elem elem) {
        visit(OverloadedVisitor{
                  [&](BSONElement& bsonElem) {
                      if (bsonElem.eoo()) {
                          for (auto&& c : buffers) {
                              c.appendMissing();
                          }
                      } else {
                          for (auto&& c : buffers) {
                              c.template append<BSONElement>(bsonElem);
                          }
                      }
                  },
                  [&](std::pair<BSONType, int64_t>& encoded) {
                      switch (encoded.first) {
                          case NumberLong:
                              appendEncodedToBuffers<Buffer, int64_t>(buffers, encoded.second);
                              break;
                          case NumberInt:
                              appendEncodedToBuffers<Buffer, int32_t>(buffers, encoded.second);
                              break;
                          case Bool:
                              appendEncodedToBuffers<Buffer, bool>(buffers, encoded.second);
                              break;
                          default:
                              invariant(false, "unsupported encoded data type");
                      }
                  },
                  [&](std::pair<BSONType, int128_t>& encoded) {
                      invariant(false, "128-bit encoded types not supported yet");
                  },
              },
              elem);
    }

    template <typename Buffer, typename T>
    static void appendEncodedToBuffers(std::vector<Buffer>& buffers, int64_t encoded) {
        for (auto b : buffers) {
            b.append(static_cast<T>(encoded));
        }
    }

    /**
     * Decoding state for a stream of values corresponding to a scalar field.
     */
    struct DecodingState {

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
            boost::optional<int64_t> lastEncodedValue;
            Simple8b<uint64_t>::Iterator pos;
        };

        /**
         * State when decoding deltas for 128-bit values. (TBD)
         */
        struct Decoder128 {};

        /**
         * Initializes a decoder given an uncompressed BSONElement in the BSONColumn bytes.
         */
        void loadUncompressed(const BSONElement& elem) {
            BSONType type = elem.type();
            invariant(!uses128bit(type));
            invariant(!usesDeltaOfDelta(type));
            auto& d64 = decoder.template emplace<Decoder64>();
            switch (type) {
                case Bool:
                    d64.lastEncodedValue = elem.boolean();
                    break;
                case NumberInt:
                    d64.lastEncodedValue = elem._numberInt();
                    break;
                case NumberLong:
                    d64.lastEncodedValue = elem._numberLong();
                    break;
                default:
                    invariant(false, "unsupported type");
            }

            _lastLiteral = elem;
        }

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
        LoadControlResult loadControl(ElementStorage& allocator, const char* buffer) {
            uint8_t control = *buffer;
            if (isUncompressedLiteralControlByte(control)) {
                BSONElement literalElem(buffer, 1, -1);
                return {literalElem, literalElem.size()};
            }

            uint8_t blocks = numSimple8bBlocksForControlByte(control);
            int size = sizeof(uint64_t) * blocks;

            auto& d64 = std::get<DecodingState::Decoder64>(decoder);
            // We can read the last known value from the decoder iterator even as it has
            // reached end.
            boost::optional<uint64_t> lastSimple8bValue = d64.pos.valid() ? *d64.pos : 0;
            d64.pos = Simple8b<uint64_t>(buffer + 1, size, lastSimple8bValue).begin();
            Elem deltaElem = loadDelta(allocator, d64);
            return LoadControlResult{deltaElem, size + 1};
        }

        /**
         * Apply a delta to an encoded representation to get a new element value. May also apply a 0
         * delta to an uncompressed literal, simply returning the literal.
         */
        Elem loadDelta(ElementStorage& allocator, Decoder64& d64) {
            invariant(d64.pos.valid());
            const auto& delta = *d64.pos;
            if (!delta) {
                // boost::none represents skip, just return an EOO BSONElement.
                return BSONElement{};
            }

            // Note: delta-of-delta not handled here yet.
            if (*delta == 0) {
                // If we have an encoded representation of the last value, return it.
                if (d64.lastEncodedValue) {
                    return std::pair{_lastLiteral.type(), *d64.lastEncodedValue};
                }
                // Otherwise return the last uncompressed value we found.
                return _lastLiteral;
            }

            invariant(d64.lastEncodedValue,
                      "attempt to expand delta for type that does not have encoded representation");
            d64.lastEncodedValue =
                expandDelta(*d64.lastEncodedValue, Simple8bTypeUtil::decodeInt64(*delta));

            return std::pair{_lastLiteral.type(), *d64.lastEncodedValue};
        }

        /**
         * The last uncompressed literal from the BSONColumn bytes.
         */
        BSONElement _lastLiteral;

        /**
         * 64- or 128-bit specific state.
         */
        std::variant<Decoder64, Decoder128> decoder = Decoder64{};
    };
};

/**
 * Implements the "materializer" concept such that the output elements are BSONElements.
 */
class BSONElementMaterializer {
public:
    using Element = BSONElement;
    using Allocator = ElementStorage;

    static BSONElement materialize(Allocator& allocator, bool val);
    static BSONElement materialize(Allocator& allocator, int32_t val);
    static BSONElement materialize(Allocator& allocator, int64_t val);
    static BSONElement materialize(Allocator& allocator, double val);
    static BSONElement materialize(Allocator& allocator, const Decimal128& val);
    static BSONElement materialize(Allocator& allocator, const Date_t& val);
    static BSONElement materialize(Allocator& allocator, const Timestamp& val);
    static BSONElement materialize(Allocator& allocator, StringData val);
    static BSONElement materialize(Allocator& allocator, const BSONBinData& val);
    static BSONElement materialize(Allocator& allocator, const BSONCode& val);
    static BSONElement materialize(Allocator& allocator, const OID& val);

    template <typename T>
    static BSONElement materialize(Allocator& allocator, BSONElement val) {
        return val;
    }

    static BSONElement materializeMissing(Allocator& allocator) {
        return BSONElement();
    }

private:
    /**
     * Helper function used by both BSONCode and String.
     */
    static BSONElement writeStringData(ElementStorage& allocator,
                                       BSONType bsonType,
                                       StringData val);
};

}  // namespace bsoncolumn
}  // namespace mongo
