// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/collection_record_store_options.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

BSONObj convertToBSON(const RecordStore::Options& opts) {
    BSONObjBuilder bob;
    bob.append("keyFormat", opts.keyFormat);
    bob.append("isCapped", opts.isCapped);
    bob.append("isOplog", opts.isOplog);
    bob.append("oplogMaxSize", opts.oplogMaxSize);
    bob.append("allowOverwrite", opts.allowOverwrite);
    bob.append("forceUpdateWithFullDocument", opts.forceUpdateWithFullDocument);
    bob.append("customBlockCompressor", opts.customBlockCompressor.value_or("none"));
    bob.append("storageEngineCollectionOptions", opts.storageEngineCollectionOptions);
    return bob.obj();
}

std::string errMsgDetails(const RecordStore::Options& expectedOpts,
                          const RecordStore::Options& actualOpts) {
    return str::stream() << "Expected 'RecordStore::Options': " << convertToBSON(expectedOpts)
                         << ", actual 'RecordStore::Options': " << convertToBSON(actualOpts);
}
class CollectionRecordStoreOptionsTest : public unittest::Test {
protected:
    const NamespaceString kBasicNss =
        NamespaceString::createNamespaceString_forTest("testdb", "testcol");

    // Asserts each field in 'actualOpts' matches that of 'expectedOpts'.
    void assertEQ(const RecordStore::Options& expectedOpts,
                  const RecordStore::Options& actualOpts) {
        ASSERT_EQ(expectedOpts.keyFormat, actualOpts.keyFormat)
            << errMsgDetails(expectedOpts, actualOpts);
        ASSERT_EQ(expectedOpts.isCapped, actualOpts.isCapped)
            << errMsgDetails(expectedOpts, actualOpts);
        ASSERT_EQ(expectedOpts.isOplog, actualOpts.isOplog)
            << errMsgDetails(expectedOpts, actualOpts);
        ASSERT_EQ(expectedOpts.oplogMaxSize, actualOpts.oplogMaxSize)
            << errMsgDetails(expectedOpts, actualOpts);
        ASSERT_EQ(expectedOpts.allowOverwrite, actualOpts.allowOverwrite)
            << errMsgDetails(expectedOpts, actualOpts);
        ASSERT_EQ(expectedOpts.forceUpdateWithFullDocument, actualOpts.forceUpdateWithFullDocument)
            << errMsgDetails(expectedOpts, actualOpts);
        ASSERT_EQ(expectedOpts.customBlockCompressor, actualOpts.customBlockCompressor)
            << errMsgDetails(expectedOpts, actualOpts);
        ASSERT_EQ(0,
                  expectedOpts.storageEngineCollectionOptions.woCompare(
                      actualOpts.storageEngineCollectionOptions))
            << errMsgDetails(expectedOpts, actualOpts);
    }
};

TEST_F(CollectionRecordStoreOptionsTest, DefaultRecordStoreOptions) {
    CollectionOptions collOptions;
    const auto actualRSOptions =
        getRecordStoreOptions(kBasicNss, collOptions, /*recordIdsReplicated=*/false);

    // Default 'RecordStore::Options' should match those generated for the default
    // 'CollectionOptions'.
    const RecordStore::Options kDefaultRSOptions{};
    assertEQ(kDefaultRSOptions, actualRSOptions);
}

TEST_F(CollectionRecordStoreOptionsTest, DefaultRecordStoreOptionsWithRecordIdsReplicated) {
    CollectionOptions collOptions;
    const auto actualRSOptions =
        getRecordStoreOptions(kBasicNss, collOptions, /*recordIdsReplicated=*/true);

    // Overwrites are disallowed for collection with replicated record Ids
    const RecordStore::Options kDefaultRSOptions{.allowOverwrite = false};
    assertEQ(kDefaultRSOptions, actualRSOptions);
}

TEST_F(CollectionRecordStoreOptionsTest, ClusteredRecordStoreOptionsBasic) {
    CollectionOptions collOptions;
    collOptions.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
    const auto actualRSOptions =
        getRecordStoreOptions(kBasicNss, collOptions, /*recordIdsReplicated=*/false);
    RecordStore::Options expectedRSOptions{.keyFormat = KeyFormat::String, .allowOverwrite = false};
    assertEQ(expectedRSOptions, actualRSOptions);
}

