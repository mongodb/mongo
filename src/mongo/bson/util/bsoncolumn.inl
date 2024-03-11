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

namespace mongo {
namespace bsoncolumn {

// TODO:  Materialize is used in some places to refer converting int encodings to
// concrete types, and in other places to refer to converting concrete types to
// a desired output type.  Here we use it to refer to a composite of these two
// actions; we should take the time to make our terminology consistent.
template <typename T, typename Encoding, class Buffer, typename Materialize>
requires Appendable<Buffer>
const char* BSONColumnBlockBased::decompressAllDelta(const char* ptr,
                                                     const char* end,
                                                     Buffer& buffer,
                                                     Encoding last,
                                                     const BSONElement& reference,
                                                     const Materialize& materialize) {
    // iterate until we stop seeing simple8b block sequences
    while (ptr < end) {
        uint8_t control = *ptr;
        if (control == EOO || isUncompressedLiteralControlByte(control) ||
            isInterleavedStartControlByte(control))
            return ptr;

        uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
        Simple8b<make_unsigned_t<Encoding>> s8b(ptr + 1, size);
        auto it = s8b.begin();
        // after reading a literal, the the buffer's last value is incorrect so we
        // process all copies of the reference object until we materialize something
        // otherwise the buffer will reference the wrong value on calls to appendLast()
        for (; it != s8b.end(); ++it) {
            const auto& delta = *it;
            if (delta) {
                if (*delta == 0)
                    buffer.template append<T>(reference);
                else
                    break;
            } else {
                buffer.appendMissing();
            }
        }
        for (; it != s8b.end(); ++it) {
            const auto& delta = *it;
            if (delta) {
                if (*delta == 0) {
                    buffer.appendLast();
                } else {
                    last = expandDelta(
                        last, Simple8bTypeUtil::decodeInt<make_unsigned_t<Encoding>>(*delta));
                    materialize(last, reference, buffer);
                }
            } else {
                buffer.appendMissing();
            }
        }
        ptr += 1 + size;
    }

    return ptr;
}

template <typename T, typename Encoding, class Buffer, typename Materialize>
requires Appendable<Buffer>
const char* BSONColumnBlockBased::decompressAllDeltaPrimitive(const char* ptr,
                                                              const char* end,
                                                              Buffer& buffer,
                                                              Encoding last,
                                                              const BSONElement& reference,
                                                              const Materialize& materialize) {
    // iterate until we stop seeing simple8b block sequences
    while (ptr < end) {
        uint8_t control = *ptr;
        if (control == EOO || isUncompressedLiteralControlByte(control) ||
            isInterleavedStartControlByte(control))
            return ptr;

        uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
        uassert(8762800,
                "Invalid control byte in BSON Column",
                bsoncolumn::scaleIndexForControlByte(control) != bsoncolumn::kInvalidScaleIndex);
        Simple8b<make_unsigned_t<Encoding>> s8b(ptr + 1, size);
        auto it = s8b.begin();
        for (; it != s8b.end(); ++it) {
            const auto& delta = *it;
            if (delta) {
                last = expandDelta(last,
                                   Simple8bTypeUtil::decodeInt<make_unsigned_t<Encoding>>(*delta));
                materialize(last, reference, buffer);
            } else {
                buffer.appendMissing();
            }
        }
        ptr += 1 + size;
    }

    return ptr;
}

template <typename T, class Buffer, typename Materialize, typename Decode>
requires Appendable<Buffer>
const char* BSONColumnBlockBased::decompressAllDeltaOfDelta(const char* ptr,
                                                            const char* end,
                                                            Buffer& buffer,
                                                            int64_t last,
                                                            const BSONElement& reference,
                                                            const Materialize& materialize,
                                                            const Decode& decode) {
    // iterate until we stop seeing simple8b block sequences
    int64_t lastlast = 0;
    while (ptr < end) {
        uint8_t control = *ptr;
        if (control == EOO || isUncompressedLiteralControlByte(control) ||
            isInterleavedStartControlByte(control))
            return ptr;

        uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
        uassert(8762801,
                "Invalid control byte in BSON Column",
                bsoncolumn::scaleIndexForControlByte(control) != bsoncolumn::kInvalidScaleIndex);
        Simple8b<uint64_t> s8b(ptr + 1, size);
        for (auto it = s8b.begin(); it != s8b.end(); ++it) {
            const auto& delta = *it;
            if (delta) {
                lastlast = expandDelta(lastlast, decode(*delta));
                last = expandDelta(last, lastlast);
                materialize(last, reference, buffer);
            } else {
                buffer.appendMissing();
            }
        }
        ptr += 1 + size;
    }

    return ptr;
}

template <class Buffer>
requires Appendable<Buffer>
const char* BSONColumnBlockBased::decompressAllDouble(const char* ptr,
                                                      const char* end,
                                                      Buffer& buffer,
                                                      double last) {
    // iterate until we stop seeing simple8b block sequences
    int64_t lastValue = 0;
    while (ptr < end) {
        uint8_t control = *ptr;
        if (control == EOO || isUncompressedLiteralControlByte(control) ||
            isInterleavedStartControlByte(control))
            return ptr;

        uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
        uint8_t scaleIndex = bsoncolumn::scaleIndexForControlByte(control);
        uassert(8762802,
                "Invalid control byte in BSON Column",
                scaleIndex != bsoncolumn::kInvalidScaleIndex);
        auto encodedDouble = Simple8bTypeUtil::encodeDouble(last, scaleIndex);
        uassert(8295701, "Invalid double encoding in BSON Column", encodedDouble);
        lastValue = *encodedDouble;
        Simple8b<uint64_t> s8b(ptr + 1, size);
        for (auto it = s8b.begin(); it != s8b.end(); ++it) {
            const auto& delta = *it;
            if (delta) {
                lastValue = expandDelta(lastValue, Simple8bTypeUtil::decodeInt64(*delta));
                last = Simple8bTypeUtil::decodeDouble(lastValue, scaleIndex);
                buffer.append(last);
            } else {
                buffer.appendMissing();
            }
        }
        ptr += 1 + size;
    }

    return ptr;
}

template <class Buffer>
requires Appendable<Buffer>
const char* BSONColumnBlockBased::decompressAllLiteral(const char* ptr,
                                                       const char* end,
                                                       Buffer& buffer,
                                                       const BSONElement& reference) {
    while (ptr < end) {
        const uint8_t control = *ptr;
        if (control == EOO || isUncompressedLiteralControlByte(control) ||
            isInterleavedStartControlByte(control))
            break;

        uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
        uassert(8762803,
                "Invalid control byte in BSON Column",
                bsoncolumn::scaleIndexForControlByte(control) != bsoncolumn::kInvalidScaleIndex);
        Simple8b<uint64_t> s8b(ptr + 1, size);
        for (auto it = s8b.begin(); it != s8b.end(); ++it) {
            const auto& delta = *it;
            if (!delta)
                buffer.appendMissing();
            else if (*delta == 0)
                buffer.template append<BSONElement>(reference);
            else
                uasserted(8609800, "Post literal delta blocks should only contain skip or 0");
        }
        ptr += 1 + size;
    }

    return ptr;
}

template <class Buffer>
requires Appendable<Buffer>
void BSONColumnBlockBased::decompress(Buffer& buffer) const {
    const char* ptr = _binary;
    const char* end = _binary + _size;
    BSONType type = EOO;  // needs to be set as something else before deltas are parsed

    while (ptr < end) {
        const uint8_t control = *ptr;
        if (control == EOO || isUncompressedLiteralControlByte(control) ||
            isInterleavedStartControlByte(control))
            break;

        // If first block(s) are simple8B, these should all be skips. Before decompressing we must
        // validate the scale factor.
        uint8_t size = numSimple8bBlocksForControlByte(control) * sizeof(uint64_t);
        uassert(8762804,
                "Invalid control byte in BSON Column",
                bsoncolumn::scaleIndexForControlByte(control) != bsoncolumn::kInvalidScaleIndex);
        Simple8b<uint64_t> s8b(ptr + 1, size);
        for (auto it = s8b.begin(); it != s8b.end(); ++it) {
            buffer.appendMissing();
        }
        ptr += 1 + size;
    }

    while (ptr < end) {
        const uint8_t control = *ptr;
        if (control == EOO) {
            uassert(
                8295703, "BSONColumn data ended without reaching end of buffer", ptr + 1 == end);
            return;
        } else if (isUncompressedLiteralControlByte(control)) {
            BSONElement literal(ptr, 1, -1);
            type = literal.type();
            ptr += literal.size();
            switch (type) {
                case Bool:
                    buffer.template append<bool>(literal);
                    ptr = decompressAllDeltaPrimitive<bool, int64_t, Buffer>(
                        ptr,
                        end,
                        buffer,
                        literal.boolean(),
                        literal,
                        [](const int64_t v, const BSONElement& ref, Buffer& buffer) {
                            buffer.append(static_cast<bool>(v));
                        });
                    break;
                case NumberInt:
                    buffer.template append<int32_t>(literal);
                    ptr = decompressAllDeltaPrimitive<int32_t, int64_t, Buffer>(
                        ptr,
                        end,
                        buffer,
                        literal._numberInt(),
                        literal,
                        [](const int64_t v, const BSONElement& ref, Buffer& buffer) {
                            buffer.append(static_cast<int32_t>(v));
                        });
                    break;
                case NumberLong:
                    buffer.template append<int64_t>(literal);
                    ptr = decompressAllDeltaPrimitive<int64_t, int64_t, Buffer>(
                        ptr,
                        end,
                        buffer,
                        literal._numberLong(),
                        literal,
                        [](const int64_t v, const BSONElement& ref, Buffer& buffer) {
                            buffer.append(v);
                        });
                    break;
                case NumberDecimal:
                    buffer.template append<Decimal128>(literal);
                    ptr = decompressAllDelta<Decimal128, int128_t, Buffer>(
                        ptr,
                        end,
                        buffer,
                        Simple8bTypeUtil::encodeDecimal128(literal._numberDecimal()),
                        literal,
                        [](const int128_t v, const BSONElement& ref, Buffer& buffer) {
                            buffer.append(Simple8bTypeUtil::decodeDecimal128(v));
                        });
                    break;
                case NumberDouble:
                    buffer.template append<double>(literal);
                    ptr = decompressAllDouble(ptr, end, buffer, literal._numberDouble());
                    break;
                case bsonTimestamp:
                    buffer.template append<Timestamp>(literal);
                    ptr = decompressAllDeltaOfDelta<Timestamp, Buffer>(
                        ptr,
                        end,
                        buffer,
                        literal.timestampValue(),
                        literal,
                        [](const int64_t v, const BSONElement& ref, Buffer& buffer) {
                            buffer.append(static_cast<Timestamp>(v));
                        },
                        Simple8bTypeUtil::decodeInt64);
                    break;
                case Date:
                    buffer.template append<Date_t>(literal);
                    ptr = decompressAllDeltaOfDelta<Date_t, Buffer>(
                        ptr,
                        end,
                        buffer,
                        literal.date().toMillisSinceEpoch(),
                        literal,
                        [](const int64_t v, const BSONElement& ref, Buffer& buffer) {
                            buffer.append(Date_t::fromMillisSinceEpoch(v));
                        },
                        Simple8bTypeUtil::decodeInt64);
                    break;
                case jstOID:
                    buffer.template append<OID>(literal);
                    ptr = decompressAllDeltaOfDelta<OID, Buffer>(
                        ptr,
                        end,
                        buffer,
                        Simple8bTypeUtil::encodeObjectId(literal.__oid()),
                        literal,
                        [](const int64_t v, const BSONElement& ref, Buffer& buffer) {
                            buffer.append(Simple8bTypeUtil::decodeObjectId(
                                v, ref.__oid().getInstanceUnique()));
                        },
                        Simple8bTypeUtil::decodeInt64);
                    break;
                case String:
                    buffer.template append<StringData>(literal);
                    ptr = decompressAllDelta<StringData, int128_t, Buffer>(
                        ptr,
                        end,
                        buffer,
                        Simple8bTypeUtil::encodeString(literal.valueStringData()).value_or(0),
                        literal,
                        [](const int128_t v, const BSONElement& ref, Buffer& buffer) {
                            auto string = Simple8bTypeUtil::decodeString(v);
                            buffer.append(StringData((const char*)string.str.data(), string.size));
                        });
                    break;
                case BinData: {
                    buffer.template append<BSONBinData>(literal);
                    int size;
                    const char* binary = literal.binData(size);
                    ptr = decompressAllDelta<BSONBinData, int128_t, Buffer>(
                        ptr,
                        end,
                        buffer,
                        Simple8bTypeUtil::encodeBinary(binary, size).value_or(0),
                        literal,
                        [](const int128_t v, const BSONElement& ref, Buffer& buffer) {
                            char data[16];
                            size_t size = ref.valuestrsize();
                            Simple8bTypeUtil::decodeBinary(v, data, size);
                            buffer.append(BSONBinData(data, size, ref.binDataType()));
                        });
                    break;
                }
                case Code:
                    buffer.template append<BSONCode>(literal);
                    ptr = decompressAllDelta<BSONCode, int128_t, Buffer>(
                        ptr,
                        end,
                        buffer,
                        Simple8bTypeUtil::encodeString(literal.valueStringData()).value_or(0),
                        literal,
                        [](const int128_t v, const BSONElement& ref, Buffer& buffer) {
                            auto string = Simple8bTypeUtil::decodeString(v);
                            buffer.append(
                                BSONCode(StringData((const char*)string.str.data(), string.size)));
                        });
                    break;
                case Object:
                case Array:
                case Undefined:
                case jstNULL:
                case RegEx:
                case DBRef:
                case CodeWScope:
                case Symbol:
                case MinKey:
                case MaxKey:
                    // Non-delta types, deltas should only contain skip or 0
                    buffer.template append<BSONElement>(literal);
                    ptr = decompressAllLiteral(ptr, end, buffer, literal);
                    break;
                default:
                    uasserted(8295704, "Type not implemented");
                    break;
            }
        } else if (isInterleavedStartControlByte(control)) {
            uasserted(8295705, "Interleaved decoding not implemented");
        } else {
            uasserted(8295706, "Unexpected control");
        }
    }
}

}  // namespace bsoncolumn
}  // namespace mongo
