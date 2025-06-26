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

#include "mongo/bson/bson_comparator_interface_base.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelementvalue.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/column/bsoncolumn_util.h"
#include "mongo/bson/column/simple8b.h"
#include "mongo/bson/column/simple8b_type_util.h"
#include "mongo/bson/util/bsonobj_traversal.h"

#include <concepts>

#include <boost/container/small_vector.hpp>

namespace mongo {

namespace bsoncolumn {

/**
 * Interface for a buffer to receive decoded elements from block-based
 * BSONColumn decompression.
 */
template <class T>
concept Appendable = requires(
    T& t, StringData strVal, BSONBinData binVal, BSONCode codeVal, BSONElement bsonVal, int32_t n) {
    t.append(true);
    t.append((int32_t)1);
    t.append((int64_t)1);
    t.append(Decimal128());
    t.append((double)1.0);
    t.append((Timestamp)1);
    t.append(Date_t::now());
    t.append(OID::gen());
    t.append(strVal);
    t.append(binVal);
    t.append(codeVal);

    // Strings can arrive either in 128-bit encoded format, or as
    // literals (BSONElement)

    // Takes pre-allocated BSONElement
    t.template append<bool>(bsonVal);
    t.template append<int32_t>(bsonVal);
    t.template append<int64_t>(bsonVal);
    t.template append<Decimal128>(bsonVal);
    t.template append<double>(bsonVal);
    t.template append<Timestamp>(bsonVal);
    t.template append<Date_t>(bsonVal);
    t.template append<OID>(bsonVal);
    t.template append<StringData>(bsonVal);
    t.template append<BSONBinData>(bsonVal);
    t.template append<BSONCode>(bsonVal);
    t.template append<BSONElement>(bsonVal);

    t.appendPreallocated(bsonVal);

    t.appendPositionInfo(n);

    t.appendMissing();

    // Repeat the last appended value
    t.appendLast();
};

/**
 * Interface to accept elements decoded from BSONColumn and materialize them
 * as Elements of user-defined type.
 *
 * This class will be used with decompress() and other methods of BSONColumn to efficiently produce
 * values of the desired type (e.g., SBE values or BSONElements). The methods provided by
 * implementors of this concept will be called from the main decompression loop, so they should be
 * inlineable, and avoid branching and memory allocations when possible.
 *
 * The data types passed to the materialize() methods could be referencing memory on the stack
 * (e.g., the pointer in a StringData instance) and so implementors should assume this data is
 * ephemeral. The provided BSONElementStorage can be used to allocate memory with the lifetime of
 * the BSONColumn instance.
 *
 * The exception to this rule is that BSONElements passed to the materialize() methods may appear in
 * decompressed form as-is in the BSONColumn binary data. If they are as such, they will have the
 * same lifetime as the BSONColumn, and may go away if a yield of query execution occurs.
 * Implementers may wish to explicitly copy the value with the allocator in this case. It may also
 * occur that decompression allocates its own BSONElements as part of its execution (e.g., when
 * materializing whole objects from compressed scalars). In this case, decompression will invoke
 * materializePreallocated() instead of materialize().
 */
template <class T>
concept Materializer = requires(T& t,
                                BSONElementStorage& alloc,
                                StringData strVal,
                                BSONBinData binVal,
                                BSONCode codeVal,
                                BSONElement bsonVal) {
    typename T::Element;

    { T::materialize(alloc, true) } -> std::same_as<typename T::Element>;
    { T::materialize(alloc, (int32_t)1) } -> std::same_as<typename T::Element>;
    { T::materialize(alloc, (int64_t)1) } -> std::same_as<typename T::Element>;
    { T::materialize(alloc, Decimal128()) } -> std::same_as<typename T::Element>;
    { T::materialize(alloc, (double)1.0) } -> std::same_as<typename T::Element>;
    { T::materialize(alloc, (Timestamp)1) } -> std::same_as<typename T::Element>;
    { T::materialize(alloc, Date_t::now()) } -> std::same_as<typename T::Element>;
    { T::materialize(alloc, OID::gen()) } -> std::same_as<typename T::Element>;

    { T::materialize(alloc, strVal) } -> std::same_as<typename T::Element>;
    { T::materialize(alloc, binVal) } -> std::same_as<typename T::Element>;
    { T::materialize(alloc, codeVal) } -> std::same_as<typename T::Element>;

    { T::template materialize<bool>(alloc, bsonVal) } -> std::same_as<typename T::Element>;
    { T::template materialize<int32_t>(alloc, bsonVal) } -> std::same_as<typename T::Element>;
    { T::template materialize<int64_t>(alloc, bsonVal) } -> std::same_as<typename T::Element>;
    { T::template materialize<Decimal128>(alloc, bsonVal) } -> std::same_as<typename T::Element>;
    { T::template materialize<double>(alloc, bsonVal) } -> std::same_as<typename T::Element>;
    { T::template materialize<Timestamp>(alloc, bsonVal) } -> std::same_as<typename T::Element>;
    { T::template materialize<Date_t>(alloc, bsonVal) } -> std::same_as<typename T::Element>;
    { T::template materialize<OID>(alloc, bsonVal) } -> std::same_as<typename T::Element>;

    { T::template materialize<StringData>(alloc, bsonVal) } -> std::same_as<typename T::Element>;
    { T::template materialize<BSONBinData>(alloc, bsonVal) } -> std::same_as<typename T::Element>;
    { T::template materialize<BSONCode>(alloc, bsonVal) } -> std::same_as<typename T::Element>;

    { T::materializePreallocated(bsonVal) } -> std::same_as<typename T::Element>;

    { T::materializeMissing(alloc) } -> std::same_as<typename T::Element>;

    { T::isMissing(typename T::Element()) } -> std::same_as<bool>;

    { T::canonicalType(typename T::Element()) } -> std::same_as<int>;

    {
        T::compare(
            typename T::Element(), typename T::Element(), (const StringDataComparator*)nullptr)
    } -> std::same_as<int>;
};

/**
 * Interface to indicate to the 'Collector' at compile time if the user requested the decompressor
 * to collect the position information of values within documents.
 */
template <typename T>
concept PositionInfoAppender = requires(T& t, int32_t n) {
    { t.appendPositionInfo(n) } -> std::same_as<void>;
};

/**
 * Helpers for block decompress-all functions
 * T - the type we are decompressing to
 * Encoding - the underlying encoding (int128_t or int64_t) for Simple8b deltas
 * Buffer - the buffer being filled by decompress()
 * Materialize - function to convert delta decoding into T and append to Buffer
 * Finish - after completion, receives the count of elements and the final element
 */

// TODO:  Materialize is used in some places to refer converting int encodings to
// concrete types, and in other places to refer to converting concrete types to
// a desired output type.  Here we use it to refer to a composite of these two
// actions; we should take the time to make our terminology consistent.

class BSONColumnBlockDecompressHelpers {
public:
    template <typename T, typename Encoding, class Buffer, typename Materialize, typename Finish>
    requires Appendable<Buffer>
    static const char* decompressAllDelta(const char* ptr,
                                          const char* end,
                                          Buffer& buffer,
                                          Encoding last,
                                          uint64_t lastNonRLEBlock,
                                          const BSONElement& reference,
                                          const Materialize& materialize,
                                          const Finish& finish) {
        // iterate until we stop seeing simple8b block sequences
        size_t elemCount = 0;
        while (ptr < end) {
            uint8_t control = *ptr;
            if (control == stdx::to_underlying(BSONType::eoo) ||
                isUncompressedLiteralControlByte(control) || isInterleavedStartControlByte(control))
                return ptr;

            uassert(8873800,
                    "Invalid control byte in BSON Column",
                    bsoncolumn::scaleIndexForControlByte(control) ==
                        Simple8bTypeUtil::kMemoryAsInteger);

            uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);

            elemCount += simple8b::visitAll<Encoding>(
                ptr + 1,
                size,
                lastNonRLEBlock,
                [&materialize, &buffer, &reference, &last](const Encoding v) {
                    if (v == 0)
                        buffer.appendLast();
                    else {
                        last = expandDelta(last, v);
                        materialize(last, reference, buffer);
                    }
                },
                [&buffer]() { buffer.appendLast(); },
                [&buffer]() { buffer.appendMissing(); });

            ptr += 1 + size;
        }

