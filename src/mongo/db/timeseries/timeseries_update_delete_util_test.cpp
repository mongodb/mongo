/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/document_source_merge_gen.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

class TimeseriesUpdateDeleteUtilTest : public ServiceContextMongoDTest {
protected:
    void setUp() {
        ServiceContextMongoDTest::setUp();
        _opCtx = cc().makeOperationContext();
    }

    void _testClassicUpdateTranslation(const BSONObj& originalUpdate,
                                       const BSONObj& expectedTranslatedUpdate) {
        auto res = timeseries::translateUpdate(
            write_ops::UpdateModification::parseFromClassicUpdate(originalUpdate), _metaField);
        ASSERT(res.type() == write_ops::UpdateModification::Type::kClassic);
        ASSERT_BSONOBJ_EQ(res.getUpdateClassic(), expectedTranslatedUpdate);
    }

    ServiceContext::UniqueOperationContext _opCtx;
    StringData _metaField = "tag";
    NamespaceString _ns{"timeseries_update_delete_util_test", "system.buckets.t"};
};

TEST_F(TimeseriesUpdateDeleteUtilTest, QueryOnlyDependsOnMetaFieldNoMetaField) {
    // Empty query.
    ASSERT_TRUE(timeseries::queryOnlyDependsOnMetaField(
        _opCtx.get(), _ns, BSONObj(), boost::none, LegacyRuntimeConstants(), boost::none));

    // Query on the "meta" field.
    ASSERT_FALSE(timeseries::queryOnlyDependsOnMetaField(_opCtx.get(),
                                                         _ns,
                                                         BSON("meta"
                                                              << "A"),
                                                         boost::none,
                                                         LegacyRuntimeConstants(),
                                                         boost::none));
}

TEST_F(TimeseriesUpdateDeleteUtilTest, QueryOnlyDependsOnMetaField) {
    // Empty query.
    ASSERT_TRUE(timeseries::queryOnlyDependsOnMetaField(
        _opCtx.get(), _ns, BSONObj(), _metaField, LegacyRuntimeConstants(), boost::none));

    // Query on the metaField using dot notation.
    ASSERT_TRUE(
        timeseries::queryOnlyDependsOnMetaField(_opCtx.get(),
                                                _ns,
                                                BSON("$and" << BSON_ARRAY(BSON(_metaField + ".a"
                                                                               << "A")
                                                                          << BSON(_metaField + ".b"
                                                                                  << "B"))),
                                                _metaField,
                                                LegacyRuntimeConstants(),
                                                boost::none));

    // Query on a nested field of the metaField.
    ASSERT_TRUE(timeseries::queryOnlyDependsOnMetaField(
        _opCtx.get(),
        _ns,
        BSON("$and" << BSON_ARRAY(BSON(_metaField << BSON("a"
                                                          << "A"))
                                  << BSON(_metaField << BSON("b"
                                                             << "B")))),
        _metaField,
        LegacyRuntimeConstants(),
        boost::none));


    // Query on a field that is a prefix of the metaField.
    ASSERT_FALSE(timeseries::queryOnlyDependsOnMetaField(_opCtx.get(),
                                                         _ns,
                                                         BSON(_metaField + "a"
                                                              << "A"),
                                                         _metaField,
                                                         LegacyRuntimeConstants(),
                                                         boost::none));

    // Query using $jsonSchema with the metaField required.
    ASSERT_TRUE(timeseries::queryOnlyDependsOnMetaField(
        _opCtx.get(),
        _ns,
        BSON("$jsonSchema" << BSON("required" << BSON_ARRAY(_metaField))),
        _metaField,
        LegacyRuntimeConstants(),
        boost::none));

    // Query using $jsonSchema with a field that is not the metaField required.
    ASSERT_FALSE(timeseries::queryOnlyDependsOnMetaField(
        _opCtx.get(),
        _ns,
        BSON("$jsonSchema" << BSON("required" << BSON_ARRAY("measurement"))),
        _metaField,
        LegacyRuntimeConstants(),
        boost::none));

    // Query using $jsonSchema with a field that is the metaField in dot notation required.
    ASSERT_TRUE(timeseries::queryOnlyDependsOnMetaField(
        _opCtx.get(),
        _ns,
        BSON("$jsonSchema" << BSON("required" << BSON_ARRAY(_metaField + ".a"))),
        _metaField,
        LegacyRuntimeConstants(),
        boost::none));

    // Query using $jsonSchema with the metaField required and an optional field that is not the
    // metaField.
    ASSERT_FALSE(timeseries::queryOnlyDependsOnMetaField(
        _opCtx.get(),
        _ns,
        BSON("$jsonSchema" << BSON("required"
                                   << BSON_ARRAY(_metaField) << "properties"
                                   << BSON("measurement" << BSON("description"
                                                                 << "can be any value")))),
        _metaField,
        LegacyRuntimeConstants(),
        boost::none));

    // Query using $jsonSchema with the metaField required and the metaField as a property.
    ASSERT_TRUE(timeseries::queryOnlyDependsOnMetaField(
        _opCtx.get(),
        _ns,
        BSON("$jsonSchema" << BSON("required" << BSON_ARRAY(_metaField) << "properties"
                                              << BSON(_metaField << BSON("bsonType"
                                                                         << "string")))),
        _metaField,
        LegacyRuntimeConstants(),
        boost::none));

    // Query using $jsonSchema with a field that is not the metaField as a property of the
    // metaField.
    ASSERT_TRUE(timeseries::queryOnlyDependsOnMetaField(
        _opCtx.get(),
        _ns,
        BSON("$jsonSchema" << BSON(
                 "properties" << BSON(_metaField
                                      << BSON("properties" << BSON("a" << BSON("bsonType"
                                                                               << "string")))))),
        _metaField,
        LegacyRuntimeConstants(),
        boost::none));
}

