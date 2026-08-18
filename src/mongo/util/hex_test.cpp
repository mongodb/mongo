// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/hex.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

TEST(HexblobTest, DecodePair) {
    ASSERT_EQ(hexblob::decodePair("00"sv), 0x00);
    ASSERT_EQ(hexblob::decodePair("0f"sv), 0x0f);
    ASSERT_EQ(hexblob::decodePair("F0"sv), 0xf0);
    ASSERT_EQ(hexblob::decodePair("aB"sv), 0xab);
    ASSERT_EQ(hexblob::decodePair("ff"sv), 0xff);
}

TEST(HexblobTest, DecodePairInvalid) {
    // Wrong number of digits.
    ASSERT_THROWS_CODE(hexblob::decodePair(""sv), DBException, ErrorCodes::FailedToParse);
    ASSERT_THROWS_CODE(hexblob::decodePair("0"sv), DBException, ErrorCodes::FailedToParse);
    ASSERT_THROWS_CODE(hexblob::decodePair("000"sv), DBException, ErrorCodes::FailedToParse);
    // Right count, bad digits.
    ASSERT_THROWS_CODE(hexblob::decodePair("0g"sv), DBException, ErrorCodes::FailedToParse);
    ASSERT_THROWS_CODE(hexblob::decodePair("x0"sv), DBException, ErrorCodes::FailedToParse);
}

TEST(HexblobTest, Validate) {
    ASSERT_TRUE(hexblob::validate(""sv));
    ASSERT_TRUE(hexblob::validate("00"sv));
    ASSERT_TRUE(hexblob::validate("0123456789abcdefABCDEF"sv));

    ASSERT_FALSE(hexblob::validate("0"sv)) << "odd length";
    ASSERT_FALSE(hexblob::validate("012"sv)) << "odd length";
    ASSERT_FALSE(hexblob::validate("0g"sv)) << "non-hex digit";
    ASSERT_FALSE(hexblob::validate("00 11"sv)) << "separators are not allowed";
    ASSERT_FALSE(
        hexblob::validate("00\0"
                          "00"sv))
        << "embedded NUL, odd length";
    ASSERT_FALSE(hexblob::validate("ff\0ff"sv)) << "embedded NUL, odd length";
    ASSERT_FALSE(hexblob::validate("0\0"sv)) << "embedded NUL";
}

TEST(HexblobTest, Encode) {
    ASSERT_EQ(hexblob::encode(""sv), "");
    ASSERT_EQ(hexblob::encode("\x00"sv), "00");
    ASSERT_EQ(hexblob::encode("abc"sv), "616263");
    ASSERT_EQ(hexblob::encode("\xde\xad\xbe\xef"sv), "DEADBEEF");
    ASSERT_EQ(hexblob::encode("\x00\x01\x7f\x80\xff"sv), "00017F80FF");
}

TEST(HexblobTest, EncodeLower) {
    ASSERT_EQ(hexblob::encodeLower(""sv), "");
    ASSERT_EQ(hexblob::encodeLower("abc"sv), "616263");
    ASSERT_EQ(hexblob::encodeLower("\xde\xad\xbe\xef"sv), "deadbeef");
    ASSERT_EQ(hexblob::encodeLower("\x00\x01\x7f\x80\xff"sv), "00017f80ff");
}

TEST(HexblobTest, EncodeAllByteValues) {
    std::string all(256, '\0');
    std::iota(all.begin(), all.end(), '\0');

    const std::string upper = hexblob::encode(all);
    const std::string lower = hexblob::encodeLower(all);
    ASSERT_EQ(upper.size(), 2 * all.size());
    ASSERT_EQ(lower.size(), 2 * all.size());
    ASSERT_TRUE(hexblob::validate(upper));
    ASSERT_TRUE(hexblob::validate(lower));
    ASSERT_EQ(hexblob::decode(upper), all);
    ASSERT_EQ(hexblob::decode(lower), all);
}

TEST(HexblobTest, EncodeRawMemory) {
    static const unsigned char data[] = {0xde, 0xad, 0xbe, 0xef};
    ASSERT_EQ(hexblob::encode(data, sizeof(data)), "DEADBEEF");
    ASSERT_EQ(hexblob::encodeLower(data, sizeof(data)), "deadbeef");
    ASSERT_EQ(hexblob::encode(data, 0), "");
    ASSERT_EQ(hexblob::encodeLower(data, 0), "");
}