        finish(elemCount, last, lastNonRLEBlock);
        return ptr;
    }

    template <typename T, typename Encoding, class Buffer, typename Materialize>
    requires Appendable<Buffer>
    static const char* decompressAllDelta(const char* ptr,
                                          const char* end,
                                          Buffer& buffer,
                                          Encoding last,
                                          const BSONElement& reference,
                                          const Materialize& materialize) {
        return decompressAllDelta<T>(ptr,
                                     end,
                                     buffer,
                                     last,
                                     simple8b::kSingleZero, /* lastNonRLEBlock */
                                     reference,
                                     materialize,
                                     [](size_t count, Encoding last, uint64_t lastNonRLEBlock) {});
    }

    /* Like decompressAllDelta, but does not have branching to avoid re-materialization
       of repeated values, intended to be used on primitive types where this does not
       result in additional allocation */
    template <typename T, typename Encoding, class Buffer, typename Materialize, typename Finish>
    requires Appendable<Buffer>
    static const char* decompressAllDeltaPrimitive(const char* ptr,
                                                   const char* end,
                                                   Buffer& buffer,
                                                   Encoding last,
                                                   uint64_t lastNonRLEBlock,
                                                   const BSONElement& reference,
                                                   const Materialize& materialize,
                                                   const Finish& finish) {
        // iterate until we stop seeing simple8b block sequences
        size_t elemCount = 0;
        while (ptr < end) {
            uint8_t control = *ptr;
            if (control == stdx::to_underlying(BSONType::eoo) ||
                isUncompressedLiteralControlByte(control) || isInterleavedStartControlByte(control))
                return ptr;

            uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
            uassert(8762800,
                    "Invalid control byte in BSON Column",
                    bsoncolumn::scaleIndexForControlByte(control) ==
                        Simple8bTypeUtil::kMemoryAsInteger);

            elemCount += simple8b::visitAll<Encoding>(
                ptr + 1,
                size,
                lastNonRLEBlock,
                [&materialize, &buffer, &reference, &last](const Encoding v) {
                    last = expandDelta(last, v);
                    materialize(last, reference, buffer);
                },
                [&buffer]() { buffer.appendMissing(); });

            ptr += 1 + size;
        }

        finish(elemCount, last, lastNonRLEBlock);
        return ptr;
    }