TEST_F(TimeseriesUpdateDeleteUtilTest, QueryOnlyDependsOnMetaFieldLet) {
    // Query on the metaField.
    ASSERT_TRUE(timeseries::queryOnlyDependsOnMetaField(
        _opCtx.get(),
        _ns,
        BSON("$expr" << BSON("$eq" << BSON_ARRAY("$" + _metaField << "$$" + _metaField))),
        _metaField,
        LegacyRuntimeConstants(),
        BSON(_metaField << "A")));

    // Query on a field that is not the metaField.
    ASSERT_FALSE(timeseries::queryOnlyDependsOnMetaField(
        _opCtx.get(),
        _ns,
        BSON("$expr" << BSON("$eq" << BSON_ARRAY("$meta"
                                                 << "$$" + _metaField))),
        _metaField,
        LegacyRuntimeConstants(),
        BSON(_metaField << "A")));
}

TEST_F(TimeseriesUpdateDeleteUtilTest, TranslateQuery) {
    // Translate empty query.
    ASSERT_BSONOBJ_EQ(timeseries::translateQuery(BSONObj(), _metaField), BSONObj());

    // Translate query using dot notation.
    ASSERT_BSONOBJ_EQ(timeseries::translateQuery(BSON("$and" << BSON_ARRAY(BSON(_metaField + ".a"
                                                                                << "A")
                                                                           << BSON(_metaField + ".b"
                                                                                   << "B"))),
                                                 _metaField),
                      BSON("$and" << BSON_ARRAY(BSON("meta.a"
                                                     << "A")
                                                << BSON("meta.b"
                                                        << "B"))));

    // Translate query on a nested field.
    ASSERT_BSONOBJ_EQ(
        timeseries::translateQuery(BSON("$and" << BSON_ARRAY(BSON(_metaField << BSON("a"
                                                                                     << "A"))
                                                             << BSON(_metaField << BSON("b"
                                                                                        << "B")))),
                                   _metaField),
        BSON("$and" << BSON_ARRAY(BSON("meta" << BSON("a"
                                                      << "A"))
                                  << BSON("meta" << BSON("b"
                                                         << "B")))));

    // Translate query on a field that is the prefix of the metaField.
    ASSERT_BSONOBJ_EQ(timeseries::translateQuery(BSON(_metaField + "a"
                                                      << "A"),
                                                 _metaField),
                      BSON(_metaField + "a"
                           << "A"));

    // Translate query using let.
    ASSERT_BSONOBJ_EQ(
        timeseries::translateQuery(
            BSON("$expr" << BSON("$eq" << BSON_ARRAY("$" + _metaField << "$$" + _metaField))),
            _metaField),
        BSON("$expr" << BSON("$eq" << BSON_ARRAY("$meta"
                                                 << "$$" + _metaField))));

    // Translate query using $literal.
    ASSERT_BSONOBJ_EQ(
        timeseries::translateQuery(
            BSON("$expr" << BSON(
                     "$eq" << BSON_ARRAY("$" + _metaField + ".b" << BSON("$literal"
                                                                         << "$" + _metaField)))),
            _metaField),
        BSON("$expr" << BSON("$eq" << BSON_ARRAY("$meta.b" << BSON("$literal"
                                                                   << "$" + _metaField)))));

    // Translate query using $jsonSchema with the metaField required.
    ASSERT_BSONOBJ_EQ(
        timeseries::translateQuery(
            BSON("$jsonSchema" << BSON("required" << BSON_ARRAY(_metaField))), _metaField),
        BSON("$jsonSchema" << BSON("required" << BSON_ARRAY("meta"))));

    // Translate query using $jsonSchema a field that is not the metaField required.
    ASSERT_BSONOBJ_EQ(
        timeseries::translateQuery(
            BSON("$jsonSchema" << BSON("required" << BSON_ARRAY("measurement"))), _metaField),
        BSON("$jsonSchema" << BSON("required" << BSON_ARRAY("measurement"))));

    // Translate query using $jsonSchema with the metaField in dot notation required.
    ASSERT_BSONOBJ_EQ(
        timeseries::translateQuery(
            BSON("$jsonSchema" << BSON("required" << BSON_ARRAY(_metaField + ".a"))), _metaField),
        BSON("$jsonSchema" << BSON("required" << BSON_ARRAY("meta.a"))));

    // Translate query using $jsonSchema with the metaField required and a required subfield of the
    // metaField with the same name as the metaField.
    ASSERT_BSONOBJ_EQ(
        timeseries::translateQuery(
            BSON("$jsonSchema" << BSON(
                     "required" << BSON_ARRAY(_metaField) << "properties"
                                << BSON(_metaField << BSON("required" << BSON_ARRAY(_metaField))))),
            _metaField),
        BSON("$jsonSchema" << BSON("required"
                                   << BSON_ARRAY("meta") << "properties"
                                   << BSON("meta" << BSON("required" << BSON_ARRAY(_metaField))))));

    // Translate query using $jsonSchema with the metaField required and the metaField as a
    // property.
    ASSERT_BSONOBJ_EQ(
        timeseries::translateQuery(
            BSON("$jsonSchema" << BSON("required" << BSON_ARRAY(_metaField) << "properties"
                                                  << BSON(_metaField << BSON("bsonType"
                                                                             << "string")))),
            _metaField),
        BSON("$jsonSchema" << BSON("required" << BSON_ARRAY("meta") << "properties"
                                              << BSON("meta" << BSON("bsonType"
                                                                     << "string")))));

    // Translate query using $jsonSchema with a field with the same name as the metaField as a
    // property of the metaField.
    ASSERT_BSONOBJ_EQ(
        timeseries::translateQuery(
            BSON("$jsonSchema" << BSON("properties"
                                       << BSON(_metaField
                                               << BSON("properties"
                                                       << BSON(_metaField << BSON("bsonType"
                                                                                  << "string")))))),
            _metaField),
        BSON("$jsonSchema" << BSON(
                 "properties" << BSON(
                     "meta" << BSON("properties" << BSON(_metaField << BSON("bsonType"
                                                                            << "string")))))));

    // Translate query with a field with the same name as the metaField as a subfield of the
    // metaField.
    ASSERT_BSONOBJ_EQ(
        timeseries::translateQuery(BSON(_metaField << BSON(_metaField << "a")), _metaField),
        BSON("meta" << BSON(_metaField << "a")));

    // Translate query with a field with the same name as the metaField as a subfield of a field
    // that is not the metaField.
    ASSERT_BSONOBJ_EQ(timeseries::translateQuery(BSON("a" << BSON(_metaField << "a")), _metaField),
                      BSON("a" << BSON(_metaField << "a")));

    // Translate query with the metaField nested within nested operators.
    ASSERT_BSONOBJ_EQ(
        timeseries::translateQuery(
            BSON("$and" << BSON_ARRAY(
                     "$or" << BSON_ARRAY(BSON(_metaField << BSON("$ne"
                                                                 << "A"))
                                         << BSON("a" << BSON(_metaField << BSON("$eq"
                                                                                << "B"))))
                           << BSON(_metaField << BSON("b"
                                                      << "B")))),
            _metaField),
        BSON("$and" << BSON_ARRAY("$or"
                                  << BSON_ARRAY(BSON("meta" << BSON("$ne"
                                                                    << "A"))
                                                << BSON("a" << BSON(_metaField << BSON("$eq"
                                                                                       << "B"))))
                                  << BSON("meta" << BSON("b"
                                                         << "B")))));
}

