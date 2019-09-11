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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection_options.h"

#include <limits>

#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
using unittest::assertGet;

void checkRoundTrip(const CollectionOptions& options) {
    CollectionOptions parsedOptions = assertGet(CollectionOptions::parse(options.toBSON()));
    ASSERT_BSONOBJ_EQ(options.toBSON(), parsedOptions.toBSON());
}

TEST(CollectionOptions, SimpleRoundTrip) {
    CollectionOptions options;
    checkRoundTrip(options);

    options.capped = true;
    options.cappedSize = 10240;
    options.cappedMaxDocs = 1111;
    checkRoundTrip(options);

    options.setNoIdIndex();
    checkRoundTrip(options);
}

TEST(CollectionOptions, Validate) {
    CollectionOptions options;
    ASSERT_OK(options.validateForStorage());

    options.storageEngine = fromjson("{storageEngine1: 1}");
    ASSERT_NOT_OK(options.validateForStorage());
}

TEST(CollectionOptions, Validator) {

    ASSERT_NOT_OK(CollectionOptions::parse(fromjson("{validator: 'notAnObject'}")).getStatus());
    CollectionOptions options =
        assertGet(CollectionOptions::parse(fromjson("{validator: {a: 1}}")));
    ASSERT_BSONOBJ_EQ(options.validator, fromjson("{a: 1}"));

    options.validator = fromjson("{b: 1}");
    ASSERT_BSONOBJ_EQ(options.toBSON()["validator"].Obj(), fromjson("{b: 1}"));

    CollectionOptions defaultOptions;
    ASSERT_BSONOBJ_EQ(defaultOptions.validator, BSONObj());
    ASSERT(!defaultOptions.toBSON()["validator"]);
}

TEST(CollectionOptions, ErrorBadSize) {
    ASSERT_NOT_OK(CollectionOptions::parse(fromjson("{capped: true, size: -1}")).getStatus());
    ASSERT_NOT_OK(CollectionOptions::parse(fromjson("{capped: false, size: -1}")).getStatus());
    ASSERT_NOT_OK(CollectionOptions::parse(
                      BSON("capped" << true << "size" << std::numeric_limits<long long>::min()))
                      .getStatus());
    ASSERT_NOT_OK(
        CollectionOptions::parse(BSON("capped" << true << "size" << (1LL << 62))).getStatus());
    ASSERT_NOT_OK(CollectionOptions::parse(
                      BSON("capped" << true << "size" << std::numeric_limits<long long>::max()))
                      .getStatus());
}

TEST(CollectionOptions, ErrorBadMax) {
    ASSERT_NOT_OK(
        CollectionOptions::parse(BSON("capped" << true << "max" << (1LL << 31))).getStatus());
}

TEST(CollectionOptions, CappedSizeRoundsUpForAlignment) {
    const long long kUnalignedCappedSize = 1000;
    const long long kAlignedCappedSize = 1024;
    // Check size rounds up to multiple of alignment.
    auto options = assertGet(
        CollectionOptions::parse((BSON("capped" << true << "size" << kUnalignedCappedSize))));

    ASSERT_EQUALS(options.capped, true);
    ASSERT_EQUALS(options.cappedSize, kAlignedCappedSize);
    ASSERT_EQUALS(options.cappedMaxDocs, 0);
}

TEST(CollectionOptions, IgnoreSizeWrongType) {
    auto options =
        assertGet(CollectionOptions::parse(fromjson("{size: undefined, capped: undefined}")));
    ASSERT_EQUALS(options.capped, false);
    ASSERT_EQUALS(options.cappedSize, 0);
}

TEST(CollectionOptions, IgnoreMaxWrongType) {
    auto options =
        assertGet(CollectionOptions::parse(fromjson("{capped: true, size: 1024, max: ''}")));
    ASSERT_EQUALS(options.capped, true);
    ASSERT_EQUALS(options.cappedSize, 1024);
    ASSERT_EQUALS(options.cappedMaxDocs, 0);
}