TEST_F(CollectionRecordStoreOptionsTest, CappedRecordStoreOptionsBasic) {
    CollectionOptions collOptions;
    collOptions.capped = true;
    const auto actualRSOptions =
        getRecordStoreOptions(kBasicNss, collOptions, /*recordIdsReplicated=*/false);
    RecordStore::Options expectedRSOptions{.isCapped = true};
    assertEQ(expectedRSOptions, actualRSOptions);
}

TEST_F(CollectionRecordStoreOptionsTest, CappedRecordStoreOptionsBasicWithRecordIdsReplicated) {
    CollectionOptions collOptions;
    collOptions.capped = true;
    const auto actualRSOptions =
        getRecordStoreOptions(kBasicNss, collOptions, /*recordIdsReplicated=*/true);

    RecordStore::Options expectedRSOptions{.isCapped = true, .allowOverwrite = false};
    assertEQ(expectedRSOptions, actualRSOptions);
}

TEST_F(CollectionRecordStoreOptionsTest, CappedRecordStoreOptions) {
    CollectionOptions collOptions;
    // Aside from 'capped', other capped related 'CollectionOptions' fields aren't relevant when
    // generating a RecordStore for a non-oplog collection. For example, 'cappedSize' and
    // 'cappedMaxDocs' don't impact the 'RecordStore::Options' generated for 'kBasicNss'.
    collOptions.capped = true;
    collOptions.cappedMaxDocs = 100 /* arbitrary */;
    collOptions.cappedSize = 100 /* arbitrary */;
    const auto actualRSOptions =
        getRecordStoreOptions(kBasicNss, collOptions, /*recordIdsReplicated=*/false);
    RecordStore::Options expectedRSOptions{.isCapped = true};
    assertEQ(expectedRSOptions, actualRSOptions);
}

TEST_F(CollectionRecordStoreOptionsTest, CappedRecordStoreOptionsWithRecordIdsReplicated) {
    CollectionOptions collOptions;
    // Aside from 'capped', other capped related 'CollectionOptions' fields aren't relevant when
    // generating a RecordStore for a non-oplog collection. For example, 'cappedSize' and
    // 'cappedMaxDocs' don't impact the 'RecordStore::Options' generated for 'kBasicNss'.
    collOptions.capped = true;
    collOptions.cappedMaxDocs = 100 /* arbitrary */;
    collOptions.cappedSize = 100 /* arbitrary */;
    const auto actualRSOptions =
        getRecordStoreOptions(kBasicNss, collOptions, /*recordIdsReplicated=*/true);
    RecordStore::Options expectedRSOptions{.isCapped = true, .allowOverwrite = false};
    assertEQ(expectedRSOptions, actualRSOptions);
}

TEST_F(CollectionRecordStoreOptionsTest, CappedClusteredRecordStoreOptions) {
    CollectionOptions collOptions;
    collOptions.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
    collOptions.capped = true;
    const auto actualRSOptions =
        getRecordStoreOptions(kBasicNss, collOptions, /*recordIdsReplicated=*/false);
    RecordStore::Options expectedRSOptions{
        .keyFormat = KeyFormat::String, .isCapped = true, .allowOverwrite = false};
    assertEQ(expectedRSOptions, actualRSOptions);
}

TEST_F(CollectionRecordStoreOptionsTest, CappedClusteredRecordStoreOptionsWithRecordIdsReplicated) {
    CollectionOptions collOptions;
    collOptions.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
    collOptions.capped = true;
    const auto actualRSOptions =
        getRecordStoreOptions(kBasicNss, collOptions, /*recordIdsReplicated=*/true);
    RecordStore::Options expectedRSOptions{
        .keyFormat = KeyFormat::String, .isCapped = true, .allowOverwrite = false};
    assertEQ(expectedRSOptions, actualRSOptions);
}

TEST_F(CollectionRecordStoreOptionsTest, OplogRecordStoreOptions) {
    CollectionOptions collOptions;
    collOptions.capped = true;
    collOptions.cappedSize = 100;
    const auto actualRSOptions = getRecordStoreOptions(
        NamespaceString::kRsOplogNamespace, collOptions, /*recordIdsReplicated=*/false);
    RecordStore::Options expectedRSOptions{
        .isCapped = true, .isOplog = true, .oplogMaxSize = collOptions.cappedSize};
    assertEQ(expectedRSOptions, actualRSOptions);
}

/*
 * Tests fields generated exclusively when 'CollectionOptions::timeseries' is set.
 */