TEST(HexblobTest, DecodeToString) {
    ASSERT_EQ(hexblob::decode(""sv), "");
    ASSERT_EQ(hexblob::decode("616263"sv), "abc");
    ASSERT_EQ(hexblob::decode("DEADBEEF"sv), "\xde\xad\xbe\xef");
    ASSERT_EQ(hexblob::decode("deadbeef"sv), "\xde\xad\xbe\xef") << "lower case accepted";
    ASSERT_EQ(hexblob::decode("DeAdBeEf"sv), "\xde\xad\xbe\xef") << "mixed case accepted";
    ASSERT_EQ(hexblob::decode("0000"sv), "\0\0"sv);
}

TEST(HexblobTest, DecodeToStringInvalid) {
    ASSERT_THROWS_CODE(hexblob::decode("0"sv), DBException, ErrorCodes::FailedToParse);
    ASSERT_THROWS_CODE(hexblob::decode("000"sv), DBException, ErrorCodes::FailedToParse);
    ASSERT_THROWS_CODE(hexblob::decode("zz"sv), DBException, ErrorCodes::FailedToParse);
    ASSERT_THROWS_CODE(hexblob::decode("00 11"sv), DBException, ErrorCodes::FailedToParse);
}

TEST(HexblobTest, DecodeDigit) {
    for (char c : "0123456789"sv)
        ASSERT_EQ(hexblob::decodeDigit(c), 0x0 + (c - '0'));
    for (char c : "abcdef"sv)
        ASSERT_EQ(hexblob::decodeDigit(c), 0xa + (c - 'a'));
    for (char c : "ABCDEF"sv)
        ASSERT_EQ(hexblob::decodeDigit(c), 0xa + (c - 'A'));
}

TEST(HexblobTest, DecodeDigitInvalid) {
    for (char c : "zZg-\0"sv)
        ASSERT_THROWS_CODE(hexblob::decodeDigit(c), DBException, ErrorCodes::FailedToParse);
}

TEST(HexblobTest, DecodeToBufBuilder) {
    BufBuilder buf;
    hexblob::decode("DEADBEEF"sv, &buf);
    ASSERT_EQ(std::string_view(buf.buf(), buf.len()), "\xde\xad\xbe\xef"sv);

    // Appends rather than replaces.
    hexblob::decode("00"sv, &buf);
    ASSERT_EQ(std::string_view(buf.buf(), buf.len()), "\xde\xad\xbe\xef\0"sv);

    // An empty blob is a no-op.
    hexblob::decode(""sv, &buf);
    ASSERT_EQ(buf.len(), 5);
}

TEST(HexblobTest, DecodeToBufBuilderInvalid) {
    BufBuilder buf;
    ASSERT_THROWS_CODE(hexblob::decode("F"sv, &buf), DBException, ErrorCodes::FailedToParse);
    ASSERT_EQ(buf.len(), 0) << "nothing appended for an odd digit count";
    ASSERT_THROWS_CODE(hexblob::decode("XY"sv, &buf), DBException, ErrorCodes::FailedToParse);
}

TEST(HexblobTest, DecodeFromValidSizedInput) {
    ASSERT_EQ(hexblob::decodeFromValidSizedInput(""sv), "");
    ASSERT_EQ(hexblob::decodeFromValidSizedInput("616263"sv), "abc");
    ASSERT_EQ(hexblob::decodeFromValidSizedInput("DeAdBeEf"sv), "\xde\xad\xbe\xef");
    ASSERT_THROWS_CODE(
        hexblob::decodeFromValidSizedInput("zz"sv), DBException, ErrorCodes::FailedToParse);
}

TEST(HexblobTest, EncodeDecodeRoundTrip) {
    for (auto s : {""sv, "a"sv, "hello world"sv, "\xff\x00\x01"sv}) {
        ASSERT_EQ(hexblob::decode(hexblob::encode(s)), s) << "input " << s;
        ASSERT_EQ(hexblob::decode(hexblob::encodeLower(s)), s) << "input " << s;
    }
}

TEST(HexdumpTest, Basic) {
    ASSERT_EQ(hexdump(""sv), "");
    ASSERT_EQ(hexdump("\x01"sv), "01");
    ASSERT_EQ(hexdump("\xde\xad\xbe\xef"sv), "de ad be ef") << "lower case, space separated";
    ASSERT_EQ(hexdump("\x00\x7f\x80"sv), "00 7f 80");
}

