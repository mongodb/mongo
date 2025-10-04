/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/ftdc/compressor.h"

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/decompressor.h"
#include "mongo/db/ftdc/ftdc_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <random>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

#define ASSERT_HAS_SPACE(st) \
    ASSERT_TRUE(st.isOK());  \
    ASSERT_FALSE(st.getValue().is_initialized());

#define ASSERT_SCHEMA_CHANGED(st)                   \
    ASSERT_TRUE(st.isOK());                         \
    ASSERT_TRUE(st.getValue().is_initialized());    \
    ASSERT_TRUE(std::get<1>(st.getValue().get()) == \
                FTDCCompressor::CompressorState::kSchemaChanged);

#define ASSERT_FULL(st)                             \
    ASSERT_TRUE(st.isOK());                         \
    ASSERT_TRUE(st.getValue().is_initialized());    \
    ASSERT_TRUE(std::get<1>(st.getValue().get()) == \
                FTDCCompressor::CompressorState::kCompressorFull);

class FTDCCompressorTest : public FTDCTest {};

// Sanity check
TEST_F(FTDCCompressorTest, TestBasic) {
    FTDCConfig config;
    FTDCCompressor c(&config);

    auto st = c.addSample(BSON("name" << "joe"
                                      << "key1" << 33 << "key2" << 42),
                          Date_t());
    ASSERT_HAS_SPACE(st);
    st = c.addSample(BSON("name" << "joe"
                                 << "key1" << 34 << "key2" << 45),
                     Date_t());
    ASSERT_HAS_SPACE(st);


    StatusWith<std::tuple<ConstDataRange, Date_t>> swBuf = c.getCompressedSamples();

    ASSERT_TRUE(swBuf.isOK());
    ASSERT_TRUE(std::get<0>(swBuf.getValue()).length() > 0);
    ASSERT_TRUE(std::get<0>(swBuf.getValue()).data() != nullptr);
}

// Test strings only
TEST_F(FTDCCompressorTest, TestStrings) {
    FTDCConfig config;
    FTDCCompressor c(&config);

    auto st = c.addSample(BSON("name" << "joe"
                                      << "key1"
                                      << "value1"
                                      << "key2"
                                      << "value2"),
                          Date_t());
    ASSERT_HAS_SPACE(st);
    st = c.addSample(BSON("name" << "joe"
                                 << "key1"
                                 << "value3"
                                 << "key2"
                                 << "value6"),
                     Date_t());
    ASSERT_HAS_SPACE(st);

    StatusWith<std::tuple<ConstDataRange, Date_t>> swBuf = c.getCompressedSamples();

    ASSERT_TRUE(swBuf.isOK());
    ASSERT_TRUE(std::get<0>(swBuf.getValue()).length() > 0);
    ASSERT_TRUE(std::get<0>(swBuf.getValue()).data() != nullptr);
}

/**
 * Test class that records a series of samples and ensures that compress + decompress round trips
 * them correctly.
 */
class TestTie {
public:
    TestTie(FTDCValidationMode mode = FTDCValidationMode::kStrict)
        : _compressor(&_config), _mode(mode) {}

    ~TestTie() {
        validate(boost::none);
    }

    StatusWith<boost::optional<std::tuple<ConstDataRange, FTDCCompressor::CompressorState, Date_t>>>
    addSample(const BSONObj& sample) {
        auto st = _compressor.addSample(sample, Date_t());

        if (!st.getValue().has_value()) {
            _docs.emplace_back(sample);
        } else if (std::get<1>(st.getValue().value()) ==
                   FTDCCompressor::CompressorState::kSchemaChanged) {
            validate(std::get<0>(st.getValue().value()));
            _docs.clear();
            _docs.emplace_back(sample);
        } else if (std::get<1>(st.getValue().value()) ==
                   FTDCCompressor::CompressorState::kCompressorFull) {
            _docs.emplace_back(sample);
            validate(std::get<0>(st.getValue().value()));
            _docs.clear();
        } else {
            MONGO_UNREACHABLE;
        }

        return st;
    }

    void validate(boost::optional<ConstDataRange> cdr) {
        std::vector<BSONObj> list;
        if (cdr.has_value()) {
            auto sw = _decompressor.uncompress(cdr.value());
            ASSERT_TRUE(sw.isOK());
            list = sw.getValue();
        } else {
            auto swBuf = _compressor.getCompressedSamples();
            ASSERT_TRUE(swBuf.isOK());
            auto sw = _decompressor.uncompress(std::get<0>(swBuf.getValue()));
            ASSERT_TRUE(sw.isOK());

            list = sw.getValue();
        }

        ValidateDocumentList(list, _docs, _mode);
    }

