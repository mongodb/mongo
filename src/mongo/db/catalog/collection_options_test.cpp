/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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

void checkRoundTrip(const CollectionOptions& options1) {
    CollectionOptions options2;
    options2.parse(options1.toBSON());
    ASSERT_BSONOBJ_EQ(options1.toBSON(), options2.toBSON());
}

TEST(CollectionOptions, SimpleRoundTrip) {
    CollectionOptions options;
    checkRoundTrip(options);

    options.capped = true;
    options.cappedSize = 10240;
    options.cappedMaxDocs = 1111;
    checkRoundTrip(options);

    options.setNoIdIndex();
    options.flags = 5;
    checkRoundTrip(options);
}

TEST(CollectionOptions, Validate) {
    CollectionOptions options;
    ASSERT_OK(options.validate());

    options.storageEngine = fromjson("{storageEngine1: 1}");
    ASSERT_NOT_OK(options.validate());
}

TEST(CollectionOptions, Validator) {
    CollectionOptions options;

    ASSERT_NOT_OK(options.parse(fromjson("{validator: 'notAnObject'}")));

    ASSERT_OK(options.parse(fromjson("{validator: {a: 1}}")));
    ASSERT_BSONOBJ_EQ(options.validator, fromjson("{a: 1}"));

    options.validator = fromjson("{b: 1}");
    ASSERT_BSONOBJ_EQ(options.toBSON()["validator"].Obj(), fromjson("{b: 1}"));

    CollectionOptions defaultOptions;
    ASSERT_BSONOBJ_EQ(defaultOptions.validator, BSONObj());
    ASSERT(!defaultOptions.toBSON()["validator"]);
}

TEST(CollectionOptions, ErrorBadSize) {
    ASSERT_NOT_OK(CollectionOptions().parse(fromjson("{capped: true, size: -1}")));
    ASSERT_NOT_OK(CollectionOptions().parse(fromjson("{capped: false, size: -1}")));
    ASSERT_NOT_OK(CollectionOptions().parse(
        BSON("capped" << true << "size" << std::numeric_limits<long long>::min())));
    ASSERT_NOT_OK(CollectionOptions().parse(BSON("capped" << true << "size" << (1LL << 62))));
    ASSERT_NOT_OK(CollectionOptions().parse(
        BSON("capped" << true << "size" << std::numeric_limits<long long>::max())));
}

TEST(CollectionOptions, ErrorBadMax) {
    ASSERT_NOT_OK(CollectionOptions().parse(BSON("capped" << true << "max" << (1LL << 31))));
}

TEST(CollectionOptions, CappedSizeRoundsUpForAlignment) {
    const long long kUnalignedCappedSize = 1000;
    const long long kAlignedCappedSize = 1024;
    CollectionOptions options;

    // Check size rounds up to multiple of alignment.
    ASSERT_OK(options.parse(BSON("capped" << true << "size" << kUnalignedCappedSize)));
    ASSERT_EQUALS(options.capped, true);
    ASSERT_EQUALS(options.cappedSize, kAlignedCappedSize);
    ASSERT_EQUALS(options.cappedMaxDocs, 0);
}

TEST(CollectionOptions, IgnoreSizeWrongType) {
    CollectionOptions options;
    ASSERT_OK(options.parse(fromjson("{size: undefined, capped: undefined}")));
    ASSERT_EQUALS(options.capped, false);
    ASSERT_EQUALS(options.cappedSize, 0);
}

TEST(CollectionOptions, IgnoreMaxWrongType) {
    CollectionOptions options;
    ASSERT_OK(options.parse(fromjson("{capped: true, size: 1024, max: ''}")));
    ASSERT_EQUALS(options.capped, true);
    ASSERT_EQUALS(options.cappedSize, 1024);
    ASSERT_EQUALS(options.cappedMaxDocs, 0);
}

TEST(CollectionOptions, InvalidStorageEngineField) {
    // "storageEngine" field has to be an object if present.
    ASSERT_NOT_OK(CollectionOptions().parse(fromjson("{storageEngine: 1}")));

    // Every field under "storageEngine" has to be an object.
    ASSERT_NOT_OK(CollectionOptions().parse(fromjson("{storageEngine: {storageEngine1: 1}}")));

    // Empty "storageEngine" not allowed
    ASSERT_OK(CollectionOptions().parse(fromjson("{storageEngine: {}}")));
}