    template <typename T, typename Encoding, class Buffer, typename Materialize>
    requires Appendable<Buffer>
    static const char* decompressAllDeltaPrimitive(const char* ptr,
                                                   const char* end,
                                                   Buffer& buffer,
                                                   Encoding last,
                                                   const BSONElement& reference,
                                                   const Materialize& materialize) {
        return decompressAllDeltaPrimitive<T>(
            ptr,
            end,
            buffer,
            last,
            simple8b::kSingleZero, /* lastNonRLEBlock */
            reference,
            materialize,
            [](size_t count, Encoding last, uint64_t lastNonRLEBlock) {});
    }

    template <typename T, class Buffer, typename Materialize, typename Finish>
    requires Appendable<Buffer>
    static const char* decompressAllDeltaOfDelta(const char* ptr,
                                                 const char* end,
                                                 Buffer& buffer,
                                                 int64_t last,
                                                 int64_t lastlast,
                                                 uint64_t lastNonRLEBlock,
                                                 const BSONElement& reference,
                                                 const Materialize& materialize,
                                                 const Finish& finish) {
        // iterate until we stop seeing simple8b block sequences
        size_t elemCount = 0;
        while (ptr < end) {
            uint8_t control = *ptr;
            if (control == stdx::to_underlying(BSONType::eoo) ||
                isUncompressedLiteralControlByte(control) || isInterleavedStartControlByte(control))
                return ptr;

            uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
            uassert(8762801,
                    "Invalid control byte in BSON Column",
                    bsoncolumn::scaleIndexForControlByte(control) ==
                        Simple8bTypeUtil::kMemoryAsInteger);

            elemCount += simple8b::visitAll<int64_t>(
                ptr + 1,
                size,
                lastNonRLEBlock,
                [&materialize, &lastlast, &buffer, &reference, &last](int64_t v) {
                    lastlast = expandDelta(lastlast, v);
                    last = expandDelta(last, lastlast);
                    materialize(last, reference, buffer);
                },
                [&buffer]() { buffer.appendMissing(); });

            ptr += 1 + size;
        }

        finish(elemCount, last, lastlast, lastNonRLEBlock);
        return ptr;
    }

