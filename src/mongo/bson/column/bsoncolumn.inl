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

template <class Buffer>
requires Appendable<Buffer>
MONGO_COMPILER_ALWAYS_INLINE_GCC14 void BSONColumnBlockBased::decompress(Buffer& buffer) const {
    const char* ptr = _binary;
    const char* end = _binary + _size;
    BSONType type = BSONType::eoo;  // needs to be set as something else before deltas are parsed

    // If first block(s) are simple8B, these should all be skips.
    ptr = BSONColumnBlockDecompressHelpers::decompressAllMissing(ptr, end, buffer);

    while (ptr < end) {
        const uint8_t control = *ptr;
        if (control == stdx::to_underlying(BSONType::eoo)) {
            uassert(
                8295703, "BSONColumn data ended without reaching end of buffer", ptr + 1 == end);
            buffer.eof();
            return;
        } else if (isUncompressedLiteralControlByte(control)) {
            BSONElement literal(ptr, 1, BSONElement::TrustedInitTag{});
            type = literal.type();
            ptr += literal.size();
            switch (type) {
                case BSONType::boolean:
                    buffer.template append<bool>(literal);
                    ptr = BSONColumnBlockDecompressHelpers::
                        decompressAllDeltaPrimitive<bool, int64_t, Buffer>(
                            ptr,
                            end,
                            buffer,
                            literal.boolean(),
                            literal,
                            [](const int64_t v, const BSONElement& ref, Buffer& buffer) {
                                buffer.append(static_cast<bool>(v));
                            });
                    break;
                case BSONType::numberInt:
                    buffer.template append<int32_t>(literal);
                    ptr = BSONColumnBlockDecompressHelpers::
                        decompressAllDeltaPrimitive<int32_t, int64_t, Buffer>(
                            ptr,
                            end,
                            buffer,
                            literal._numberInt(),
                            literal,
                            [](const int64_t v, const BSONElement& ref, Buffer& buffer) {
                                buffer.append(static_cast<int32_t>(v));
                            });
                    break;
                case BSONType::numberLong:
                    buffer.template append<int64_t>(literal);
                    ptr = BSONColumnBlockDecompressHelpers::
                        decompressAllDeltaPrimitive<int64_t, int64_t, Buffer>(
                            ptr,
                            end,
                            buffer,
                            literal._numberLong(),
                            literal,
                            [](const int64_t v, const BSONElement& ref, Buffer& buffer) {
                                buffer.append(v);
                            });
                    break;
                case BSONType::numberDecimal:
                    buffer.template append<Decimal128>(literal);
                    ptr = BSONColumnBlockDecompressHelpers::
                        decompressAllDelta<Decimal128, int128_t, Buffer>(
                            ptr,
                            end,
                            buffer,
                            Simple8bTypeUtil::encodeDecimal128(literal._numberDecimal()),
                            literal,
                            [](const int128_t v, const BSONElement& ref, Buffer& buffer) {
                                buffer.append(Simple8bTypeUtil::decodeDecimal128(v));
                            });
                    break;
                case BSONType::numberDouble:
                    buffer.template append<double>(literal);
                    ptr = BSONColumnBlockDecompressHelpers::decompressAllDouble(
                        ptr, end, buffer, literal._numberDouble());
                    break;
                case BSONType::timestamp:
                    buffer.template append<Timestamp>(literal);
                    ptr = BSONColumnBlockDecompressHelpers::decompressAllDeltaOfDelta<Timestamp,
                                                                                      Buffer>(
                        ptr,
                        end,
                        buffer,
                        literal.timestampValue(),
                        literal,
                        [](const int64_t v, const BSONElement& ref, Buffer& buffer) {
                            buffer.append(static_cast<Timestamp>(v));
                        });
                    break;
                case BSONType::date:
                    buffer.template append<Date_t>(literal);
                    ptr =
                        BSONColumnBlockDecompressHelpers::decompressAllDeltaOfDelta<Date_t, Buffer>(
                            ptr,
                            end,
                            buffer,
                            literal.date().toMillisSinceEpoch(),
                            literal,
                            [](const int64_t v, const BSONElement& ref, Buffer& buffer) {
                                buffer.append(Date_t::fromMillisSinceEpoch(v));
                            });
                    break;
                case BSONType::oid:
                    buffer.template append<OID>(literal);
                    ptr = BSONColumnBlockDecompressHelpers::decompressAllDeltaOfDelta<OID, Buffer>(
                        ptr,
                        end,
                        buffer,
                        Simple8bTypeUtil::encodeObjectId(literal.__oid()),
                        literal,
                        [](const int64_t v, const BSONElement& ref, Buffer& buffer) {
                            buffer.append(Simple8bTypeUtil::decodeObjectId(
                                v, ref.__oid().getInstanceUnique()));
                        });
                    break;
                case BSONType::string:
                    buffer.template append<StringData>(literal);
                    ptr = BSONColumnBlockDecompressHelpers::decompressAllDelta<StringData,
                                                                               int128_t,
                                                                               Buffer>(
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
                case BSONType::binData: {
                    buffer.template append<BSONBinData>(literal);
                    int size;
                    const char* binary = literal.binData(size);
                    if (size <= 16) {
                        ptr = BSONColumnBlockDecompressHelpers::
                            decompressAllDelta<BSONBinData, int128_t, Buffer>(
                                ptr,
                                end,
                                buffer,
                                Simple8bTypeUtil::encodeBinary(binary, size).value_or(0),
                                literal,
                                [&size](const int128_t v, const BSONElement& ref, Buffer& buffer) {
                                    char data[16];
                                    Simple8bTypeUtil::decodeBinary(v, data, size);
                                    buffer.append(BSONBinData(data, size, ref.binDataType()));
                                });
                    } else {
                        ptr = BSONColumnBlockDecompressHelpers::decompressAllLiteral<int128_t>(
                            ptr, end, buffer);
                    }
                    break;
                }
                case BSONType::code:
                    buffer.template append<BSONCode>(literal);
                    ptr = BSONColumnBlockDecompressHelpers::decompressAllDelta<BSONCode,
                                                                               int128_t,
                                                                               Buffer>(
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
                case BSONType::object:
                case BSONType::array:
                case BSONType::undefined:
                case BSONType::null:
                case BSONType::regEx:
                case BSONType::dbRef:
                case BSONType::codeWScope:
                case BSONType::symbol:
                case BSONType::minKey:
                case BSONType::maxKey:
                    // Non-delta types, deltas should only contain skip or 0
                    buffer.template append<BSONElement>(literal);
                    ptr = BSONColumnBlockDecompressHelpers::decompressAllLiteral<int64_t>(
                        ptr, end, buffer);
                    break;
                default:
                    uasserted(8295704, "Type not implemented");
                    break;
            }
        } else if (isInterleavedStartControlByte(control)) {
            BlockBasedInterleavedDecompressor decompressor{buffer.getAllocator(), ptr, end};
            using PathBufferPair = std::pair<RootPath, Buffer&>;
            std::array<PathBufferPair, 1> path{{{RootPath{}, buffer}}};
            ptr = decompressor.decompress(std::span<PathBufferPair, 1>{path});
            ptr = BSONColumnBlockDecompressHelpers::decompressAllLiteral<int64_t>(ptr, end, buffer);
        } else {
            uasserted(8295706, "Unexpected control");
        }
    }
}

}  // namespace bsoncolumn
}  // namespace mongo