    void setExpectedDocuments(const std::vector<BSONObj>& docs) {
        _docs.clear();
        std::copy(docs.begin(), docs.end(), std::back_inserter(_docs));
    }

private:
    std::vector<BSONObj> _docs;
    FTDCConfig _config;
    FTDCCompressor _compressor;
    FTDCDecompressor _decompressor;
    FTDCValidationMode _mode;
};

// Test various schema changes
TEST_F(FTDCCompressorTest, TestSchemaChanges) {
    TestTie c;

    auto st = c.addSample(BSON("name" << "joe"
                                      << "key1" << 33 << "key2" << 42));
    ASSERT_HAS_SPACE(st);
    st = c.addSample(BSON("name" << "joe"
                                 << "key1" << 34 << "key2" << 45));
    ASSERT_HAS_SPACE(st);
    st = c.addSample(BSON("name" << "joe"
                                 << "key1" << 34 << "key2" << 45));
    ASSERT_HAS_SPACE(st);

    // Add Field
    st = c.addSample(BSON("name" << "joe"
                                 << "key1" << 34 << "key2" << 45 << "key3" << 47));
    ASSERT_SCHEMA_CHANGED(st);

    st = c.addSample(BSON("name" << "joe"
                                 << "key1" << 34 << "key2" << 45 << "key3" << 47));
    ASSERT_HAS_SPACE(st);

    // Rename field
    st = c.addSample(BSON("name" << "joe"
                                 << "key1" << 34 << "key5" << 45 << "key3" << 47));
    ASSERT_SCHEMA_CHANGED(st);

    // Change type
    st = c.addSample(BSON("name" << "joe"
                                 << "key1" << 34 << "key5"
                                 << "45"
                                 << "key3" << 47));
    ASSERT_SCHEMA_CHANGED(st);

    // Add Field
    st = c.addSample(BSON("name" << "joe"
                                 << "key1" << 34 << "key2" << 45 << "key3" << 47 << "key7" << 34
                                 << "key9" << 45 << "key13" << 47));
    ASSERT_SCHEMA_CHANGED(st);

    // Remove Field
    st = c.addSample(BSON("name" << "joe"
                                 << "key7" << 34 << "key9" << 45 << "key13" << 47));
    ASSERT_SCHEMA_CHANGED(st);

    st = c.addSample(BSON("name" << "joe"
                                 << "key7" << 34 << "key9" << 45 << "key13" << 47));
    ASSERT_HAS_SPACE(st);

    // Start new batch
    st = c.addSample(BSON("name" << "joe"
                                 << "key7" << 5));
    ASSERT_SCHEMA_CHANGED(st);

    // Change field to object
    st = c.addSample(BSON("name" << "joe"
                                 << "key7"
                                 << BSON(  // nested object
                                        "a" << 1)));
    ASSERT_SCHEMA_CHANGED(st);

    // Change field from object to number
    st = c.addSample(BSON("name" << "joe"
                                 << "key7" << 7));
    ASSERT_SCHEMA_CHANGED(st);

    // Change field from number to array
    st = c.addSample(BSON("name" << "joe"
                                 << "key7" << BSON_ARRAY(13 << 17)));
    ASSERT_SCHEMA_CHANGED(st);

    // Change field from array to number
    st = c.addSample(BSON("name" << "joe"
                                 << "key7" << 19));
    ASSERT_SCHEMA_CHANGED(st);


    // New Schema
    st = c.addSample(BSON("_id" << 1));
    ASSERT_SCHEMA_CHANGED(st);

    // Change field to oid
    st = c.addSample(BSON("_id" << OID::gen()));
    ASSERT_SCHEMA_CHANGED(st);

    // Change field from oid to object
    st = c.addSample(BSON("_id" << BSON("sub1" << 1)));
    ASSERT_SCHEMA_CHANGED(st);

    // Change field from object to oid
    st = c.addSample(BSON("_id" << OID::gen()));
    ASSERT_SCHEMA_CHANGED(st);
}

