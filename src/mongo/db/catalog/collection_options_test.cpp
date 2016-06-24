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

#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

void checkRoundTrip(const CollectionOptions& options1) {
    CollectionOptions options2;
    options2.parse(options1.toBSON());
    ASSERT_EQUALS(options1.toBSON(), options2.toBSON());
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

TEST(CollectionOptions, IsValid) {
    CollectionOptions options;
    ASSERT_TRUE(options.isValid());

    options.storageEngine = fromjson("{storageEngine1: 1}");
    ASSERT_FALSE(options.isValid());
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
    ASSERT_EQ(options.validator, fromjson("{a: 1}"));

    options.validator = fromjson("{b: 1}");
    ASSERT_EQ(options.toBSON()["validator"].Obj(), fromjson("{b: 1}"));

    options.reset();
    ASSERT_EQ(options.validator, BSONObj());
    ASSERT(!options.toBSON()["validator"]);
}

TEST(CollectionOptions, ErrorBadSize) {
    ASSERT_NOT_OK(CollectionOptions().parse(fromjson("{capped: true, size: -1}")));
    ASSERT_NOT_OK(CollectionOptions().parse(fromjson("{capped: false, size: -1}")));
}

TEST(CollectionOptions, ErrorBadMax) {
    ASSERT_NOT_OK(CollectionOptions().parse(BSON("capped" << true << "max" << (1LL << 31))));
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

TEST(CollectionOptions, IgnoreUnregisteredFields) {
    ASSERT_OK(CollectionOptions().parse(BSON("create"
                                             << "c")));
    ASSERT_OK(CollectionOptions().parse(BSON("foo"
                                             << "bar")));
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
        fromjson("{unknownField: 1, "
                 "storageEngine: {storageEngine1: {x: 1, y: 2}, storageEngine2: {a: 1, b:2}}}")));
    checkRoundTrip(opts);

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

    opts.reset();

    ASSERT_TRUE(opts.storageEngine.isEmpty());
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
    ASSERT_EQ(options.collation, fromjson("{locale: 'en'}"));
    ASSERT_TRUE(options.isValid());
    ASSERT_OK(options.validate());
}

TEST(CollectionOptions, ParsedCollationObjShouldBeOwned) {
    CollectionOptions options;
    ASSERT_OK(options.parse(fromjson("{collation: {locale: 'en'}}")));
    ASSERT_EQ(options.collation, fromjson("{locale: 'en'}"));
    ASSERT_TRUE(options.collation.isOwned());
}

TEST(CollectionOptions, ResetClearsCollationField) {
    CollectionOptions options;
    ASSERT_OK(options.parse(fromjson("{collation: {locale: 'en'}}")));
    ASSERT_FALSE(options.collation.isEmpty());
    options.reset();
    ASSERT_TRUE(options.collation.isEmpty());
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
    ASSERT_EQ(options.pipeline, fromjson("[{$match: {}}]"));
}

TEST(CollectionOptions, ViewParsesCorrectlyWithoutPipeline) {
    CollectionOptions options;
    ASSERT_OK(options.parse(fromjson("{viewOn: 'c'}")));
    ASSERT_EQ(options.viewOn, "c");
    ASSERT_EQ(options.pipeline, BSONObj());
}

TEST(CollectionOptions, PipelineFieldRequiresViewOn) {
    CollectionOptions options;
    ASSERT_NOT_OK(options.parse(fromjson("{pipeline: [{$match: {}}]}")));
}
}