    template <typename T, class Buffer, typename Materialize>
    requires Appendable<Buffer>
    static const char* decompressAllDeltaOfDelta(const char* ptr,
                                                 const char* end,
                                                 Buffer& buffer,
                                                 int64_t last,
                                                 const BSONElement& reference,
                                                 const Materialize& materialize) {
        return decompressAllDeltaOfDelta<T>(
            ptr,
            end,
            buffer,
            last,
            0,
            simple8b::kSingleZero, /* lastNonRLEBlock */
            reference,
            materialize,
            [](size_t count, int64_t last, int64_t lastlast, uint64_t lastNonRLEBlock) {});
    }

    template <class Buffer, typename Finish>
    requires Appendable<Buffer>
    static const char* decompressAllDouble(const char* ptr,
                                           const char* end,
                                           Buffer& buffer,
                                           double last,
                                           uint64_t lastNonRLEBlock,
                                           const Finish& finish) {
        // iterate until we stop seeing simple8b block sequences
        int64_t lastValue = 0;
        size_t elemCount = 0;
        uint8_t scaleIndex = bsoncolumn::kInvalidScaleIndex;
        while (ptr < end) {
            uint8_t control = *ptr;
            if (control == stdx::to_underlying(BSONType::eoo) ||
                isUncompressedLiteralControlByte(control) || isInterleavedStartControlByte(control))
                return ptr;

            uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
            scaleIndex = bsoncolumn::scaleIndexForControlByte(control);
            uassert(8762802,
                    "Invalid control byte in BSON Column",
                    scaleIndex != bsoncolumn::kInvalidScaleIndex);
            auto encodedDouble = Simple8bTypeUtil::encodeDouble(last, scaleIndex);
            uassert(8295701, "Invalid double encoding in BSON Column", encodedDouble);
            lastValue = *encodedDouble;

            elemCount += simple8b::visitAll<int64_t>(
                ptr + 1,
                size,
                lastNonRLEBlock,
                [&last, &buffer, &scaleIndex, &lastValue](int64_t v) {
                    lastValue = expandDelta(lastValue, v);
                    last = Simple8bTypeUtil::decodeDouble(lastValue, scaleIndex);
                    buffer.append(last);
                },
                [&buffer]() { buffer.appendMissing(); });

            ptr += 1 + size;
        }

        finish(elemCount, lastValue, scaleIndex, lastNonRLEBlock);
        return ptr;
    }

    template <class Buffer, typename Finish>
    requires Appendable<Buffer>
    static const char* decompressAllMissing(const char* ptr,
                                            const char* end,
                                            Buffer& buffer,
                                            uint64_t lastNonRLEBlock,
                                            const Finish& finish) {
        size_t elemCount = 0;
        while (ptr < end) {
            const uint8_t control = *ptr;
            if (control == stdx::to_underlying(BSONType::eoo) ||
                isUncompressedLiteralControlByte(control) || isInterleavedStartControlByte(control))
                break;

            uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
            uassert(8915000,
                    "Invalid control byte in BSON Column",
                    bsoncolumn::scaleIndexForControlByte(control) ==
                        Simple8bTypeUtil::kMemoryAsInteger);

            elemCount = simple8b::count(ptr + 1, size);
            for (size_t i = 0; i < elemCount; ++i) {
                buffer.appendMissing();
            }

            ptr += 1 + size;
        }

        finish(elemCount, lastNonRLEBlock);
        return ptr;
    }