TEST_F(TimeseriesUpdateDeleteUtilTest, UpdateOnlyModifiesMetaField) {
    // {$set: {tag: "A"}}
    ASSERT_TRUE(timeseries::updateOnlyModifiesMetaField(
        _opCtx.get(),
        _ns,
        write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$set" << BSON(_metaField << "A"))),
        _metaField));

    // {$set: {nonMetaField: "A"}}
    ASSERT_FALSE(timeseries::updateOnlyModifiesMetaField(
        _opCtx.get(),
        _ns,
        write_ops::UpdateModification::parseFromClassicUpdate(BSON("$set" << BSON("nonMetaField"
                                                                                  << "A"))),
        _metaField));

    // {$set: {tag.a: "A"}}
    ASSERT_TRUE(timeseries::updateOnlyModifiesMetaField(
        _opCtx.get(),
        _ns,
        write_ops::UpdateModification::parseFromClassicUpdate(BSON("$set" << BSON(_metaField + ".a"
                                                                                  << "A"))),
        _metaField));

    // {$unset: {tag.a: ""}}
    ASSERT_TRUE(timeseries::updateOnlyModifiesMetaField(
        _opCtx.get(),
        _ns,
        write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$unset" << BSON(_metaField + ".a"
                                  << ""))),
        _metaField));

    // {$unset: {tag.a: ""}, {$inc: {nonMetaField: 10}}}
    ASSERT_FALSE(timeseries::updateOnlyModifiesMetaField(
        _opCtx.get(),
        _ns,
        write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$unset" << BSON(_metaField + ".a"
                                  << "")
                          << "inc" << BSON("nonMetaField" << 10))),
        _metaField));

    // {$unset: {tag.a: ""}, {$inc: {tagggg: 10}}}
    ASSERT_FALSE(timeseries::updateOnlyModifiesMetaField(
        _opCtx.get(),
        _ns,
        write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$unset" << BSON(_metaField + ".a"
                                  << "")
                          << "inc" << BSON(_metaField + "ggg" << 10))),
        _metaField));

    // TODO: SERVER-59173 Modify this test since renaming the metaField is not allowed.
    // {$rename: {"tag.a.a": "A", "tag.b": "B"}}
    ASSERT_TRUE(timeseries::updateOnlyModifiesMetaField(
        _opCtx.get(),
        _ns,
        write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$rename" << BSON(_metaField + ".a.a"
                                   << "A" << _metaField + ".b"
                                   << "B"))),
        _metaField));

    // TODO: SERVER-59173 Modify this test since renaming the metaField is not allowed.
    // {$rename: {tag.tag.tag: 8}}
    ASSERT_TRUE(timeseries::updateOnlyModifiesMetaField(
        _opCtx.get(),
        _ns,
        write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$rename" << BSON(_metaField + "." + _metaField + "." + _metaField << 8))),
        _metaField));

    // {$set: {tag.$: 100}}
    ASSERT_TRUE(timeseries::updateOnlyModifiesMetaField(
        _opCtx.get(),
        _ns,
        write_ops::UpdateModification::parseFromClassicUpdate(BSON("$set" << BSON(_metaField + ".$"
                                                                                  << ""))),
        _metaField));

    // {$pull: {tag.arr: {$gte: 80}}}
    ASSERT_TRUE(timeseries::updateOnlyModifiesMetaField(
        _opCtx.get(),
        _ns,
        write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$pull" << BSON(_metaField + ".arr" << BSON("gte" << 80)))),
        _metaField));

    // Update a collection that does not have a metaField.
    // {$inc: {nonMetaField: ""}}
    ASSERT_FALSE(timeseries::updateOnlyModifiesMetaField(
        _opCtx.get(),
        _ns,
        write_ops::UpdateModification::parseFromClassicUpdate(BSON("$inc" << BSON("nonMetaField"
                                                                                  << ""))),
        StringData()));

    // Update with an empty document, which is considered a replacement document.
    ASSERT_THROWS(timeseries::updateOnlyModifiesMetaField(
                      _opCtx.get(), _ns, write_ops::UpdateModification(), _metaField),
                  ExceptionFor<ErrorCodes::InvalidOptions>);
}