TEST(CollectionOptions, InvalidStorageEngineField) {
    // "storageEngine" field has to be an object if present.
    ASSERT_NOT_OK(CollectionOptions::parse(fromjson("{storageEngine: 1}")).getStatus());

    // Every field under "storageEngine" has to be an object.
    ASSERT_NOT_OK(
        CollectionOptions::parse(fromjson("{storageEngine: {storageEngine1: 1}}")).getStatus());

    // Empty "storageEngine" not allowed
    ASSERT_OK(CollectionOptions::parse(fromjson("{storageEngine: {}}")).getStatus());
}

TEST(CollectionOptions, ParseEngineField) {
    auto opts = assertGet(CollectionOptions::parse(
        fromjson("{storageEngine: {storageEngine1: {x: 1, y: 2}, storageEngine2: {a: 1, b:2}}}")));
    checkRoundTrip(opts);

    BSONObj obj = opts.toBSON();

    // Check "storageEngine" field.
    ASSERT_TRUE(obj.hasField("storageEngine"));
    ASSERT_TRUE(obj.getField("storageEngine").isABSONObj());
    BSONObj storageEngine = obj.getObjectField("storageEngine");

    // Check individual storage storageEngine fields.
    ASSERT_TRUE(storageEngine.getField("storageEngine1").isABSONObj());
    BSONObj storageEngine1 = storageEngine.getObjectField("storageEngine1");
    ASSERT_EQUALS(1, storageEngine1.getIntField("x"));
    ASSERT_EQUALS(2, storageEngine1.getIntField("y"));

    ASSERT_TRUE(storageEngine.getField("storageEngine2").isABSONObj());
    BSONObj storageEngine2 = storageEngine.getObjectField("storageEngine2");
    ASSERT_EQUALS(1, storageEngine2.getIntField("a"));
    ASSERT_EQUALS(2, storageEngine2.getIntField("b"));
}

TEST(CollectionOptions, ResetStorageEngineField) {

    auto opts =
        assertGet(CollectionOptions::parse(fromjson("{storageEngine: {storageEngine1: {x: 1}}}")));
    checkRoundTrip(opts);

    CollectionOptions defaultOpts;
    ASSERT_TRUE(defaultOpts.storageEngine.isEmpty());
}

TEST(CollectionOptions, ModifyStorageEngineField) {
    CollectionOptions opts;

    // Directly modify storageEngine field in collection options.
    opts.storageEngine = fromjson("{storageEngine1: {x: 1}}");

    // Unrecognized field should not be present in BSON representation.
    BSONObj obj = opts.toBSON();
    ASSERT_FALSE(obj.hasField("unknownField"));

    // Check "storageEngine" field.
    ASSERT_TRUE(obj.hasField("storageEngine"));
    ASSERT_TRUE(obj.getField("storageEngine").isABSONObj());
    BSONObj storageEngine = obj.getObjectField("storageEngine");

    // Check individual storage storageEngine fields.
    ASSERT_TRUE(storageEngine.getField("storageEngine1").isABSONObj());
    BSONObj storageEngine1 = storageEngine.getObjectField("storageEngine1");
    ASSERT_EQUALS(1, storageEngine1.getIntField("x"));
}

TEST(CollectionOptions, FailToParseCollationThatIsNotAnObject) {
    ASSERT_NOT_OK(CollectionOptions::parse(fromjson("{collation: 'notAnObject'}")).getStatus());
}

TEST(CollectionOptions, FailToParseCollationThatIsAnEmptyObject) {
    ASSERT_NOT_OK(CollectionOptions::parse(fromjson("{collation: {}}")).getStatus());
}