TEST(CollectionOptions, ParseEngineField) {
    CollectionOptions opts;
    ASSERT_OK(opts.parse(
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
    CollectionOptions opts;
    ASSERT_OK(opts.parse(fromjson("{storageEngine: {storageEngine1: {x: 1}}}")));
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
    CollectionOptions options;
    ASSERT_NOT_OK(options.parse(fromjson("{collation: 'notAnObject'}")));
}

TEST(CollectionOptions, FailToParseCollationThatIsAnEmptyObject) {
    CollectionOptions options;
    ASSERT_NOT_OK(options.parse(fromjson("{collation: {}}")));
}

TEST(CollectionOptions, CollationFieldParsesCorrectly) {
    CollectionOptions options;
    ASSERT_OK(options.parse(fromjson("{collation: {locale: 'en'}}")));
    ASSERT_BSONOBJ_EQ(options.collation, fromjson("{locale: 'en'}"));
    ASSERT_OK(options.validate());
}

TEST(CollectionOptions, ParsedCollationObjShouldBeOwned) {
    CollectionOptions options;
    ASSERT_OK(options.parse(fromjson("{collation: {locale: 'en'}}")));
    ASSERT_BSONOBJ_EQ(options.collation, fromjson("{locale: 'en'}"));
    ASSERT_TRUE(options.collation.isOwned());
}

TEST(CollectionOptions, ResetClearsCollationField) {
    CollectionOptions options;
    ASSERT_TRUE(options.collation.isEmpty());
    ASSERT_OK(options.parse(fromjson("{collation: {locale: 'en'}}")));
    ASSERT_FALSE(options.collation.isEmpty());
}

TEST(CollectionOptions, CollationFieldLeftEmptyWhenOmitted) {
    CollectionOptions options;
    ASSERT_OK(options.parse(fromjson("{validator: {a: 1}}")));
    ASSERT_TRUE(options.collation.isEmpty());
}

TEST(CollectionOptions, CollationFieldNotDumpedToBSONWhenOmitted) {
    CollectionOptions options;
    ASSERT_OK(options.parse(fromjson("{validator: {a: 1}}")));
    ASSERT_TRUE(options.collation.isEmpty());
    BSONObj asBSON = options.toBSON();
    ASSERT_FALSE(asBSON["collation"]);
}

TEST(CollectionOptions, ViewParsesCorrectly) {
    CollectionOptions options;
    ASSERT_OK(options.parse(fromjson("{viewOn: 'c', pipeline: [{$match: {}}]}")));
    ASSERT_EQ(options.viewOn, "c");
    ASSERT_BSONOBJ_EQ(options.pipeline, fromjson("[{$match: {}}]"));
}

TEST(CollectionOptions, ViewParsesCorrectlyWithoutPipeline) {
    CollectionOptions options;
    ASSERT_OK(options.parse(fromjson("{viewOn: 'c'}")));
    ASSERT_EQ(options.viewOn, "c");
    ASSERT_BSONOBJ_EQ(options.pipeline, BSONObj());
}

TEST(CollectionOptions, PipelineFieldRequiresViewOn) {
    CollectionOptions options;
    ASSERT_NOT_OK(options.parse(fromjson("{pipeline: [{$match: {}}]}")));
}

TEST(CollectionOptions, UnknownTopLevelOptionFailsToParse) {
    CollectionOptions options;
    auto status = options.parse(fromjson("{invalidOption: 1}"));
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.code(), ErrorCodes::InvalidOptions);
}

TEST(CollectionOptions, CreateOptionIgnoredIfFirst) {
    CollectionOptions options;
    auto status = options.parse(fromjson("{create: 1}"));
    ASSERT_OK(status);
}

TEST(CollectionOptions, CreateOptionIgnoredIfNotFirst) {
    CollectionOptions options;
    auto status = options.parse(fromjson("{capped: true, create: 1, size: 1024}"));
    ASSERT_OK(status);
    ASSERT_EQ(options.capped, true);
    ASSERT_EQ(options.cappedSize, 1024L);
}

TEST(CollectionOptions, UnknownOptionIgnoredIfCreateOptionFirst) {
    CollectionOptions options;
    ASSERT_OK(options.parse(fromjson("{create: 1, invalidOption: 1}")));
}

TEST(CollectionOptions, UnknownOptionIgnoredIfCreateOptionPresent) {
    CollectionOptions options;
    ASSERT_OK(options.parse(fromjson("{invalidOption: 1, create: 1}")));
}

TEST(CollectionOptions, UnknownOptionRejectedIfCreateOptionNotPresent) {
    CollectionOptions options;
    auto status = options.parse(fromjson("{invalidOption: 1}"));
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.code(), ErrorCodes::InvalidOptions);
}

TEST(CollectionOptions, DuplicateCreateOptionIgnoredIfCreateOptionFirst) {
    CollectionOptions options;
    auto status = options.parse(BSON("create" << 1 << "create" << 1));
    ASSERT_OK(status);
}

TEST(CollectionOptions, DuplicateCreateOptionIgnoredIfCreateOptionNotFirst) {
    CollectionOptions options;
    auto status =
        options.parse(BSON("capped" << true << "create" << 1 << "create" << 1 << "size" << 1024));
    ASSERT_OK(status);
}

TEST(CollectionOptions, MaxTimeMSWhitelistedOptionIgnored) {
    CollectionOptions options;
    auto status = options.parse(fromjson("{maxTimeMS: 1}"));
    ASSERT_OK(status);
}

TEST(CollectionOptions, WriteConcernWhitelistedOptionIgnored) {
    CollectionOptions options;
    auto status = options.parse(fromjson("{writeConcern: 1}"));
    ASSERT_OK(status);
}

TEST(CollectionOptions, ParseUUID) {
    CollectionOptions options;
    CollectionUUID uuid = CollectionUUID::gen();

    // Check required parse failures
    ASSERT_FALSE(options.uuid);
    ASSERT_NOT_OK(options.parse(uuid.toBSON()));
    ASSERT_NOT_OK(options.parse(BSON("uuid" << 1)));
    ASSERT_NOT_OK(options.parse(BSON("uuid" << 1), CollectionOptions::parseForStorage));
    ASSERT_FALSE(options.uuid);

    // Check successful parse and roundtrip.
    ASSERT_OK(options.parse(uuid.toBSON(), CollectionOptions::parseForStorage));
    ASSERT(options.uuid.get() == uuid);
}
}  // namespace mongo
