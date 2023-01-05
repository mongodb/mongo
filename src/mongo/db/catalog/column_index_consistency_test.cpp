/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/column_index_consistency.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

const NamespaceString kNss = NamespaceString("test.t");

class ColumnIndexConsistencyTest : public CatalogTestFixture {
protected:
    ColumnIndexConsistencyTest(Options options = {}) : CatalogTestFixture(std::move(options)) {}

    ColumnIndexConsistency createColumnStoreConsistency(
        const size_t numHashBuckets = IndexConsistency::kNumHashBuckets) {
        return ColumnIndexConsistency(operationContext(), _validateState.get(), numHashBuckets);
    }

    void setUp() override {
        CatalogTestFixture::setUp();

        auto opCtx = operationContext();

        // Create collection kNss for unit tests to use. It will possess a default _id index.
        const CollectionOptions defaultCollectionOptions;
        ASSERT_OK(storageInterface()->createCollection(opCtx, kNss, defaultCollectionOptions));

        _validateState = std::make_unique<CollectionValidation::ValidateState>(
            opCtx,
            kNss,
            CollectionValidation::ValidateMode::kForeground,
            CollectionValidation::RepairMode::kNone);
    };

    void tearDown() override {
        _validateState.reset();
        CatalogTestFixture::tearDown();
    }

    std::unique_ptr<CollectionValidation::ValidateState> _validateState;
};