TEST(HexdumpTest, RawMemory) {
    static const unsigned char data[] = {0x00, 0x0f, 0xf0};
    ASSERT_EQ(hexdump(data, sizeof(data)), "00 0f f0");
    ASSERT_EQ(hexdump(data, 0), "");
    ASSERT_EQ(hexdump(data, 1), "00");
}

TEST(HexdumpTest, MaximumSize) {
    // The dumped length must be strictly less than kHexDumpMaxSize.
    const std::string data(kHexDumpMaxSize - 1, 'x');
    ASSERT_EQ(hexdump(data).size(), 3 * data.size() - 1);
}

// A tassert leaves a tripwire behind, which aborts the process at exit, so this must be a death
// test.
DEATH_TEST_REGEX(HexdumpDeathTest, TooLarge, "7781000.*Data length exceeds maximum buffer size") {
    ASSERT_THROWS_CODE(hexdump(std::string(kHexDumpMaxSize, 'x')), DBException, 7781000);
}

TEST(HexdumpTest, Streamable) {
    auto str = [](const void* data, size_t size) {
        std::ostringstream os;
        os << StreamableHexdump(data, size);
        return os.str();
    };
    static const unsigned char data[] = {0xde, 0xad, 0xbe, 0xef};
    ASSERT_EQ(str(data, sizeof(data)), "de ad be ef");
    ASSERT_EQ(str(data, 1), "de");
    ASSERT_EQ(str(data, 0), "");
}

TEST(HexdumpTest, StreamableMatchesHexdump) {
    std::string all(256, '\0');
    std::iota(all.begin(), all.end(), '\0');
    std::ostringstream os;
    os << StreamableHexdump(all.data(), all.size());
    ASSERT_EQ(os.str(), hexdump(all));
}

TEST(HexdumpTest, StreamablePreservesStreamFormatting) {
    // The dump must not be affected by (or leak) stream flags such as `hex` or `showbase`.
    static const unsigned char data[] = {0x0a};
    std::ostringstream os;
    os << std::hex << std::showbase << StreamableHexdump(data, sizeof(data)) << " " << 255;
    ASSERT_EQ(os.str(), "0a 0xff");
}

TEST(ZeroPaddedHexTest, Widths) {
    ASSERT_EQ(zeroPaddedHex(uint8_t{0}), "00");
    ASSERT_EQ(zeroPaddedHex(uint8_t{0xab}), "AB");
    ASSERT_EQ(zeroPaddedHex(uint16_t{0x1234}), "1234");
    ASSERT_EQ(zeroPaddedHex(uint32_t{0x1234}), "00001234");
    ASSERT_EQ(zeroPaddedHex(uint64_t{0x1234}), "0000000000001234");
    ASSERT_EQ(zeroPaddedHex(std::numeric_limits<uint64_t>::max()), "FFFFFFFFFFFFFFFF");
}

TEST(ZeroPaddedHexTest, SignedValuesRenderAsUnsigned) {
    ASSERT_EQ(zeroPaddedHex(int8_t{-1}), "FF");
    ASSERT_EQ(zeroPaddedHex(int32_t{-1}), "FFFFFFFF");
    ASSERT_EQ(zeroPaddedHex(int64_t{-1}), "FFFFFFFFFFFFFFFF");
    ASSERT_EQ(zeroPaddedHex(std::numeric_limits<int32_t>::min()), "80000000");
}

TEST(UnsignedHexTest, Basic) {
    ASSERT_EQ(unsignedHex(0), "0");
    ASSERT_EQ(unsignedHex(uint8_t{0xab}), "AB");
    ASSERT_EQ(unsignedHex(uint32_t{0x1234}), "1234");
    ASSERT_EQ(unsignedHex(std::numeric_limits<uint64_t>::max()), "FFFFFFFFFFFFFFFF");
}

TEST(UnsignedHexTest, SignedValuesRenderAsUnsigned) {
    ASSERT_EQ(unsignedHex(int8_t{-1}), "FF");
    ASSERT_EQ(unsignedHex(int32_t{-1}), "FFFFFFFF");
    ASSERT_EQ(unsignedHex(std::numeric_limits<int32_t>::min()), "80000000");
}

}  // namespace
}  // namespace mongo
