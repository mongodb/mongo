/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/collection_record_store_options.h"

#include "mongo/db/local_catalog/clustered_collection_util.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
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
    const auto actualRSOptions = getRecordStoreOptions(kBasicNss, collOptions);

    // Default 'RecordStore::Options' should match those generated for the default
    // 'CollectionOptions'.
    const RecordStore::Options kDefaultRSOptions{};
    assertEQ(kDefaultRSOptions, actualRSOptions);
}

TEST_F(CollectionRecordStoreOptionsTest, ClusteredRecordStoreOptionsBasic) {
    CollectionOptions collOptions;
    collOptions.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
    const auto actualRSOptions = getRecordStoreOptions(kBasicNss, collOptions);
    RecordStore::Options expectedRSOptions{.keyFormat = KeyFormat::String, .allowOverwrite = false};
    assertEQ(expectedRSOptions, actualRSOptions);
}

TEST_F(CollectionRecordStoreOptionsTest, CappedRecordStoreOptionsBasic) {
    CollectionOptions collOptions;
    collOptions.capped = true;
    const auto actualRSOptions = getRecordStoreOptions(kBasicNss, collOptions);
    RecordStore::Options expectedRSOptions{.isCapped = true};
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
    const auto actualRSOptions = getRecordStoreOptions(kBasicNss, collOptions);
    RecordStore::Options expectedRSOptions{.isCapped = true};
    assertEQ(expectedRSOptions, actualRSOptions);
}

TEST_F(CollectionRecordStoreOptionsTest, CappedClusteredRecordStoreOptions) {
    CollectionOptions collOptions;
    collOptions.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
    collOptions.capped = true;
    const auto actualRSOptions = getRecordStoreOptions(kBasicNss, collOptions);
    RecordStore::Options expectedRSOptions{
        .keyFormat = KeyFormat::String, .isCapped = true, .allowOverwrite = false};
    assertEQ(expectedRSOptions, actualRSOptions);
}

TEST_F(CollectionRecordStoreOptionsTest, OplogRecordStoreOptions) {
    CollectionOptions collOptions;
    collOptions.capped = true;
    collOptions.cappedSize = 100;
    const auto actualRSOptions =
        getRecordStoreOptions(NamespaceString::kRsOplogNamespace, collOptions);
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
    const auto actualRSOptions = getRecordStoreOptions(kBasicNss, collOptions);
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
    const auto actualRSOptions = getRecordStoreOptions(timeseriesNss, collOptions);
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
    const auto actualRSOptions = getRecordStoreOptions(kBasicNss, collOptions);
    RecordStore::Options expectedRSOptions{.storageEngineCollectionOptions =
                                               collOptions.storageEngine};
    assertEQ(expectedRSOptions, actualRSOptions);
}

}  // namespace
}  // namespace mongo
