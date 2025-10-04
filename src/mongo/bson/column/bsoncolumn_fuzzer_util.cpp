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

#include "mongo/bson/column/bsoncolumn_fuzzer_util.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/column/simple8b_helpers.h"
#include "mongo/bson/column/simple8b_type_util.h"

#include <absl/numeric/int128.h>

namespace mongo::bsoncolumn {

static constexpr size_t kRecursionLimit = BSONObj::maxToStringRecursionDepth + 1;

// in element creation functions, we use a common elementMemory as storage
// to hold content in scope for the lifetime on the fuzzer

BSONElement createBSONColumn(const char* buffer,
                             int size,
                             std::forward_list<BSONObj>& elementMemory) {
    BSONObjBuilder ob;
    ob.appendBinData(""_sd, size, BinDataType::Column, buffer);
    elementMemory.emplace_front(ob.obj());
    return elementMemory.front().firstElement();
}

template <typename T>
BSONElement createElement(T val, std::forward_list<BSONObj>& elementMemory) {
    BSONObjBuilder ob;
    ob.append("0"_sd, val);
    elementMemory.emplace_front(ob.obj());
    return elementMemory.front().firstElement();
}

BSONElement createElementDouble(double val, std::forward_list<BSONObj>& elementMemory) {
    BSONObjBuilder ob;
    ob.append("0"_sd, val);
    elementMemory.emplace_front(ob.obj());
    return elementMemory.front().firstElement();
}

BSONElement createObjectId(OID val, std::forward_list<BSONObj>& elementMemory) {
    return createElement(val, elementMemory);
}

BSONElement createTimestamp(Timestamp val, std::forward_list<BSONObj>& elementMemory) {
    return createElement(val, elementMemory);
}

BSONElement createElementInt64(int64_t val, std::forward_list<BSONObj>& elementMemory) {
    return createElement(val, elementMemory);
}

BSONElement createElementInt32(int32_t val, std::forward_list<BSONObj>& elementMemory) {
    return createElement(val, elementMemory);
}

BSONElement createElementDecimal128(Decimal128 val, std::forward_list<BSONObj>& elementMemory) {
    return createElement(val, elementMemory);
}

BSONElement createDate(Date_t dt, std::forward_list<BSONObj>& elementMemory) {
    return createElement(dt, elementMemory);
}

BSONElement createBool(bool b, std::forward_list<BSONObj>& elementMemory) {
    return createElement(b, elementMemory);
}

BSONElement createElementMinKey(std::forward_list<BSONObj>& elementMemory) {
    BSONObjBuilder ob;
    ob.appendMinKey("0"_sd);
    elementMemory.emplace_front(ob.obj());
    return elementMemory.front().firstElement();
}

BSONElement createElementMaxKey(std::forward_list<BSONObj>& elementMemory) {
    BSONObjBuilder ob;
    ob.appendMaxKey("0"_sd);
    elementMemory.emplace_front(ob.obj());
    return elementMemory.front().firstElement();
}

BSONElement createNull(std::forward_list<BSONObj>& elementMemory) {
    BSONObjBuilder ob;
    ob.appendNull("0"_sd);
    elementMemory.emplace_front(ob.obj());
    return elementMemory.front().firstElement();
}

BSONElement createUndefined(std::forward_list<BSONObj>& elementMemory) {
    BSONObjBuilder ob;
    ob.appendUndefined("0"_sd);
    elementMemory.emplace_front(ob.obj());
    return elementMemory.front().firstElement();
}

BSONElement createRegex(StringData pattern,
                        StringData options,
                        std::forward_list<BSONObj>& elementMemory) {
    BSONObjBuilder ob;
    ob.appendRegex("0"_sd, pattern, options);
    elementMemory.emplace_front(ob.obj());
    return elementMemory.front().firstElement();
}

BSONElement createDBRef(StringData ns, const OID& oid, std::forward_list<BSONObj>& elementMemory) {
    BSONObjBuilder ob;
    ob.appendDBRef("0"_sd, ns, oid);
    elementMemory.emplace_front(ob.obj());
    return elementMemory.front().firstElement();
}

BSONElement createElementCode(StringData code, std::forward_list<BSONObj>& elementMemory) {
    BSONObjBuilder ob;
    ob.appendCode("0"_sd, code);
    elementMemory.emplace_front(ob.obj());
    return elementMemory.front().firstElement();
}

BSONElement createCodeWScope(StringData code,
                             const BSONObj& scope,
                             std::forward_list<BSONObj>& elementMemory) {
    BSONObjBuilder ob;
    ob.appendCodeWScope("0"_sd, code, scope);
    elementMemory.emplace_front(ob.obj());
    return elementMemory.front().firstElement();
}

BSONElement createSymbol(StringData symbol, std::forward_list<BSONObj>& elementMemory) {
    BSONObjBuilder ob;
    ob.appendSymbol("0"_sd, symbol);
    elementMemory.emplace_front(ob.obj());
    return elementMemory.front().firstElement();
}

BSONElement createElementBinData(BinDataType binDataType,
                                 const char* buf,
                                 size_t len,
                                 std::forward_list<BSONObj>& elementMemory) {
    BSONObjBuilder ob;
    ob.appendBinData("f", len, binDataType, buf);
    elementMemory.emplace_front(ob.obj());
    return elementMemory.front().firstElement();
}

BSONElement createElementString(StringData val, std::forward_list<BSONObj>& elementMemory) {
    return createElement(val, elementMemory);
}

BSONElement createElementObj(BSONObj obj, std::forward_list<BSONObj>& elementMemory) {
    return createElement(obj, elementMemory);
}

BSONElement createElementArray(BSONArray arr, std::forward_list<BSONObj>& elementMemory) {
    return createElement(arr, elementMemory);
}

bool createFuzzedObj(const char*& ptr,
                     const char* end,
                     std::forward_list<BSONObj>& elementMemory,
                     BSONObj& result,
                     size_t depth);


// We restrict lengths of generated bufs to 25 bytes, this is not exhaustive but is
// enough to exercise the ways bsoncolumn behaves with these data (i.e. having some
// length variance both above and below the 128-bit cutoff where strings are treated
// differently by bsoncolumn) and adding more would slow down the fuzzer in finding
// edge cases more than help
constexpr size_t kMaxBufLength = 25;

// Reusable code for generating fuzzed buf content
bool generateBuf(const char*& ptr, const char* end, const char* buf, size_t& len) {
    // Generate len.
    size_t ptrLen = static_cast<size_t>(end - ptr);
    if (ptrLen < sizeof(uint8_t))
        return false;
    uint8_t lenRead;
    memcpy(&lenRead, ptr, sizeof(uint8_t));
    size_t maxLen = lenRead % (kMaxBufLength + 1);
    ptr += sizeof(uint8_t);
    ptrLen = static_cast<size_t>(end - ptr);

    // Pull out buf.
    len = std::min(maxLen, ptrLen);
    memcpy((void*)buf, ptr, len);
    ptr += len;
    return true;
};

bool generateBufNoNuls(const char*& ptr, const char* end, const char* buf, size_t& len) {
    // Generate max possible len the no-null string could be.
    size_t ptrLen = static_cast<size_t>(end - ptr);
    if (ptrLen < sizeof(uint8_t))
        return false;
    uint8_t lenRead;
    memcpy(&lenRead, ptr, sizeof(uint8_t));
    ptr += sizeof(uint8_t);
    ptrLen = static_cast<size_t>(end - ptr);

    // Pull out non-null buf.
    size_t maxLen = std::min(ptrLen, lenRead % (kMaxBufLength + 1));
    len = strnlen(ptr, maxLen);
    memcpy((void*)buf, ptr, len);

    // If the len returned by strnlen is maxLen, then we don't detect a null byte in the buffer.
    // Otherwise, we increment the ptr one past the null byte we detected.
    ptr += (len == maxLen ? len : len + 1);
    return true;
}

/**
 * Interpret the fuzzer input as a distribution on the full range of valid
 * BSONElement that could be passed to the builder.
 *
 * We could try to make this more compact, i.e. using the minimum number
 * of distinct bytes possible to represent the distribution of valid BSONElement.
 * However this is not what would most assist the fuzzer in exploring the
 * BSONElement space in a manner that exercises as many of the edge cases in the
 * code as quickly as possible.
 *
 * The fuzzer will try to build up a library of input strings, saving ones that
 * reach new code paths, and mutating to produce new ones by doing byte inserts,
 * deletes, and substitutions.  Thus, when a new code path is reached and we save
 * a new input string, we want the saved string to represent just the amount of
 * input that got us to the new path, without carrying "extra" state that would
 * point us to the next element or sub-element, since having such extra state
 * would restrain the variety of mutations we follow up with.
 *
 * Therefore, rather than trying to make the encoding compact by maximizing
 * utilization of the range of values, it is better to have each byte have
 * distinct meaning, and allow the fuzzer to navigate each range we want to
 * exercise along byte boundaries.  So we will use a distinct byte for type, and
 * use the next byte for content, etc.
 *
 * Additionally we will reuse values in the byte to make all 256 values have
 * semantic meaning, even if redundant, to minimize the times the fuzzer needs to
 * reject strings and reattempt mutations to find new BSONElement to feed to the
 * builder.
 *
 * ptr - pointer into the original fuzzer input, will be advanced
 * end - end of the original fuzzer input
 * elementMemory - needs to stay in scope for the lifetime of when we expect
 *                 generated elements to remain valid
 * repetition - number of instances to emit, may be discarded or used by caller
 * result - receives new BSONElement
 * depth - nesting depth of the element currently being generated, used to
 *         bound total depth
 * return - true if successful, false if fuzzer input is invalid
 */

// helpers to safely read one byte from [ptr, end)
#define READ_BYTE(ptr, end, out, arithmetic) \
    if (ptr >= end)                          \
        return false;                        \
    uint8_t out = *ptr arithmetic;           \
    ptr++;

bool createFuzzedElement(const char*& ptr,
                         const char* end,
                         std::forward_list<BSONObj>& elementMemory,
                         int& repetition,
                         BSONElement& result,
                         size_t depth) {
    // Interpret first byte as a BSONType inclusively
    // Valid types range from -1 to 19 and 127
    READ_BYTE(ptr, end, typeRun, );
    // There are 22 distinct types, interpret every possible value as one of them
    uint8_t typeMagnitude = typeRun % 22;
    BSONType type = typeMagnitude <= 19 ? static_cast<BSONType>(typeMagnitude)
                                        :  // EOO - NumberDecimal
        typeMagnitude == 20 ? BSONType::maxKey
                            :  // reinterpret 20 -> 127
        BSONType::minKey;      // reinterpret 21 -> -1
    // Use the remainder of the type entropy to represent repetition factor, this helps
    // bias the probability to trigger the RLE encoding more often
    //
    // We effectively have 3 remaining bits of entropy to work with, we use this to add
    // any or all of +1 (for more 0 deltas), +120 (the minimum amount to create an RLE
    // block), and +(16 * 120) (the maximum amount in an RLE block)
    //
    // If we choose to add one of these run types, we will pull another byte to
    // dictate how many within that order of magnitude to add.

    // repetitionFactor represents a scale of repetition (single, block, max block)
    // repetitionFactor can go up to 11, 0-8 mean no repetition, this is a bias
    // towards singletons so as not to have too many runs (each individual element
    // in a fuzzer input has this chance, so there will be many RLE cases explored)
    // 9 means we add 0-119 more copies of the element
    // 10 means we additionally add 0-15 full rle blocks (120 copies each)
    // 11 means we additionally add 0-9 max rle blocks (120 * 16 copies each)
    uint8_t repetitionFactor = typeRun / 22;
    repetition = 1;
    if (repetitionFactor >= 9) {  // add singletons of repetition
        READ_BYTE(ptr, end, singles, % 120);
        repetition += singles;
    }
    if (repetitionFactor >= 10) {  // add full rle blocks
        READ_BYTE(ptr, end, blocks, % 16);
        repetition += blocks * mongo::simple8b_internal::kRleMultiplier;
    }
    if (repetitionFactor >= 11) {  // add max rle blocks
        READ_BYTE(ptr, end, maxBlocks, % 10)
        repetition += maxBlocks * mongo::simple8b_internal::kRleMultiplier *
            mongo::simple8b_internal::kMaxRleCount;
    }

    // Construct a BSONElement based on type.
    size_t len;
    char buf[kMaxBufLength];
    switch (type) {
        case BSONType::array: {
            if (depth >= kRecursionLimit)
                return false;
            READ_BYTE(ptr, end, count, );

            UniqueBSONArrayBuilder bab;
            for (uint8_t i = 0; i < count; ++i) {
                BSONElement elem;
                int dummy;  // do not use repetition for arrays; we don't rle on this axis
                if (!createFuzzedElement(ptr, end, elementMemory, dummy, elem, depth + 1))
                    return false;
                if (elem.eoo())
                    return false;
                bab.append(elem);
            }
            bab.done();
            result = createElementArray(bab.arr(), elementMemory);
            return true;
        }
        case BSONType::binData: {
            READ_BYTE(ptr, end, binDataTypeMagnitude, );
            binDataTypeMagnitude %= 10;
            BinDataType binDataType = binDataTypeMagnitude <= 8
                ? static_cast<BinDataType>(binDataTypeMagnitude)
                : BinDataType::bdtCustom;
            if (!generateBuf(ptr, end, &buf[0], len))
                return false;
            result = createElementBinData(binDataType, &buf[0], len, elementMemory);
            return true;
        }
        case BSONType::code: {
            if (!generateBuf(ptr, end, &buf[0], len))
                return false;
            result = createElementCode(StringData(buf, len), elementMemory);
            return true;
        }
        case BSONType::codeWScope: {
            if (depth >= kRecursionLimit)
                return false;
            if (!generateBuf(ptr, end, &buf[0], len))
                return false;
            BSONObj obj;
            if (!createFuzzedObj(ptr, end, elementMemory, obj, depth))
                return false;
            result = createCodeWScope(StringData(buf, len), obj, elementMemory);
            return true;
        }
        case BSONType::dbRef: {
            if (!generateBuf(ptr, end, &buf[0], len))
                return false;
            // Initialize an OID from a 12 byte array
            if (end - ptr < 12)
                return false;
            unsigned char arr[12];
            memcpy(arr, ptr, 12);
            ptr += 12;
            OID oid(arr);
            result = createDBRef(StringData(buf, len), oid, elementMemory);
            return true;
        }
        case BSONType::object: {
            if (depth >= kRecursionLimit)
                return false;
            BSONObj obj;
            if (!createFuzzedObj(ptr, end, elementMemory, obj, depth))
                return false;
            result = createElementObj(obj, elementMemory);
            return true;
        }
        case BSONType::regEx: {
            if (!generateBufNoNuls(ptr, end, &buf[0], len))
                return false;
            auto patternStr = StringData(buf, len);

            char optionsBuf[kMaxBufLength];
            size_t optionsLen;
            if (!generateBufNoNuls(ptr, end, &optionsBuf[0], optionsLen))
                return false;
            auto optionsStr = StringData(optionsBuf, optionsLen);

            result = createRegex(patternStr, optionsStr, elementMemory);
            return true;
        }
        case BSONType::string: {
            if (!generateBuf(ptr, end, &buf[0], len))
                return false;
            result = createElementString(StringData(buf, len), elementMemory);
            return true;
        }
        case BSONType::symbol: {
            if (!generateBuf(ptr, end, &buf[0], len))
                return false;
            result = createSymbol(StringData(buf, len), elementMemory);
            return true;
        }
        case BSONType::boolean: {
            READ_BYTE(ptr, end, boolVal, % 2);
            result = createBool(boolVal == 1, elementMemory);
            return true;
        }
        case BSONType::timestamp: {
            if (static_cast<size_t>(end - ptr) < sizeof(long long))
                return false;
            long long val;
            memcpy(&val, ptr, sizeof(long long));
            ptr += sizeof(long long);
            Timestamp timestamp(val);
            result = createTimestamp(timestamp, elementMemory);
            return true;
        }
        case BSONType::date: {
            if (static_cast<size_t>(end - ptr) < sizeof(long long))
                return false;
            long long millis;
            memcpy(&millis, ptr, sizeof(long long));
            ptr += sizeof(long long);
            Date_t val = Date_t::fromMillisSinceEpoch(millis);
            result = createDate(val, elementMemory);
            return true;
        }
        case BSONType::eoo: {
            result = BSONElement();
            return true;
        }
        case BSONType::null: {
            result = createNull(elementMemory);
            return true;
        }
        case BSONType::oid: {
            // Initialize an OID from a 12 byte array
            if (end - ptr < 12)
                return false;
            unsigned char arr[12];
            memcpy(arr, ptr, 12);
            ptr += 12;
            OID val(arr);
            result = createObjectId(val, elementMemory);
            return true;
        }
        case BSONType::maxKey: {
            result = createElementMaxKey(elementMemory);
            return true;
        }
        case BSONType::minKey: {
            result = createElementMinKey(elementMemory);
            return true;
        }
        case BSONType::numberDecimal: {
            // Initialize a Decimal128 from parts
            if (static_cast<size_t>(end - ptr) < 4 * sizeof(uint64_t))
                return false;
            uint64_t sign, exponent, coeffHigh, coeffLow;
            memcpy(&sign, ptr, sizeof(uint64_t));
            ptr += sizeof(uint64_t);
            memcpy(&exponent, ptr, sizeof(uint64_t));
            ptr += sizeof(uint64_t);
            memcpy(&coeffHigh, ptr, sizeof(uint64_t));
            ptr += sizeof(uint64_t);
            memcpy(&coeffLow, ptr, sizeof(uint64_t));
            ptr += sizeof(uint64_t);
            if (!Decimal128::isValid(sign, exponent, coeffHigh, coeffLow))
                return false;
            Decimal128 val(sign, exponent, coeffHigh, coeffLow);
            result = createElementDecimal128(val, elementMemory);
            return true;
        }
        case BSONType::numberDouble: {
            if (static_cast<size_t>(end - ptr) < sizeof(double))
                return false;
            double val;
            memcpy(&val, ptr, sizeof(double));
            ptr += sizeof(double);
            result = createElementDouble(val, elementMemory);
            return true;
        }
        case BSONType::numberInt: {
            if (static_cast<size_t>(end - ptr) < sizeof(int32_t))
                return false;
            int32_t val;
            memcpy(&val, ptr, sizeof(int32_t));
            ptr += sizeof(int32_t);
            result = createElementInt32(val, elementMemory);
            return true;
        }
        case BSONType::numberLong: {
            if (static_cast<size_t>(end - ptr) < sizeof(int64_t))
                return false;
            int64_t val;
            memcpy(&val, ptr, sizeof(int64_t));
            ptr += sizeof(int64_t);
            result = createElementInt64(val, elementMemory);
            return true;
        }
        case BSONType::undefined: {
            result = createUndefined(elementMemory);
            return true;
        }
        default:
            MONGO_UNREACHABLE;
    }

    return false;
}

/* Obj fuzzing requires recursion to handle subobjects
 *
 * ptr - pointer into the original fuzzer input, will be advanced
 * end - end of the original fuzzer input
 * elementMemory - needs to stay in scope for the lifetime of when we expect
 *                 generated elements to remain valid
 * result - receives new BSONObj
 * return - true if successful, false if fuzzer input is invalid
 */
bool createFuzzedObj(const char*& ptr,
                     const char* end,
                     std::forward_list<BSONObj>& elementMemory,
                     BSONObj& result,
                     size_t depth) {
    // Use branching factor of objects of up to 255
    READ_BYTE(ptr, end, count, );

    BSONObjBuilder bob;
    for (uint8_t i = 0; i < count; ++i) {
        // Generate a field name
        size_t len;
        char buf[kMaxBufLength + 1];  // Extra byte to hold terminating 0
        if (!generateBuf(ptr, end, &buf[0], len))
            return false;
        for (size_t i = 0; i < len; ++i)
            if (buf[i] == 0)
                buf[i] = 1;
        buf[len] = 0;

        BSONElement elem;
        int dummy;  // do not use repetition for obj; we don't rle on this axis
        if (!createFuzzedElement(ptr, end, elementMemory, dummy, elem, depth + 1))
            return false;

        if (elem.eoo())
            return false;

        bob.appendAs(elem, StringData(buf, len));
    }
    bob.done();
    result = bob.obj();
    return true;
}

/* Fill in BSONElement(s) into a generated elements vector, with repetition,
 * respecting delta, delta-of-delta, and string/binary encodings as appropriate.
 * Uses fuzzer input to determine delta spacing as needed
 *
 * ptr - pointer into the original fuzzer input, will be advanced
 * end - end of the original fuzzer input
 * elementMemory - needs to stay in scope for the lifetime of when we expect
 *                 generated elements to remain valid
 * element - the element to populate with
 * repetition - the number of elements to add
 * generatedElements - vector to receive results
 * return - true if successful, false if fuzzer input is invalid
 */

bool addFuzzedElements(const char*& ptr,
                       const char* end,
                       std::forward_list<BSONObj>& elementMemory,
                       BSONElement element,
                       int repetition,
                       std::vector<BSONElement>& generatedElements) {

    auto process_delta_run = [&]<typename T, typename Create>(T val, const Create& create) {
        T maxVal = -1 + (1 << (sizeof(T) - 1));
        T minVal = 1 - (1 << (sizeof(T) - 1));
        // use fuzzed byte as a fractional delta increase, 0 means no delta
        READ_BYTE(ptr, end, factor, );
        T delta = val / 255 * factor;
        for (int i = 1; i < repetition; ++i) {
            if (delta > 0) {
                if (val > maxVal - delta)
                    return false;
            } else {
                if (val < minVal - delta)
                    return false;
            }
            val += delta;
            auto runElement = create(val, elementMemory);
            generatedElements.push_back(runElement);
        }
        return true;
    };

    auto process_delta_of_delta_run = [&]<typename T, typename Create>(T val,
                                                                       const Create& create) {
        T maxVal = -1 + (1 << (sizeof(T) - 1));
        T minVal = 1 - (1 << (sizeof(T) - 1));
        // use fuzzed byte as a fractional delta increase, 0 means no delta
        READ_BYTE(ptr, end, factor, );
        T delta = val / 255 * factor;
        T deltaOfDelta = delta;
        for (int i = 1; i < repetition; ++i) {
            if (delta > 0) {
                if (val > maxVal - delta)
                    return false;
            } else {
                if (val < minVal - delta)
                    return false;
            }
            val += delta;
            delta += deltaOfDelta;
            auto runElement = create(val, elementMemory);
            generatedElements.push_back(runElement);
        }
        return true;
    };

    auto process_delta_of_buffer_run = [&]<typename Create>(boost::optional<int128_t>& encodedVal,
                                                            const Create& create) {
        if (!encodedVal) {
            // Don't bother with delta encoding, only exercise 0-delta repeats
            for (int i = 1; i < repetition; ++i)
                generatedElements.push_back(element);
        } else {
            int128_t encoding = encodedVal.get();
            READ_BYTE(ptr, end, factor, );
            int128_t delta = encoding / 255 * factor;
            for (int i = 1; i < repetition; ++i) {
                if (delta > 0) {
                    if (encoding > absl::Int128Max() - delta)
                        return false;
                } else {
                    if (encoding > absl::Int128Min() - delta)
                        return false;
                }
                encoding += delta;
                auto smallString = Simple8bTypeUtil::decodeString(encoding);
                auto runElement = create(smallString, elementMemory);
                generatedElements.push_back(runElement);
            }
        }
        return true;
    };

    generatedElements.push_back(element);
    if (repetition > 1) {
        switch (element.type()) {
            case BSONType::numberDouble: {
                double doubleVal = element.Double();
                boost::optional<int64_t> val;
                uint8_t scale = 0;
                for (; !val; ++scale) {
                    val = Simple8bTypeUtil::encodeDouble(doubleVal, scale);
                }
                scale--;
                if (!process_delta_run(
                        val.get(), [&](int64_t v, std::forward_list<BSONObj>& elMem) {
                            double doubleV = Simple8bTypeUtil::decodeDouble(v, scale);
                            return mongo::bsoncolumn::createElementDouble(doubleV, elMem);
                        }))
                    return false;
                break;
            }
            case BSONType::numberInt: {
                int32_t val = element.Int();
                if (!process_delta_run(val, mongo::bsoncolumn::createElementInt32))
                    return false;
                break;
            }
            case BSONType::numberLong: {
                int64_t val = element.Long();
                if (!process_delta_run(val, mongo::bsoncolumn::createElementInt64))
                    return false;
                break;
            }
            case BSONType::string: {
                std::string val = element.str();
                boost::optional<int128_t> encodedVal = Simple8bTypeUtil::encodeString(val);
                if (!process_delta_of_buffer_run(encodedVal,
                                                 [&](mongo::Simple8bTypeUtil::SmallString ss,
                                                     std::forward_list<BSONObj>& elMem) {
                                                     return mongo::bsoncolumn::createElementString(
                                                         StringData(ss.str.data(), ss.size), elMem);
                                                 }))
                    return false;
                break;
            }
            case BSONType::code: {
                std::string val = element._asCode();
                boost::optional<int128_t> encodedVal = Simple8bTypeUtil::encodeString(val);
                if (!process_delta_of_buffer_run(encodedVal,
                                                 [&](mongo::Simple8bTypeUtil::SmallString ss,
                                                     std::forward_list<BSONObj>& elMem) {
                                                     return mongo::bsoncolumn::createElementCode(
                                                         StringData(ss.str.data(), ss.size), elMem);
                                                 }))
                    return false;
                break;
            }
            case BSONType::binData: {
                int len;
                const char* val = element.binData(len);
                boost::optional<int128_t> encodedVal =
                    Simple8bTypeUtil::encodeString(StringData(val, len));
                if (!process_delta_of_buffer_run(
                        encodedVal,
                        [&](mongo::Simple8bTypeUtil::SmallString ss,
                            std::forward_list<BSONObj>& elMem) {
                            BinDataType binDataType = element.binDataType();
                            return mongo::bsoncolumn::createElementBinData(
                                binDataType, ss.str.data(), ss.size, elMem);
                        }))
                    return false;
                break;
            }
            case BSONType::oid: {
                int64_t val = Simple8bTypeUtil::encodeObjectId(element.OID());
                if (!process_delta_of_delta_run(val,
                                                [&](int64_t v, std::forward_list<BSONObj>& elMem) {
                                                    return mongo::bsoncolumn::createObjectId(
                                                        Simple8bTypeUtil::decodeObjectId(
                                                            v, element.__oid().getInstanceUnique()),
                                                        elMem);
                                                }))
                    return false;
                break;
            }
            case BSONType::date: {
                long long val = element.date().toMillisSinceEpoch();
                if (!process_delta_of_delta_run(
                        val, [&](long long v, std::forward_list<BSONObj>& elMem) {
                            return mongo::bsoncolumn::createDate(Date_t::fromMillisSinceEpoch(v),
                                                                 elMem);
                        }))
                    return false;
                break;
            }
            case BSONType::timestamp: {
                long long val = element.timestampValue();
                if (!process_delta_of_delta_run(
                        val, [&](long long v, std::forward_list<BSONObj>& elMem) {
                            Timestamp timestamp(v);
                            return mongo::bsoncolumn::createTimestamp(timestamp, elMem);
                        }))
                    return false;
                break;
            }
            default: {
                for (int i = 1; i < repetition; ++i)
                    generatedElements.push_back(element);
                break;
            }
        }
    }

    return true;
}

}  // namespace mongo::bsoncolumn