    template <class Buffer>
    requires Appendable<Buffer>
    static const char* decompressAllMissing(const char* ptr, const char* end, Buffer& buffer) {
        return decompressAllMissing(ptr,
                                    end,
                                    buffer,
                                    simple8b::kSingleZero /* lastNonRLEBlock */,
                                    [](size_t count, uint64_t lastNonRLEBlock) {});
    }

    template <class Buffer>
    requires Appendable<Buffer>
    static const char* decompressAllDouble(const char* ptr,
                                           const char* end,
                                           Buffer& buffer,
                                           double last) {
        return decompressAllDouble(
            ptr,
            end,
            buffer,
            last,
            simple8b::kSingleZero, /* lastNonRLEBlock */
            [](size_t count, int64_t last, uint8_t scaleIndex, uint64_t lastNonRLEBlock) {});
    }

    template <typename Encoding, class Buffer, typename Finish>
    requires Appendable<Buffer>
    static const char* decompressAllLiteral(const char* ptr,
                                            const char* end,
                                            Buffer& buffer,
                                            uint64_t lastNonRLEBlock,
                                            const Finish& finish) {
        if (buffer.isLastMissing()) {
            // The last element in the buffer is missing (EOO).
            return decompressAllMissing(ptr, end, buffer, lastNonRLEBlock, finish);
        }

        size_t elemCount = 0;
        while (ptr < end) {
            const uint8_t control = *ptr;
            if (control == stdx::to_underlying(BSONType::eoo) ||
                isUncompressedLiteralControlByte(control) || isInterleavedStartControlByte(control))
                break;

            uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
            uassert(8762803,
                    "Invalid control byte in BSON Column",
                    bsoncolumn::scaleIndexForControlByte(control) ==
                        Simple8bTypeUtil::kMemoryAsInteger);

            elemCount += simple8b::visitAll<Encoding>(
                ptr + 1,
                size,
                lastNonRLEBlock,
                [&buffer](const Encoding v) {
                    uassert(
                        8609800, "Post literal delta blocks should only contain skip or 0", v == 0);
                    buffer.appendLast();
                },
                [&buffer]() { buffer.appendLast(); },
                [&buffer]() { buffer.appendMissing(); });

            ptr += 1 + size;
        }

        finish(elemCount, lastNonRLEBlock);
        return ptr;
    }

    template <typename Encoding, class Buffer>
    requires Appendable<Buffer>
    static const char* decompressAllLiteral(const char* ptr, const char* end, Buffer& buffer) {
        return decompressAllLiteral<Encoding>(ptr,
                                              end,
                                              buffer,
                                              simple8b::kSingleZero /* lastNonRLEBlock */,
                                              [](size_t count, uint64_t lastNonRLEBlock) {});
    }

    template <typename Encoding>
    static const char* lastDelta(const char* ptr, const char* end, Encoding& out) {
        uint64_t lastNonRLEBlock = simple8b::kSingleZero;

        while (ptr < end) {
            uint8_t control = *ptr;
            if (control == stdx::to_underlying(BSONType::eoo) ||
                isUncompressedLiteralControlByte(control) || isInterleavedStartControlByte(control))
                return ptr;

            uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
            uassert(9095623,
                    "Invalid control byte in BSON Column",
                    bsoncolumn::scaleIndexForControlByte(control) ==
                        Simple8bTypeUtil::kMemoryAsInteger);

            out = simple8b::add(out, simple8b::sum<Encoding>(ptr + 1, size, lastNonRLEBlock));

            ptr += 1 + size;
        }

        return ptr;
    }