// Test various schema changes with strings
TEST_F(FTDCCompressorTest, TestStringSchemaChanges) {
    TestTie c(FTDCValidationMode::kWeak);

    auto st = c.addSample(BSON("str1" << "joe"
                                      << "int1" << 42));
    ASSERT_HAS_SPACE(st);
    st = c.addSample(BSON("str1" << "joe"
                                 << "int1" << 45));
    ASSERT_HAS_SPACE(st);

    // Add string field
    st = c.addSample(BSON("str1" << "joe"
                                 << "str2"
                                 << "smith"
                                 << "int1" << 47));
    ASSERT_HAS_SPACE(st);

    // Reset schema by renaming a int field
    st = c.addSample(BSON("str1" << "joe"
                                 << "str2"
                                 << "smith"
                                 << "int2" << 48));
    ASSERT_SCHEMA_CHANGED(st);

    // Remove string field
    st = c.addSample(BSON("str1" << "joe"
                                 << "int2" << 49));
    ASSERT_HAS_SPACE(st);


    // Add string field as last element
    st = c.addSample(BSON("str1" << "joe"
                                 << "int2" << 50 << "str3"
                                 << "bar"));
    ASSERT_HAS_SPACE(st);

    // Reset schema by renaming a int field
    st = c.addSample(BSON("str1" << "joe"
                                 << "int1" << 51 << "str3"
                                 << "bar"));
    ASSERT_SCHEMA_CHANGED(st);

    // Remove string field as last element
    st = c.addSample(BSON("str1" << "joe"
                                 << "int1" << 52));
    ASSERT_HAS_SPACE(st);


    // Add 2 string fields
    st = c.addSample(BSON("str1" << "joe"
                                 << "str2"
                                 << "smith"
                                 << "str3"
                                 << "foo"
                                 << "int1" << 53));
    ASSERT_HAS_SPACE(st);

    // Reset schema by renaming a int field
    st = c.addSample(BSON("str1" << "joe"
                                 << "str2"
                                 << "smith"
                                 << "str3"
                                 << "foo"
                                 << "int2" << 54));
    ASSERT_SCHEMA_CHANGED(st);

    // Remove 2 string fields
    st = c.addSample(BSON("str1" << "joe"
                                 << "int2" << 55));
    ASSERT_HAS_SPACE(st);

    // Change string to number
    st = c.addSample(BSON("str1" << 12 << "int1" << 56));
    ASSERT_SCHEMA_CHANGED(st);

    // Change number to string
    st = c.addSample(BSON("str1" << "joe"
                                 << "int1" << 67));
    ASSERT_SCHEMA_CHANGED(st);
}

// Ensure changing between the various number formats is considered compatible
TEST_F(FTDCCompressorTest, TestNumbersCompat) {
    TestTie c;

    auto st = c.addSample(BSON("name" << "joe"
                                      << "key1" << 33 << "key2" << 42LL));
    ASSERT_HAS_SPACE(st);
    st = c.addSample(BSON("name" << "joe"
                                 << "key1" << 34LL << "key2" << 45.0f));
    ASSERT_HAS_SPACE(st);
    st = c.addSample(BSON("name" << "joe"
                                 << "key1" << static_cast<char>(32) << "key2" << 45.0F));
    ASSERT_HAS_SPACE(st);
}

// Test various date time types
TEST_F(FTDCCompressorTest, TestDateTimeTypes) {
    TestTie c;
    for (int i = 0; i < 10; i++) {
        BSONObjBuilder builder1;
        builder1.append("ts", Timestamp(0x556677LL + i * 1356, 0x11223344LL + i * 2396));
        builder1.append("d1", Date_t::fromMillisSinceEpoch((0x556677LL + i * 1356) / 1000));
        BSONObj obj = builder1.obj().getOwned();

        auto st = c.addSample(obj);
        ASSERT_HAS_SPACE(st);
    }
}

// Test all types
TEST_F(FTDCCompressorTest, Types) {
    TestTie c;

    auto st = c.addSample(BSON("name" << "joe"
                                      << "key1" << 33 << "key2" << 42LL));
    ASSERT_HAS_SPACE(st);

    const char bytes[] = {0x1, 0x2, 0x3};
    BSONObj o = BSON("created" << DATENOW                       // date_t
                               << "null" << BSONNULL            // { a : null }
                               << "undefined" << BSONUndefined  // { a : undefined }
                               << "obj"
                               << BSON(  // nested object
                                      "a" << "abc"
                                          << "b" << 123LL)
                               << "foo"
                               << BSON_ARRAY("bar" << "baz"
                                                   << "qux")         // array of strings
                               << "foo2" << BSON_ARRAY(5 << 6 << 7)  // array of ints
                               << "bindata" << BSONBinData(&bytes[0], 3, bdtCustom)  // bindata
                               << "oid" << OID("010203040506070809101112")           // oid
                               << "bool" << true                                     // bool
                               << "regex" << BSONRegEx("mongodb")                    // regex
                               << "ref" << BSONDBRef("c", OID("010203040506070809101112"))  // ref
                               << "code" << BSONCode("func f() { return 1; }")              // code
                               << "codewscope"
                               << BSONCodeWScope("func f() { return 1; }",
                                                 BSON("c" << true))  // codew
                               << "minkey" << MINKEY                 // minkey
                               << "maxkey" << MAXKEY                 // maxkey
    );

    st = c.addSample(o);
    ASSERT_SCHEMA_CHANGED(st);

    st = c.addSample(o);
    ASSERT_HAS_SPACE(st);

    st = c.addSample(BSON("name" << "joe"
                                 << "key1" << 34LL << "key2" << 45.0f));
    ASSERT_SCHEMA_CHANGED(st);
    st = c.addSample(BSON("name" << "joe"
                                 << "key1" << static_cast<char>(32) << "key2" << 45.0F));
    ASSERT_HAS_SPACE(st);
}