const std::vector<FullCellView> kEntries{
    {"path1"_sd, 1, "value1"_sd},
    {"path1"_sd, 2, "value1"_sd},
    {"path1"_sd, 3, "value1"_sd},

    {"path1"_sd, 4, "value1"_sd},
    {"path2"_sd, 4, "value1"_sd},
    {"path3"_sd, 4, "value1"_sd},

    {"path4"_sd, 5, "value1"_sd},
    {"path4"_sd, 5, "value2"_sd},
    {"path4"_sd, 5, "value3"_sd},

    {"path4"_sd, 0x1000'0000'0000'0001, "value3"_sd},
    {"path4"_sd, 0x2000'0000'0000'0001, "value3"_sd},
    {"path4"_sd, 0x3000'0000'0000'0001, "value3"_sd},

    {"path44"_sd, 5, "value3"_sd},
    {"path4"_sd, 5, "value33"_sd},

    {"path1"_sd, 1, "1value1"_sd},
    {"path1"_sd, 2, "1value1"_sd},
    {"path1"_sd, 3, "1value1"_sd},

    {"path1"_sd, 4, "1value1"_sd},
    {"path2"_sd, 4, "1value1"_sd},
    {"path3"_sd, 4, "1value1"_sd},

    {"path4"_sd, 5, "1value1"_sd},
    {"path4"_sd, 5, "1value2"_sd},
    {"path4"_sd, 5, "1value3"_sd},

    {"path4"_sd, 0x1000'0000'0000'0001, "value32"_sd},
    {"path4"_sd, 0x2000'0000'0000'0001, "value33"_sd},
    {"path4"_sd, 0x3000'0000'0000'0001, "value34"_sd},

    {"path44"_sd, 5, "4value3"_sd},
    {"path4"_sd, 5, "4value33"_sd},

    // A couple duplicates
    {"path44"_sd, 5, "4value3"_sd},
    {"path4"_sd, 5, "4value33"_sd},

    {"path.some.path"_sd, 5, "a dog likes to chew"_sd},
    {"path.some.path"_sd, 6, "a dog likes to bite"_sd},
    {"path.some.path"_sd, 7, "a dog likes to sleep"_sd},
    {"path.some.path"_sd, 8, "a dog likes to eat"_sd},
    {"path.some.path"_sd, 9, "a dog likes to walk"_sd},
    {"path.some.path"_sd, 10, "a dog likes to play"_sd},
    {"path.some.path"_sd, 11, "a dog likes to drool"_sd},
    {"path.some.path"_sd, 12, "a dog likes to sniff"_sd},
    {"path.some.path"_sd, 13, "a dog likes to snuggle"_sd},
    {"path.some.path"_sd, 14, "a dog likes to go to the park with other dogs"_sd},

    {"path.some.otherpath"_sd, 1, "life is understood backwards but lived forwards"_sd},
    {"path.some.otherpath"_sd, 2, "the key is not spending time but investing it"_sd},
    {"path.some.otherpath"_sd, 3, "if we take care of the moments, the years"_sd},
    {"path.some.otherpath"_sd, 4, "time is an illusion"_sd},
    {"path.some.otherpath"_sd, 5, "life and times"_sd},
    {"path.some.otherpath"_sd, 667, "how many more times"_sd},

    {"still.some.otherpath"_sd, 667, "islands in the stream"_sd},
    {"still.some.otherpath"_sd, 668, "the gambler"_sd},
    {"still.some.otherpath"_sd, 669, "coward of the county"_sd},
    {"still.some.otherpath"_sd, 662, "lady"_sd},
    {"still.some.otherpath"_sd, 661, "we've got tonight"_sd},
    {"still.some.otherpath"_sd, 660, "through the years"_sd},
};

TEST_F(ColumnIndexConsistencyTest, VerifyHashFunction) {
    stdx::unordered_set<int64_t> hashes;
    for (const auto& v : kEntries) {
        hashes.insert(ColumnIndexConsistency::_hash(v));
    }
    ASSERT_EQ(hashes.size(), 50u);
}

TEST_F(ColumnIndexConsistencyTest, VerifyEntriesEqual) {
    ColumnIndexConsistency cic = createColumnStoreConsistency();

    for (const auto& v : kEntries) {
        cic.addDocEntry(v);
    }
    for (const auto& v : kEntries) {
        cic.addIndexEntry(v);
    }
    ASSERT_FALSE(cic.haveEntryMismatch());
    ASSERT_EQ(cic.getNumDocs(), cic.getTotalIndexKeys());
}

TEST_F(ColumnIndexConsistencyTest, VerifyEntriesNotEqual) {
    ColumnIndexConsistency cic = createColumnStoreConsistency();

    for (const auto& v : kEntries) {
        cic.addDocEntry(v);
    }
    cic.addDocEntry({"path444"_sd, 4, "value1"_sd});
    for (const auto& v : kEntries) {
        cic.addIndexEntry(v);
    }
    cic.addIndexEntry({"path444"_sd, 4, "value10"_sd});
    ASSERT_TRUE(cic.haveEntryMismatch());
    ASSERT_EQ(cic.getNumDocs(), cic.getTotalIndexKeys());
}

TEST_F(ColumnIndexConsistencyTest, VerifyUnorderedEntriesEqual) {
    ColumnIndexConsistency cic = createColumnStoreConsistency();

    for (int i = 0; i < 10000; i++) {
        cic.addDocEntry({"foo"_sd, i, "bar"_sd});
    }

    for (int i = 9999; i >= 0; i--) {
        cic.addIndexEntry({"foo"_sd, i, "bar"_sd});
    }
    ASSERT_EQ(cic.getNumDocs(), 10000u);
    ASSERT_EQ(cic.getNumDocs(), cic.getTotalIndexKeys());
    ASSERT_FALSE(cic.haveEntryMismatch());

    cic.addDocEntry({"foo"_sd, 5311, "bar"_sd});
    ASSERT_NE(cic.getNumDocs(), cic.getTotalIndexKeys());
    ASSERT_TRUE(cic.haveEntryMismatch());

    cic.addIndexEntry({"foo"_sd, 5311, "bar"_sd});
    ASSERT_EQ(cic.getNumDocs(), cic.getTotalIndexKeys());
    ASSERT_FALSE(cic.haveEntryMismatch());

    for (const auto& v : kEntries) {
        cic.addDocEntry(v);
    }
    for (auto it = kEntries.crbegin(); it != kEntries.crend(); it++) {
        cic.addIndexEntry(*it);
    }
    ASSERT_FALSE(cic.haveEntryMismatch());
    ASSERT_EQ(cic.getNumDocs(), cic.getTotalIndexKeys());
}

TEST_F(ColumnIndexConsistencyTest, VerifyEvenBucketDistribution) {
    const size_t numBuckets = 1000;
    ColumnIndexConsistency cic = createColumnStoreConsistency(numBuckets);

    for (int i = 0; i < 10000; i++) {
        cic.addDocEntry({"foo"_sd, i, "bar"_sd});
    }

    uint32_t maxValue = 0;
    for (size_t i = 0; i < numBuckets; i++) {
        maxValue = std::max(cic.getBucketCount(i), maxValue);
    }
    ASSERT_LT(maxValue, uint32_t(40));  // perfect would be 20, so 2x that is our made-up threshold
}

TEST_F(ColumnIndexConsistencyTest, VerifyEvenBucketDistributionFewerInputs) {
    // fewer, more varied inputs
    const size_t numBuckets = 5;
    ColumnIndexConsistency cic = createColumnStoreConsistency(numBuckets);
    for (const auto& v : kEntries) {
        cic.addDocEntry(v);
    }
    uint32_t maxValue = 0;
    for (size_t i = 0; i < numBuckets; i++) {
        maxValue = std::max(cic.getBucketCount(i), maxValue);
    }
    ASSERT_LT(maxValue, uint32_t(40));  // perfect would be 20, 2x that is our made-up threshold
}

TEST_F(ColumnIndexConsistencyTest, VerifySizeCounters) {
    const size_t numBuckets = 5;
    ColumnIndexConsistency cic = createColumnStoreConsistency(numBuckets);

    cic.addDocEntry({""_sd, 1, "b"_sd});   // 9 bytes
    cic.addDocEntry({"f"_sd, 0, ""_sd});   // 9 bytes
    cic.addDocEntry({"f"_sd, 1, "b"_sd});  // 10 bytes

    cic.addIndexEntry({""_sd, 1, "b"_sd});
    cic.addIndexEntry({"f"_sd, 0, ""_sd});
    cic.addIndexEntry({"f"_sd, 1, "b"_sd});

    size_t totalSize = 0;
    for (size_t i = 0; i < numBuckets; i++) {
        totalSize += cic.getBucketSizeBytes(i);
    }
    ASSERT_EQ(totalSize, 112u);  // 28 * 2 (hashed twice) * 2 (hash doc + index) = 112
}

}  // namespace
}  // namespace mongo