    template <typename Encoding>
    static const char* lastDeltaOfDelta(const char* ptr, const char* end, Encoding& out) {
        Encoding prefix = 0;
        uint64_t lastNonRLEBlock = simple8b::kSingleZero;

        while (ptr < end) {
            uint8_t control = *ptr;
            if (control == stdx::to_underlying(BSONType::eoo) ||
                isUncompressedLiteralControlByte(control) || isInterleavedStartControlByte(control))
                return ptr;

            uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
            uassert(9095624,
                    "Invalid control byte in BSON Column",
                    bsoncolumn::scaleIndexForControlByte(control) ==
                        Simple8bTypeUtil::kMemoryAsInteger);

            out = simple8b::add(
                out, simple8b::prefixSum<Encoding>(ptr + 1, size, prefix, lastNonRLEBlock));

            ptr += 1 + size;
        }

        return ptr;
    }

    static const char* lastDouble(const char* ptr, const char* end, double& last) {
        uint64_t lastNonRLEBlock = simple8b::kSingleZero;
        int64_t lastValue = 0;
        uint8_t scaleIndex = bsoncolumn::kInvalidScaleIndex;

        while (ptr < end) {
            uint8_t control = *ptr;
            if (control == stdx::to_underlying(BSONType::eoo) ||
                isUncompressedLiteralControlByte(control) || isInterleavedStartControlByte(control))
                return ptr;

            uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
            scaleIndex = bsoncolumn::scaleIndexForControlByte(control);
            uassert(9095625,
                    "Invalid control byte in BSON Column",
                    scaleIndex != bsoncolumn::kInvalidScaleIndex);
            auto encodedDouble = Simple8bTypeUtil::encodeDouble(last, scaleIndex);
            uassert(9095626, "Invalid double encoding in BSON Column", encodedDouble);
            lastValue = *encodedDouble;
            lastValue =
                simple8b::add(lastValue, simple8b::sum<int64_t>(ptr + 1, size, lastNonRLEBlock));

            last = Simple8bTypeUtil::decodeDouble(lastValue, scaleIndex);

            ptr += 1 + size;
        }

        return ptr;
    }

    static const char* lastString(const char* ptr,
                                  const char* end,
                                  boost::optional<int128_t> last,
                                  int128_t& out,
                                  int128_t& uncompressed) {
        uint64_t lastNonRLEBlock = simple8b::kSingleZero;
        uncompressed = last.value_or(0);
        out = uncompressed;

        if (last) {
            while (ptr < end) {
                uint8_t control = *ptr;
                if (control == stdx::to_underlying(BSONType::eoo) ||
                    isUncompressedLiteralControlByte(control) ||
                    isInterleavedStartControlByte(control))
                    return ptr;

                uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
                uassert(9095627,
                        "Invalid control byte in BSON Column",
                        bsoncolumn::scaleIndexForControlByte(control) ==
                            Simple8bTypeUtil::kMemoryAsInteger);

                out = simple8b::add(out, simple8b::sum<int128_t>(ptr + 1, size, lastNonRLEBlock));

                ptr += 1 + size;
            }
        } else {
            int128_t lastNonZero{0};
            while (ptr < end) {
                uint8_t control = *ptr;
                if (control == stdx::to_underlying(BSONType::eoo) ||
                    isUncompressedLiteralControlByte(control) ||
                    isInterleavedStartControlByte(control)) {

                    if (lastNonZero != 0) {
                        uncompressed = lastNonZero;
                    }
                    return ptr;
                }


                uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
                uassert(9095628,
                        "Invalid control byte in BSON Column",
                        bsoncolumn::scaleIndexForControlByte(control) ==
                            Simple8bTypeUtil::kMemoryAsInteger);

                simple8b::visitAll<int128_t>(
                    ptr + 1,
                    size,
                    lastNonRLEBlock,
                    [&](int128_t delta) {
                        if (delta != 0) {
                            lastNonZero = delta;
                        }
                        out = expandDelta(out, delta);
                    },
                    []() {});

                ptr += 1 + size;
            }
        }

        return ptr;
    }