TEST(CollectionOptions, CollationFieldParsesCorrectly) {
    auto options = assertGet(CollectionOptions::parse(fromjson("{collation: {locale: 'en'}}")));
    ASSERT_BSONOBJ_EQ(options.collation, fromjson("{locale: 'en'}"));
    ASSERT_OK(options.validateForStorage());
}

TEST(CollectionOptions, ParsedCollationObjShouldBeOwned) {
    auto options = assertGet(CollectionOptions::parse(fromjson("{collation: {locale: 'en'}}")));
    ASSERT_BSONOBJ_EQ(options.collation, fromjson("{locale: 'en'}"));
    ASSERT_TRUE(options.collation.isOwned());
}

TEST(CollectionOptions, ResetClearsCollationField) {
    CollectionOptions options;
    ASSERT_TRUE(options.collation.isEmpty());
    options = assertGet(CollectionOptions::parse(fromjson("{collation: {locale: 'en'}}")));
    ASSERT_FALSE(options.collation.isEmpty());
}

TEST(CollectionOptions, CollationFieldLeftEmptyWhenOmitted) {
    auto options = assertGet(CollectionOptions::parse(fromjson("{validator: {a: 1}}")));
    ASSERT_TRUE(options.collation.isEmpty());
}

TEST(CollectionOptions, CollationFieldNotDumpedToBSONWhenOmitted) {
    auto options = assertGet(CollectionOptions::parse(fromjson("{validator: {a: 1}}")));
    ASSERT_TRUE(options.collation.isEmpty());
    BSONObj asBSON = options.toBSON();
    ASSERT_FALSE(asBSON["collation"]);
}

TEST(CollectionOptions, ViewParsesCorrectly) {
    auto options =
        assertGet(CollectionOptions::parse(fromjson("{viewOn: 'c', pipeline: [{$match: {}}]}")));
    ASSERT_EQ(options.viewOn, "c");
    ASSERT_BSONOBJ_EQ(options.pipeline, fromjson("[{$match: {}}]"));
}

TEST(CollectionOptions, ViewParsesCorrectlyWithoutPipeline) {
    auto options = assertGet(CollectionOptions::parse(fromjson("{viewOn: 'c'}")));
    ASSERT_EQ(options.viewOn, "c");
    ASSERT_BSONOBJ_EQ(options.pipeline, BSONObj());
}

TEST(CollectionOptions, PipelineFieldRequiresViewOn) {
    ASSERT_NOT_OK(CollectionOptions::parse(fromjson("{pipeline: [{$match: {}}]}")).getStatus());
}

TEST(CollectionOptions, UnknownTopLevelOptionFailsToParse) {
    auto statusWith = CollectionOptions::parse(fromjson("{invalidOption: 1}"));
    ASSERT_EQ(statusWith.getStatus().code(), ErrorCodes::InvalidOptions);
}

TEST(CollectionOptions, CreateOptionIgnoredIfFirst) {
    ASSERT_OK(CollectionOptions::parse(fromjson("{create: 1}")).getStatus());
}

TEST(CollectionOptions, CreateOptionIgnoredIfNotFirst) {
    auto options =
        assertGet(CollectionOptions::parse(fromjson("{capped: true, create: 1, size: 1024}")));
    ASSERT_EQ(options.capped, true);
    ASSERT_EQ(options.cappedSize, 1024L);
}

TEST(CollectionOptions, UnknownOptionIgnoredIfCreateOptionFirst) {
    ASSERT_OK(CollectionOptions::parse(fromjson("{create: 1, invalidOption: 1}")).getStatus());
}

TEST(CollectionOptions, UnknownOptionIgnoredIfCreateOptionPresent) {
    ASSERT_OK(CollectionOptions::parse(fromjson("{invalidOption: 1, create: 1}")).getStatus());
}

TEST(CollectionOptions, UnknownOptionRejectedIfCreateOptionNotPresent) {
    auto statusWith = CollectionOptions::parse(fromjson("{invalidOption: 1}"));
    ASSERT_EQ(statusWith.getStatus().code(), ErrorCodes::InvalidOptions);
}