TEST_F(CollectionRecordStoreOptionsTest, TimeseriesRecordStoreOptionsNotClustered) {
    CollectionOptions collOptions;
    // In practice, a valid set of CollectionOptions must have both 'CollectionOptions::timeseries'
    // and 'CollectionOptions::clusteredIndex' set. 'getRecordStoreOptions()' does not perform
    // validation on the provided CollectionOptions.
    collOptions.timeseries = TimeseriesOptions(/*timeField=*/"t");
    const auto actualRSOptions =
        getRecordStoreOptions(kBasicNss, collOptions, /*recordIdsReplicated=*/false);
    RecordStore::Options expectedRSOptions{
        .forceUpdateWithFullDocument = true /* exclusive for timeseries */,
        .customBlockCompressor =
            std::string{kDefaultTimeseriesCollectionCompressor} /* exclusive for timeseries */};
    assertEQ(expectedRSOptions, actualRSOptions);
}

TEST_F(CollectionRecordStoreOptionsTest, TimeseriesRecordStoreOptions) {
    CollectionOptions collOptions;
    collOptions.timeseries = TimeseriesOptions(/*timeField=*/"t");
    collOptions.clusteredIndex = clustered_util::makeCanonicalClusteredInfoForLegacyFormat();
    NamespaceString timeseriesNss =
        NamespaceString::createNamespaceString_forTest("test.system.buckets.ts");
    const auto actualRSOptions =
        getRecordStoreOptions(timeseriesNss, collOptions, /*recordIdsReplicated=*/false);
    RecordStore::Options expectedRSOptions{
        .keyFormat = KeyFormat::String,
        .allowOverwrite = false,
        .forceUpdateWithFullDocument = true /* exclusive for timeseries */,
        .customBlockCompressor =
            std::string{kDefaultTimeseriesCollectionCompressor} /* exclusive for timeseries */};
    assertEQ(expectedRSOptions, actualRSOptions);
}

TEST_F(CollectionRecordStoreOptionsTest, TimeseriesRecordStoreOptionsWithRecordIdsReplicated) {
    CollectionOptions collOptions;
    collOptions.timeseries = TimeseriesOptions(/*timeField=*/"t");
    collOptions.clusteredIndex = clustered_util::makeCanonicalClusteredInfoForLegacyFormat();
    NamespaceString timeseriesNss =
        NamespaceString::createNamespaceString_forTest("test.system.buckets.ts");
    const auto actualRSOptions =
        getRecordStoreOptions(timeseriesNss, collOptions, /*recordIdsReplicated=*/true);
    RecordStore::Options expectedRSOptions{
        .keyFormat = KeyFormat::String,
        .allowOverwrite = false,
        .forceUpdateWithFullDocument = true /* exclusive for timeseries */,
        .customBlockCompressor =
            std::string{kDefaultTimeseriesCollectionCompressor} /* exclusive for timeseries */};
    assertEQ(expectedRSOptions, actualRSOptions);
}

TEST_F(CollectionRecordStoreOptionsTest, RecordStoreOptionsWithStorageEngineCollectionOptions) {
    CollectionOptions collOptions;
    collOptions.storageEngine =
        BSON("create" << kBasicNss.coll() << "storageEngine"
                      << BSON("wiredTiger" << BSON("configString" << "prefix_compression=true")));
    const auto actualRSOptions =
        getRecordStoreOptions(kBasicNss, collOptions, /*recordIdsReplicated=*/false);
    RecordStore::Options expectedRSOptions{.storageEngineCollectionOptions =
                                               collOptions.storageEngine};
    assertEQ(expectedRSOptions, actualRSOptions);
}

TEST_F(CollectionRecordStoreOptionsTest,
       RecordStoreOptionsWithStorageEngineCollectionOptionsWithRecordIdsReplicated) {
    CollectionOptions collOptions;
    collOptions.storageEngine =
        BSON("create" << kBasicNss.coll() << "storageEngine"
                      << BSON("wiredTiger" << BSON("configString" << "prefix_compression=true")));
    const auto actualRSOptions =
        getRecordStoreOptions(kBasicNss, collOptions, /*recordIdsReplicated=*/true);
    RecordStore::Options expectedRSOptions{
        .allowOverwrite = false, .storageEngineCollectionOptions = collOptions.storageEngine};
    assertEQ(expectedRSOptions, actualRSOptions);
}

}  // namespace
}  // namespace mongo