    template <class Encoding>
    static const char* validateLiteral(const char* ptr, const char* end) {
        uint64_t lastNonRLEBlock = simple8b::kSingleZero;
        while (ptr < end) {
            const uint8_t control = *ptr;
            if (control == stdx::to_underlying(BSONType::eoo) ||
                isUncompressedLiteralControlByte(control) || isInterleavedStartControlByte(control))
                break;

            uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
            uassert(9095629,
                    "Invalid control byte in BSON Column",
                    bsoncolumn::scaleIndexForControlByte(control) ==
                        Simple8bTypeUtil::kMemoryAsInteger);

            simple8b::visitAll<int64_t>(
                ptr + 1,
                size,
                lastNonRLEBlock,
                [](int64_t v) {
                    uassert(
                        9095630, "Post literal delta blocks should only contain skip or 0", v == 0);
                },
                []() {},
                []() {});

            ptr += 1 + size;
        }

        return ptr;
    }

    static bool containsScalars(const BSONObj& obj) {
        bool result = false;
        BSONObjTraversal{true,
                         BSONType::object,
                         [](auto&&...) { return true; },
                         [&](auto&&...) {
                             result = true;
                             return false;
                         }}
            .traverse(obj);

        return result;
    }
};

/**
 * Implements the "materializer" concept such that the output elements are BSONElements.
 */
class BSONElementMaterializer {
public:
    using Element = BSONElement;

    static BSONElement materialize(BSONElementStorage& allocator, bool val);
    static BSONElement materialize(BSONElementStorage& allocator, int32_t val);
    static BSONElement materialize(BSONElementStorage& allocator, int64_t val);
    static BSONElement materialize(BSONElementStorage& allocator, double val);
    static BSONElement materialize(BSONElementStorage& allocator, const Decimal128& val);
    static BSONElement materialize(BSONElementStorage& allocator, const Date_t& val);
    static BSONElement materialize(BSONElementStorage& allocator, const Timestamp& val);
    static BSONElement materialize(BSONElementStorage& allocator, StringData val);
    static BSONElement materialize(BSONElementStorage& allocator, const BSONBinData& val);
    static BSONElement materialize(BSONElementStorage& allocator, const BSONCode& val);
    static BSONElement materialize(BSONElementStorage& allocator, const OID& val);

    template <typename T>
    static BSONElement materialize(BSONElementStorage& allocator, BSONElement val) {
        if (val.eoo())
            return BSONElement();
        auto allocatedElem = allocator.allocate(val.type(), "", val.valuesize());
        memcpy(allocatedElem.value(), val.value(), val.valuesize());
        return allocatedElem.element();
    }

    template <typename T>
    static T get(const Element& elem) {
        if constexpr (std::is_same_v<T, double>) {
            return BSONElementValue(elem.value()).Double();
        } else if constexpr (std::is_same_v<T, StringData>) {
            return BSONElementValue(elem.value()).String();
        } else if constexpr (std::is_same_v<T, BSONObj>) {
            return BSONElementValue(elem.value()).Obj();
        } else if constexpr (std::is_same_v<T, BSONArray>) {
            return BSONElementValue(elem.value()).Array();
        } else if constexpr (std::is_same_v<T, BSONBinData>) {
            return BSONElementValue(elem.value()).BinData();
        } else if constexpr (std::is_same_v<T, OID>) {
            return BSONElementValue(elem.value()).ObjectID();
        } else if constexpr (std::is_same_v<T, bool>) {
            return BSONElementValue(elem.value()).Boolean();
        } else if constexpr (std::is_same_v<T, Date_t>) {
            return BSONElementValue(elem.value()).Date();
        } else if constexpr (std::is_same_v<T, BSONRegEx>) {
            return BSONElementValue(elem.value()).Regex();
        } else if constexpr (std::is_same_v<T, BSONDBRef>) {
            return BSONElementValue(elem.value()).DBRef();
        } else if constexpr (std::is_same_v<T, BSONCode>) {
            return BSONElementValue(elem.value()).Code();
        } else if constexpr (std::is_same_v<T, BSONSymbol>) {
            return BSONElementValue(elem.value()).Symbol();
        } else if constexpr (std::is_same_v<T, BSONCodeWScope>) {
            return BSONElementValue(elem.value()).CodeWScope();
        } else if constexpr (std::is_same_v<T, int32_t>) {
            return BSONElementValue(elem.value()).Int32();
        } else if constexpr (std::is_same_v<T, Timestamp>) {
            return BSONElementValue(elem.value()).timestamp();
        } else if constexpr (std::is_same_v<T, int64_t>) {
            return BSONElementValue(elem.value()).Int64();
        } else if constexpr (std::is_same_v<T, Decimal128>) {
            return BSONElementValue(elem.value()).Decimal();
        }
        invariant(false);
        return T{};
    }

