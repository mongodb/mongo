/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/BinTokenReaderTester.h"

#include "mozilla/EndianUtils.h"
#include "gc/Zone.h"

namespace js {
namespace frontend {

using BinFields = BinTokenReaderTester::BinFields;
using AutoList = BinTokenReaderTester::AutoList;
using AutoTaggedTuple = BinTokenReaderTester::AutoTaggedTuple;
using AutoTuple = BinTokenReaderTester::AutoTuple;

BinTokenReaderTester::BinTokenReaderTester(JSContext* cx, const uint8_t* start, const size_t length)
    : cx_(cx)
    , start_(start)
    , current_(start)
    , stop_(start + length)
    , latestKnownGoodPos_(0)
{ }

BinTokenReaderTester::BinTokenReaderTester(JSContext* cx, const Vector<uint8_t>& chars)
    : cx_(cx)
    , start_(chars.begin())
    , current_(chars.begin())
    , stop_(chars.end())
    , latestKnownGoodPos_(0)
{ }

bool
BinTokenReaderTester::raiseError(const char* description)
{
    MOZ_ASSERT(!cx_->isExceptionPending());
    TokenPos pos = this->pos();
    JS_ReportErrorASCII(cx_, "BinAST parsing error: %s at offsets %u => %u",
                        description, pos.begin, pos.end);
    return false;
}

bool
BinTokenReaderTester::readBuf(uint8_t* bytes, uint32_t len)
{
    MOZ_ASSERT(!cx_->isExceptionPending());
    MOZ_ASSERT(len > 0);

    if (stop_ < current_ + len)
        return raiseError("Buffer exceeds length");

    for (uint32_t i = 0; i < len; ++i)
        *bytes++ = *current_++;

    return true;
}

bool
BinTokenReaderTester::readByte(uint8_t* byte)
{
    return readBuf(byte, 1);
}


// Nullable booleans:
//
// 0 => false
// 1 => true
// 2 => null
bool
BinTokenReaderTester::readMaybeBool(Maybe<bool>& result)
{
    updateLatestKnownGood();
    uint8_t byte;
    if (!readByte(&byte))
        return false;

    switch (byte) {
      case 0:
        result = Some(false);
        break;
      case 1:
        result = Some(true);
        break;
      case 2:
        result = Nothing();
        break;
      default:
        return raiseError("Invalid boolean value");
    }
    return true;
}

bool
BinTokenReaderTester::readBool(bool& out)
{
    Maybe<bool> result;

    if (!readMaybeBool(result))
        return false;

    if (result.isNothing())
        return raiseError("Empty boolean value");

    out = *result;
    return true;
}

// Nullable doubles (little-endian)
//
// 0x7FF0000000000001 (signaling NaN) => null
// anything other 64 bit sequence => IEEE-764 64-bit floating point number
bool
BinTokenReaderTester::readMaybeDouble(Maybe<double>& result)
{
    updateLatestKnownGood();

    uint8_t bytes[8];
    MOZ_ASSERT(sizeof(bytes) == sizeof(double));
    if (!readBuf(reinterpret_cast<uint8_t*>(bytes), ArrayLength(bytes)))
        return false;

    // Decode little-endian.
    const uint64_t asInt = LittleEndian::readUint64(bytes);

    if (asInt == 0x7FF0000000000001) {
        result = Nothing();
    } else {
        // Canonicalize NaN, just to make sure another form of signalling NaN
        // doesn't slip past us.
        const double asDouble = CanonicalizeNaN(BitwiseCast<double>(asInt));
        result = Some(asDouble);
    }

    return true;
}

bool
BinTokenReaderTester::readDouble(double& out)
{
    Maybe<double> result;

    if (!readMaybeDouble(result))
        return false;

    if (result.isNothing())
        return raiseError("Empty double value");

    out = *result;
    return true;
}

// Internal uint32_t
//
// Encoded as 4 bytes, little-endian.
bool
BinTokenReaderTester::readInternalUint32(uint32_t* result)
{
    uint8_t bytes[4];
    MOZ_ASSERT(sizeof(bytes) == sizeof(uint32_t));
    if (!readBuf(bytes, 4))
        return false;

    // Decode little-endian.
    *result = LittleEndian::readUint32(bytes);

    return true;
}



// Nullable strings:
// - "<string>" (not counted in byte length)
// - byte length (not counted in byte length)
// - bytes (UTF-8)
// - "</string>" (not counted in byte length)
//
// The special sequence of bytes `[255, 0]` (which is an invalid UTF-8 sequence)
// is reserved to `null`.
bool
BinTokenReaderTester::readMaybeChars(Maybe<Chars>& out)
{
    updateLatestKnownGood();

    if (!readConst("<string>"))
        return false;

    // 1. Read byteLength
    uint32_t byteLen;
    if (!readInternalUint32(&byteLen))
        return false;

    // 2. Reject if we can't read
    if (current_ + byteLen < current_) // Check for overflows
        return raiseError("Arithmetics overflow: string is too long");

    if (current_ + byteLen > stop_)
        return raiseError("Not enough bytes to read chars");

    // 3. Check null string (no allocation)
    if (byteLen == 2 && *current_ == 255 && *(current_ + 1) == 0) {
        // Special case: null string.
        out = Nothing();
        current_ += byteLen;
        return true;
    }

    // 4. Other strings (bytes are copied)
    out.emplace(cx_);
    if (!out->resize(byteLen)) {
        ReportOutOfMemory(cx_);
        return false;
    }
    PodCopy(out->begin(), current_, byteLen);
    current_ += byteLen;

    if (!readConst("</string>"))
        return false;

    return true;
}

bool
BinTokenReaderTester::readChars(Chars& out)
{
    Maybe<Chars> result;

    if (!readMaybeChars(result))
        return false;

    if (result.isNothing())
        return raiseError("Empty string");

    out = Move(*result);
    return true;
}

template <size_t N>
bool
BinTokenReaderTester::matchConst(const char (&value)[N])
{
    MOZ_ASSERT(N > 0);
    MOZ_ASSERT(value[N - 1] == 0);
    MOZ_ASSERT(!cx_->isExceptionPending());

    if (current_ + N - 1 > stop_)
        return false;

    // Perform lookup, without side-effects.
    if (!std::equal(current_, current_ + N - 1 /*implicit NUL*/, value))
        return false;

    // Looks like we have a match. Now perform side-effects
    current_ += N - 1;
    updateLatestKnownGood();
    return true;
}


// Untagged tuple:
// - "<tuple>";
// - contents (specified by the higher-level grammar);
// - "</tuple>"
bool
BinTokenReaderTester::enterUntaggedTuple(AutoTuple& guard)
{
    if (!readConst("<tuple>"))
        return false;

    guard.init();
    return true;
}

template <size_t N>
bool
BinTokenReaderTester::readConst(const char (&value)[N])
{
    updateLatestKnownGood();
    if (!matchConst(value))
        return raiseError("Could not find expected literal");

    return true;
}

// Tagged tuples:
// - "<tuple>"
// - "<head>"
// - non-null string `name`, followed by \0 (see `readString()`);
// - uint32_t number of fields;
// - array of `number of fields` non-null strings followed each by \0 (see `readString()`);
// - "</head>"
// - content (specified by the higher-level grammar);
// - "</tuple>"
bool
BinTokenReaderTester::enterTaggedTuple(BinKind& tag, BinFields& fields, AutoTaggedTuple& guard)
{
    // Header
    if (!readConst("<tuple>"))
        return false;

    if (!readConst("<head>"))
        return false;

    // This would probably be much faster with a HashTable, but we don't
    // really care about the speed of BinTokenReaderTester.
    do {

#define FIND_MATCH(CONSTRUCTOR, NAME) \
        if (matchConst(#NAME "\0")) { \
            tag = BinKind::CONSTRUCTOR; \
            break; \
        } // else

        FOR_EACH_BIN_KIND(FIND_MATCH)
#undef FIND_MATCH

        // else
        return raiseError("Invalid tag");
    } while(false);

    // Now fields.
    uint32_t fieldNum;
    if (!readInternalUint32(&fieldNum))
        return false;

    fields.clear();
    if (!fields.reserve(fieldNum))
        return raiseError("Out of memory");

    for (uint32_t i = 0; i < fieldNum; ++i) {
        // This would probably be much faster with a HashTable, but we don't
        // really care about the speed of BinTokenReaderTester.
        BinField field;
        do {

#define FIND_MATCH(CONSTRUCTOR, NAME) \
            if (matchConst(#NAME "\0")) { \
                field = BinField::CONSTRUCTOR; \
                break; \
            } // else

            FOR_EACH_BIN_FIELD(FIND_MATCH)
#undef FIND_MATCH

            // else
            return raiseError("Invalid field");
        } while (false);

        // Make sure that we do not have duplicate fields.
        // Search is linear, but again, we don't really care
        // in this implementation.
        for (uint32_t j = 0; j < i; ++j) {
            if (fields[j] == field) {
                return raiseError("Duplicate field");
            }
        }

        fields.infallibleAppend(field); // Already checked.
    }

    // End of header

    if (!readConst("</head>"))
        return false;

    // Enter the body.
    guard.init();
    return true;
}

// List:
//
// - "<list>" (not counted in byte length);
// - uint32_t byte length (not counted in byte length);
// - uint32_t number of items;
// - contents (specified by higher-level grammar);
// - "</list>" (not counted in byte length)
//
// The total byte length of `number of items` + `contents` must be `byte length`.
bool
BinTokenReaderTester::enterList(uint32_t& items, AutoList& guard)
{
    if (!readConst("<list>"))
        return false;

    uint32_t byteLen;
    if (!readInternalUint32(&byteLen))
        return false;

    const uint8_t* stop = current_ + byteLen;

    if (stop < current_) // Check for overflows
        return raiseError("Arithmetics overflow: list is too long");

    if (stop > this->stop_)
        return raiseError("Incorrect list length");

    guard.init(stop);

    if (!readInternalUint32(&items))
        return false;

    return true;
}

void
BinTokenReaderTester::updateLatestKnownGood()
{
    MOZ_ASSERT(current_ >= start_);
    const size_t update = current_ - start_;
    MOZ_ASSERT(update >= latestKnownGoodPos_);
    latestKnownGoodPos_ = update;
}

size_t
BinTokenReaderTester::offset() const
{
    return latestKnownGoodPos_;
}

TokenPos
BinTokenReaderTester::pos()
{
    return pos(latestKnownGoodPos_);
}

TokenPos
BinTokenReaderTester::pos(size_t start)
{
    TokenPos pos;
    pos.begin = start;
    pos.end = current_ - start_;
    MOZ_ASSERT(pos.end >= pos.begin);
    return pos;
}

void
BinTokenReaderTester::AutoBase::init()
{
    initialized_ = true;
}

BinTokenReaderTester::AutoBase::AutoBase(BinTokenReaderTester& reader)
    : reader_(reader)
{ }

BinTokenReaderTester::AutoBase::~AutoBase()
{
    // By now, the `AutoBase` must have been deinitialized by calling `done()`.
    // The only case in which we can accept not calling `done()` is if we have
    // bailed out because of an error.
    MOZ_ASSERT_IF(initialized_, reader_.cx_->isExceptionPending());
}

bool
BinTokenReaderTester::AutoBase::checkPosition(const uint8_t* expectedEnd)
{
    if (reader_.current_ != expectedEnd)
        return reader_.raiseError("Caller did not consume the expected set of bytes");

    return true;
}

BinTokenReaderTester::AutoList::AutoList(BinTokenReaderTester& reader)
    : AutoBase(reader)
{ }

void
BinTokenReaderTester::AutoList::init(const uint8_t* expectedEnd)
{
    AutoBase::init();
    this->expectedEnd_ = expectedEnd;
}

bool
BinTokenReaderTester::AutoList::done()
{
    MOZ_ASSERT(initialized_);
    initialized_ = false;
    if (reader_.cx_->isExceptionPending()) {
        // Already errored, no need to check further.
        return false;
    }

    // Check that we have consumed the exact number of bytes.
    if (!checkPosition(expectedEnd_))
        return false;

    // Check suffix.
    if (!reader_.readConst("</list>"))
        return false;

    return true;
}

BinTokenReaderTester::AutoTaggedTuple::AutoTaggedTuple(BinTokenReaderTester& reader)
    : AutoBase(reader)
{ }

bool
BinTokenReaderTester::AutoTaggedTuple::done()
{
    MOZ_ASSERT(initialized_);
    initialized_ = false;
    if (reader_.cx_->isExceptionPending()) {
        // Already errored, no need to check further.
        return false;
    }

    // Check suffix.
    if (!reader_.readConst("</tuple>"))
        return false;

    return true;
}

BinTokenReaderTester::AutoTuple::AutoTuple(BinTokenReaderTester& reader)
    : AutoBase(reader)
{ }

bool
BinTokenReaderTester::AutoTuple::done()
{
    MOZ_ASSERT(initialized_);
    initialized_ = false;
    if (reader_.cx_->isExceptionPending()) {
        // Already errored, no need to check further.
        return false;
    }

    // Check suffix.
    if (!reader_.readConst("</tuple>"))
        return false;

    return true;
}

} // namespace frontend
} // namespace js