// Test a full buffer
TEST_F(FTDCCompressorTest, TestFull) {
    // Test a large numbers of zeros, and incremental numbers in a full buffer
    for (int j = 0; j < 2; j++) {
        TestTie c;

        auto st = c.addSample(BSON("name" << "joe"
                                          << "key1" << 33 << "key2" << 42));
        ASSERT_HAS_SPACE(st);

        for (size_t i = 0; i != FTDCConfig::kMaxSamplesPerArchiveMetricChunkDefault - 2; i++) {
            st = c.addSample(BSON("name" << "joe"
                                         << "key1" << static_cast<long long int>(i * j) << "key2"
                                         << 45));
            ASSERT_HAS_SPACE(st);
        }

        st = c.addSample(BSON("name" << "joe"
                                     << "key1" << 34 << "key2" << 45));
        ASSERT_FULL(st);

        // Add Value
        st = c.addSample(BSON("name" << "joe"
                                     << "key1" << 34 << "key2" << 45));
        ASSERT_HAS_SPACE(st);
    }
}

template <typename T>
BSONObj generateSample(std::random_device& rd, T generator, size_t count) {
    BSONObjBuilder builder;

    for (size_t i = 0; i < count; ++i) {
        builder.append("key", generator(rd));
    }

    return builder.obj();
}

// Test many metrics
TEST_F(FTDCCompressorTest, TestManyMetrics) {
    std::random_device rd;
    std::mt19937 gen(rd());

    std::uniform_int_distribution<long long> genValues(1, std::numeric_limits<long long>::max());
    const size_t metrics = 1000;

    // Test a large numbers of zeros, and incremental numbers in a full buffer
    for (int j = 0; j < 2; j++) {
        TestTie c;

        auto st = c.addSample(generateSample(rd, genValues, metrics));
        ASSERT_HAS_SPACE(st);

        for (size_t i = 0; i != FTDCConfig::kMaxSamplesPerArchiveMetricChunkDefault - 2; i++) {
            st = c.addSample(generateSample(rd, genValues, metrics));
            ASSERT_HAS_SPACE(st);
        }

        st = c.addSample(generateSample(rd, genValues, metrics));
        ASSERT_FULL(st);

        // Add Value
        st = c.addSample(generateSample(rd, genValues, metrics));
        ASSERT_HAS_SPACE(st);
    }
}

// Test various non-finite double values
TEST_F(FTDCCompressorTest, TestDoubleValues) {
    TestTie c;

    auto st = c.addSample(BSON("d" << 0.0));
    ASSERT_HAS_SPACE(st);
    st = c.addSample(BSON("d" << -42.0));
    ASSERT_HAS_SPACE(st);
    st = c.addSample(BSON("d" << 42.0));
    ASSERT_HAS_SPACE(st);
    st = c.addSample(BSON("d" << std::numeric_limits<double>::max()));
    ASSERT_HAS_SPACE(st);
    st = c.addSample(BSON("d" << std::numeric_limits<double>::min()));
    ASSERT_HAS_SPACE(st);
    st = c.addSample(BSON("d" << std::numeric_limits<double>::lowest()));
    ASSERT_HAS_SPACE(st);
    st = c.addSample(BSON("d" << std::numeric_limits<double>::infinity()));
    ASSERT_HAS_SPACE(st);
    st = c.addSample(BSON("d" << -std::numeric_limits<double>::infinity()));
    ASSERT_HAS_SPACE(st);
    st = c.addSample(BSON("d" << std::numeric_limits<double>::quiet_NaN()));
    ASSERT_HAS_SPACE(st);

    c.setExpectedDocuments({
        BSON("d" << 0.0),
        BSON("d" << -42.0),
        BSON("d" << 42.0),
        BSON("d" << std::numeric_limits<long long>::max()),
        BSON("d" << 0),
        BSON("d" << std::numeric_limits<long long>::min()),
        BSON("d" << std::numeric_limits<long long>::max()),
        BSON("d" << std::numeric_limits<long long>::min()),
        BSON("d" << 0),
    });
}

}  // namespace mongo