    static BSONElement materializePreallocated(BSONElement val) {
        return val;
    }

    static BSONElement materializeMissing(BSONElementStorage& allocator) {
        return BSONElement();
    }

    static bool isMissing(const Element& elem) {
        return elem.eoo();
    }

    static int canonicalType(const Element& elem) {
        return elem.canonicalType();
    }

    static int compare(const Element& lhs,
                       const Element& rhs,
                       const StringDataComparator* comparator) {
        return BSONElement::compareElements(
            lhs, rhs, BSONElement::ComparisonRules::kConsiderFieldName, comparator);
    }

private:
    /**
     * Helper function used by both BSONCode and String.
     */
    static BSONElement writeStringData(BSONElementStorage& allocator,
                                       BSONType bsonType,
                                       StringData val);
};

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<bool>(
    BSONElementStorage& allocator, BSONElement val) {
    dassert(val.type() == BSONType::boolean, "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, val.boolean());
}

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<int32_t>(
    BSONElementStorage& allocator, BSONElement val) {
    dassert(val.type() == BSONType::numberInt,
            "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, (int32_t)val._numberInt());
}

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<int64_t>(
    BSONElementStorage& allocator, BSONElement val) {
    dassert(val.type() == BSONType::numberLong,
            "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, (int64_t)val._numberLong());
}

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<double>(
    BSONElementStorage& allocator, BSONElement val) {
    dassert(val.type() == BSONType::numberDouble,
            "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, val._numberDouble());
}

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<Decimal128>(
    BSONElementStorage& allocator, BSONElement val) {
    dassert(val.type() == BSONType::numberDecimal,
            "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, val._numberDecimal());
}

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<Date_t>(
    BSONElementStorage& allocator, BSONElement val) {
    dassert(val.type() == BSONType::date, "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, val.date());
}

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<Timestamp>(
    BSONElementStorage& allocator, BSONElement val) {
    dassert(val.type() == BSONType::timestamp,
            "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, val.timestamp());
}

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<StringData>(
    BSONElementStorage& allocator, BSONElement val) {
    dassert(val.type() == BSONType::string, "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, val.valueStringData());
}

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<BSONBinData>(
    BSONElementStorage& allocator, BSONElement val) {
    dassert(val.type() == BSONType::binData, "materialize invoked with incorrect BSONElement type");
    int len = 0;
    const char* data = val.binData(len);
    return materialize(allocator, BSONBinData(data, len, val.binDataType()));
}

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<BSONCode>(
    BSONElementStorage& allocator, BSONElement val) {
    dassert(val.type() == BSONType::code, "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, BSONCode(val.valueStringData()));
}

template <>
inline BSONElementMaterializer::Element BSONElementMaterializer::materialize<OID>(
    BSONElementStorage& allocator, BSONElement val) {
    dassert(val.type() == BSONType::oid, "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, val.OID());
}

struct RootPath {
    boost::container::small_vector<const char*, 1> elementsToMaterialize(BSONObj refObj) {
        return {refObj.objdata()};
    }
};

/**
 * Returns true if the given path is the root path. If it returns anything given the empty object,
 * then it's the root path.
 */
template <class Path>
bool isRootPath(Path& path) {
    return !path.elementsToMaterialize(BSONObj{}).empty();
}

}  // namespace bsoncolumn
}  // namespace mongo