TEST_F(TimeseriesUpdateDeleteUtilTest, TranslateUpdate) {
    // {$set: {nonMetaField: "A"}} => {$set: {nonMetaField: "A"}}
    _testClassicUpdateTranslation(BSON("$set" << BSON("nonMetaField"
                                                      << "A")),
                                  BSON("$set" << BSON("nonMetaField"
                                                      << "A")));

    // {$set: {tag: "A"}} => {$set: {meta: "A"}}
    _testClassicUpdateTranslation(BSON("$set" << BSON(_metaField << "A")),
                                  BSON("$set" << BSON("meta"
                                                      << "A")));

    // {$unset: {tag.a: ""}, {$inc: {nonMetaField: 10}}} =>
    // {$unset: {meta.a: ""}, {$inc: {nonMetaField: 10}}}
    _testClassicUpdateTranslation(BSON("$unset" << BSON(_metaField + ".a"
                                                        << "")
                                                << "inc" << BSON("nonMetaField" << 10)),
                                  BSON("$unset" << BSON("meta.a"
                                                        << "")
                                                << "inc" << BSON("nonMetaField" << 10)));

    // TODO: SERVER-59173 Remove or modify this test since renaming the metaField is not allowed.
    // {$rename: {"tag.a.a": "A", "tag.b": "B"}}
    _testClassicUpdateTranslation(BSON("$rename" << BSON(_metaField + ".a.a"
                                                         << "A" << _metaField + ".b"
                                                         << "B")),
                                  BSON("$rename" << BSON("meta.a.a"
                                                         << "A"
                                                         << "meta.b"
                                                         << "B")));

    // TODO: SERVER-59173 Remove or modify this test since renaming the metaField is not allowed.
    // {$rename: {tag.tag.tag: 8}}
    _testClassicUpdateTranslation(
        BSON("$rename" << BSON(_metaField + "." + _metaField + "." + _metaField << 8)),
        BSON("$rename" << BSON("meta.tag.tag" << 8)));
}

// Translate an update with an empty metaField, which violates the translation method's
// precondition.
DEATH_TEST_F(TimeseriesUpdateDeleteUtilTest,
             TranslateUpdateWithEmptyMetaField,
             "Invariant failure") {
    timeseries::translateUpdate(write_ops::UpdateModification::parseFromClassicUpdate(
                                    BSON("$set" << BSON(_metaField << "A"))),
                                StringData());
}

// Translate an empty update, which is considered a replacement document.
DEATH_TEST_F(TimeseriesUpdateDeleteUtilTest, TranslateEmptyUpdate, "Invariant failure") {
    timeseries::translateUpdate(write_ops::UpdateModification(), _metaField);
}
}  // namespace
}  // namespace mongo