TEST(CollectionOptions, DuplicateCreateOptionIgnoredIfCreateOptionFirst) {
    auto statusWith = CollectionOptions::parse(BSON("create" << 1 << "create" << 1));
    ASSERT_OK(statusWith.getStatus());
}

TEST(CollectionOptions, DuplicateCreateOptionIgnoredIfCreateOptionNotFirst) {
    auto statusWith = CollectionOptions::parse(
        BSON("capped" << true << "create" << 1 << "create" << 1 << "size" << 1024));
    ASSERT_OK(statusWith.getStatus());
}

TEST(CollectionOptions, MaxTimeMSWhitelistedOptionIgnored) {
    auto statusWith = CollectionOptions::parse(fromjson("{maxTimeMS: 1}"));
    ASSERT_OK(statusWith.getStatus());
}

TEST(CollectionOptions, WriteConcernWhitelistedOptionIgnored) {
    auto statusWith = CollectionOptions::parse(fromjson("{writeConcern: 1}"));
    ASSERT_OK(statusWith.getStatus());
}

TEST(CollectionOptions, ParseUUID) {
    CollectionOptions options;
    CollectionUUID uuid = CollectionUUID::gen();

    // Check required parse failures
    ASSERT_FALSE(options.uuid);
    ASSERT_NOT_OK(CollectionOptions::parse(uuid.toBSON()).getStatus());
    ASSERT_NOT_OK(CollectionOptions::parse(BSON("uuid" << 1)).getStatus());
    ASSERT_NOT_OK(CollectionOptions::parse(BSON("uuid" << 1), CollectionOptions::parseForStorage)
                      .getStatus());

    // Check successful parse and roundtrip.
    options =
        assertGet(CollectionOptions::parse(uuid.toBSON(), CollectionOptions::parseForStorage));
    ASSERT(options.uuid.get() == uuid);

    // Check that a collection options containing a UUID passes validation.
    ASSERT_OK(options.validateForStorage());
}

TEST(CollectionOptions, SizeNumberLimits) {
    CollectionOptions options = assertGet(CollectionOptions::parse(fromjson("{size: 'a'}")));
    ASSERT_EQ(options.cappedSize, 0);

    options = assertGet(CollectionOptions::parse(fromjson("{size: '-1'}")));
    ASSERT_EQ(options.cappedSize, 0);

    options =
        assertGet(CollectionOptions::parse(fromjson("{size: '-9999999999999999999999999999999'}")));
    ASSERT_EQ(options.cappedSize, 0);

    // The test for size is redundant since size returns a status that's not ok if it's larger
    // than a petabyte, which is smaller than LLONG_MAX anyways. We test that here.
    ASSERT_NOT_OK(CollectionOptions::parse(fromjson("{size: 9999999999999999}")).getStatus());
}

TEST(CollectionOptions, MaxNumberLimits) {
    CollectionOptions options = assertGet(CollectionOptions::parse(fromjson("{max: 'a'}")));
    ASSERT_EQ(options.cappedMaxDocs, 0);

    options = assertGet(CollectionOptions::parse(fromjson("{max: '-1'}")));
    ASSERT_EQ(options.cappedMaxDocs, 0);

    options = assertGet(
        CollectionOptions::parse(fromjson("{max: '-9999999999999999999999999999999999'}")));
    ASSERT_EQ(options.cappedMaxDocs, 0);

    options = assertGet(CollectionOptions::parse(fromjson("{max: 99999999999999999999999999999}")));
    ASSERT_EQ(options.cappedMaxDocs, 0);
}

TEST(CollectionOptions, NExtentsNoError) {
    // Check that $nExtents does not cause an error for backwards compatability
    assertGet(CollectionOptions::parse(fromjson("{$nExtents: 'a'}")));
}
}  // namespace mongo
