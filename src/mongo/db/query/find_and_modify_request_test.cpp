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

#include "mongo/bson/json.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/pipeline/runtime_constants_gen.h"
#include "mongo/db/query/find_and_modify_request.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(FindAndModifyRequest, BasicUpdate) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));
    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON({}));
}

TEST(FindAndModifyRequest, PipelineUpdate) {
    setTestCommandsEnabled(true);
    const BSONObj query(BSON("x" << 1));
    const BSONObj pipelineBSON(
        BSON("pipeline" << BSON_ARRAY(BSON("$addFields" << BSON("y" << 1)))));
    auto request = FindAndModifyRequest::makeUpdate(
        NamespaceString("test.user"), query, pipelineBSON["pipeline"]);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: [{$addFields: {y: 1}}]
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON({}));
}

TEST(FindAndModifyRequest, UpdateWithUpsert) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));
    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);
    request.setUpsert(true);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 },
            upsert: true
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON({}));
}

TEST(FindAndModifyRequest, UpdateWithUpsertFalse) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));
    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);
    request.setUpsert(false);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON({}));
}

TEST(FindAndModifyRequest, UpdateWithProjection) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));
    const BSONObj field(BSON("z" << 1));

    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);
    request.setFieldProjection(field);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 },
            fields: { z: 1 }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON({}));
}

TEST(FindAndModifyRequest, UpdateWithNewTrue) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));

    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);
    request.setShouldReturnNew(true);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 },
            new: true
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON({}));
}

TEST(FindAndModifyRequest, UpdateWithNewFalse) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));

    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);
    request.setShouldReturnNew(false);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON({}));
}

TEST(FindAndModifyRequest, UpdateWithSort) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));
    const BSONObj sort(BSON("z" << -1));

    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);
    request.setSort(sort);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 },
            sort: { z: -1 }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON({}));
}

TEST(FindAndModifyRequest, UpdateWithCollation) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));
    const BSONObj collation(BSON("locale"
                                 << "en_US"));

    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);
    request.setCollation(collation);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 },
            collation: { locale: 'en_US' }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON({}));
}

TEST(FindAndModifyRequest, UpdateWithArrayFilters) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));
    const std::vector<BSONObj> arrayFilters{BSON("i" << 0)};

    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);
    request.setArrayFilters(arrayFilters);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 },
            arrayFilters: [ { i: 0 } ]
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON({}));
}

TEST(FindAndModifyRequest, UpdateWithWriteConcern) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));
    const WriteConcernOptions writeConcern(2, WriteConcernOptions::SyncMode::FSYNC, 150);

    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);
    request.setWriteConcern(writeConcern);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 },
            writeConcern: { w: 2, fsync: true, wtimeout: 150 }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON({}));
}

TEST(FindAndModifyRequest, UpdateWithRuntimeConstants) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));

    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);
    request.setRuntimeConstants({Date_t(), Timestamp(1, 0)});

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 },
            runtimeConstants: {
                localNow: new Date(0),
                clusterTime: Timestamp(1, 0)
            }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON({}));
}

TEST(FindAndModifyRequest, UpdateWithFullSpec) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj update(BSON("y" << 1));
    const BSONObj sort(BSON("z" << -1));
    const BSONObj collation(BSON("locale"
                                 << "en_US"));
    const std::vector<BSONObj> arrayFilters{BSON("i" << 0)};
    const BSONObj field(BSON("x" << 1 << "y" << 1));
    const WriteConcernOptions writeConcern(2, WriteConcernOptions::SyncMode::FSYNC, 150);
    auto rtc = RuntimeConstants{Date_t(), Timestamp(1, 0)};

    auto request = FindAndModifyRequest::makeUpdate(NamespaceString("test.user"), query, update);
    request.setFieldProjection(field);
    request.setShouldReturnNew(true);
    request.setSort(sort);
    request.setCollation(collation);
    request.setArrayFilters(arrayFilters);
    request.setRuntimeConstants(rtc);
    request.setWriteConcern(writeConcern);
    request.setBypassDocumentValidation(true);
    request.setUpsert(true);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 },
            upsert: true,
            fields: { x: 1, y: 1 },
            sort: { z: -1 },
            collation: { locale: 'en_US' },
            arrayFilters: [ { i: 0 } ],
            runtimeConstants: {
                localNow: new Date(0),
                clusterTime: Timestamp(1, 0)
            },
            new: true,
            writeConcern: { w: 2, fsync: true, wtimeout: 150 },
            bypassDocumentValidation: true
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON({}));
}

TEST(FindAndModifyRequest, BasicRemove) {
    const BSONObj query(BSON("x" << 1));
    auto request = FindAndModifyRequest::makeRemove(NamespaceString("test.user"), query);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            remove: true
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON({}));
}

TEST(FindAndModifyRequest, RemoveWithProjection) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj field(BSON("z" << 1));

    auto request = FindAndModifyRequest::makeRemove(NamespaceString("test.user"), query);
    request.setFieldProjection(field);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            remove: true,
            fields: { z: 1 }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON({}));
}

TEST(FindAndModifyRequest, RemoveWithSort) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj sort(BSON("z" << -1));

    auto request = FindAndModifyRequest::makeRemove(NamespaceString("test.user"), query);
    request.setSort(sort);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            remove: true,
            sort: { z: -1 }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON({}));
}

TEST(FindAndModifyRequest, RemoveWithCollation) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj collation(BSON("locale"
                                 << "en_US"));

    auto request = FindAndModifyRequest::makeRemove(NamespaceString("test.user"), query);
    request.setCollation(collation);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            remove: true,
            collation: { locale: 'en_US' }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON({}));
}

TEST(FindAndModifyRequest, RemoveWithWriteConcern) {
    const BSONObj query(BSON("x" << 1));
    const WriteConcernOptions writeConcern(2, WriteConcernOptions::SyncMode::FSYNC, 150);

    auto request = FindAndModifyRequest::makeRemove(NamespaceString("test.user"), query);
    request.setWriteConcern(writeConcern);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            remove: true,
            writeConcern: { w: 2, fsync: true, wtimeout: 150 }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON({}));
}

TEST(FindAndModifyRequest, RemoveWithFullSpec) {
    const BSONObj query(BSON("x" << 1));
    const BSONObj sort(BSON("z" << -1));
    const BSONObj collation(BSON("locale"
                                 << "en_US"));
    const BSONObj field(BSON("x" << 1 << "y" << 1));
    const WriteConcernOptions writeConcern(2, WriteConcernOptions::SyncMode::FSYNC, 150);
    auto rtc = RuntimeConstants{Date_t(), Timestamp(1, 0)};

    auto request = FindAndModifyRequest::makeRemove(NamespaceString("test.user"), query);
    request.setFieldProjection(field);
    request.setSort(sort);
    request.setCollation(collation);
    request.setWriteConcern(writeConcern);
    request.setRuntimeConstants(rtc);

    BSONObj expectedObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            remove: true,
            fields: { x: 1, y: 1 },
            sort: { z: -1 },
            collation: { locale: 'en_US' },
            runtimeConstants: {
                localNow: new Date(0),
                clusterTime: Timestamp(1, 0)
            },
            writeConcern: { w: 2, fsync: true, wtimeout: 150 }
        })json"));

    ASSERT_BSONOBJ_EQ(expectedObj, request.toBSON({}));
}

TEST(FindAndModifyRequest, ParseWithUpdateOnlyRequiredFields) {
    BSONObj cmdObj(fromjson(R"json({
            query: { x: 1 },
            update: { y: 1 }
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_OK(parseStatus.getStatus());

    auto request = parseStatus.getValue();
    ASSERT_EQ(NamespaceString("a.b"), request.getNamespaceString());
    ASSERT_BSONOBJ_EQ(BSON("x" << 1), request.getQuery());
    ASSERT(request.getUpdate());
    ASSERT(request.getUpdate()->type() == write_ops::UpdateModification::Type::kClassic);
    ASSERT_BSONOBJ_EQ(BSON("y" << 1), request.getUpdate()->getUpdateClassic());
    ASSERT_EQ(false, request.isUpsert());
    ASSERT_EQ(false, request.isRemove());
    ASSERT_BSONOBJ_EQ(BSONObj(), request.getFields());
    ASSERT_BSONOBJ_EQ(BSONObj(), request.getSort());
    ASSERT_BSONOBJ_EQ(BSONObj(), request.getCollation());
    ASSERT_EQ(0u, request.getArrayFilters().size());
    ASSERT_EQ(false, request.shouldReturnNew());
}

TEST(FindAndModifyRequest, ParseWithUpdateFullSpec) {
    BSONObj cmdObj(fromjson(R"json({
            query: { x: 1 },
            update: { y: 1 },
            upsert: true,
            fields: { x: 1, y: 1 },
            sort: { z: -1 },
            collation: {locale: 'en_US' },
            arrayFilters: [ { i: 0 } ],
            new: true
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_OK(parseStatus.getStatus());

    auto request = parseStatus.getValue();
    ASSERT_EQ(NamespaceString("a.b"), request.getNamespaceString());
    ASSERT_BSONOBJ_EQ(BSON("x" << 1), request.getQuery());
    ASSERT(request.getUpdate());
    ASSERT(request.getUpdate()->type() == write_ops::UpdateModification::Type::kClassic);
    ASSERT_BSONOBJ_EQ(BSON("y" << 1), request.getUpdate()->getUpdateClassic());
    ASSERT_EQ(true, request.isUpsert());
    ASSERT_EQ(false, request.isRemove());
    ASSERT_BSONOBJ_EQ(BSON("x" << 1 << "y" << 1), request.getFields());
    ASSERT_BSONOBJ_EQ(BSON("z" << -1), request.getSort());
    ASSERT_BSONOBJ_EQ(BSON("locale"
                           << "en_US"),
                      request.getCollation());
    ASSERT_EQ(1u, request.getArrayFilters().size());
    ASSERT_BSONOBJ_EQ(BSON("i" << 0), request.getArrayFilters()[0]);
    ASSERT_EQ(true, request.shouldReturnNew());
}

TEST(FindAndModifyRequest, ParseWithRemoveOnlyRequiredFields) {
    BSONObj cmdObj(fromjson(R"json({
            query: { x: 1 },
            remove: true
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_OK(parseStatus.getStatus());

    auto request = parseStatus.getValue();
    ASSERT_EQ(NamespaceString("a.b"), request.getNamespaceString());
    ASSERT_BSONOBJ_EQ(BSON("x" << 1), request.getQuery());
    ASSERT_FALSE(request.getUpdate());
    ASSERT_EQ(false, request.isUpsert());
    ASSERT_EQ(true, request.isRemove());
    ASSERT_BSONOBJ_EQ(BSONObj(), request.getFields());
    ASSERT_BSONOBJ_EQ(BSONObj(), request.getSort());
    ASSERT_BSONOBJ_EQ(BSONObj(), request.getCollation());
    ASSERT_EQ(false, request.shouldReturnNew());
}

TEST(FindAndModifyRequest, ParseWithRemoveFullSpec) {
    BSONObj cmdObj(fromjson(R"json({
            query: { x: 1 },
            remove: true,
            fields: { x: 1, y: 1 },
            sort: { z: -1 },
            collation: { locale: 'en_US' },
            new: false
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_OK(parseStatus.getStatus());

    auto request = parseStatus.getValue();
    ASSERT_EQ(NamespaceString("a.b"), request.getNamespaceString());
    ASSERT_BSONOBJ_EQ(BSON("x" << 1), request.getQuery());
    ASSERT_FALSE(request.getUpdate());
    ASSERT_EQ(false, request.isUpsert());
    ASSERT_EQ(true, request.isRemove());
    ASSERT_BSONOBJ_EQ(BSON("x" << 1 << "y" << 1), request.getFields());
    ASSERT_BSONOBJ_EQ(BSON("z" << -1), request.getSort());
    ASSERT_BSONOBJ_EQ(BSON("locale"
                           << "en_US"),
                      request.getCollation());
    ASSERT_EQ(false, request.shouldReturnNew());
}

TEST(FindAndModifyRequest, ParseWithIncompleteSpec) {
    BSONObj cmdObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 }
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_NOT_OK(parseStatus.getStatus());
}

TEST(FindAndModifyRequest, ParseWithAmbiguousUpdateRemove) {
    BSONObj cmdObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            update: { y: 1 },
            remove: true
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_NOT_OK(parseStatus.getStatus());
}

TEST(FindAndModifyRequest, ParseWithRemovePlusUpsert) {
    BSONObj cmdObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            remove: true,
            upsert: true
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_NOT_OK(parseStatus.getStatus());
}

TEST(FindAndModifyRequest, ParseWithRemoveAndReturnNew) {
    BSONObj cmdObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            remove: true,
            new: true
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_NOT_OK(parseStatus.getStatus());
}

TEST(FindAndModifyRequest, ParseWithRemoveAndArrayFilters) {
    BSONObj cmdObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            remove: true,
            arrayFilters: [ { i: 0 } ]
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_NOT_OK(parseStatus.getStatus());
}

TEST(FindAndModifyRequest, ParseWithCollationTypeMismatch) {
    BSONObj cmdObj(fromjson(R"json({
            query: { x: 1 },
            update: { y: 1 },
            collation: 'en_US'
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_EQ(parseStatus.getStatus(), ErrorCodes::TypeMismatch);
}

TEST(FindAndModifyRequest, ParseWithBypassDocumentValidation) {
    BSONObj cmdObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            remove: true,
            bypassDocumentValidation: true
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_OK(parseStatus.getStatus());

    auto request = parseStatus.getValue();
    ASSERT_EQ(NamespaceString("a.b"), request.getNamespaceString());
    ASSERT_BSONOBJ_EQ(BSON("x" << 1), request.getQuery());
    ASSERT_FALSE(request.getUpdate());
    ASSERT_EQ(true, request.isRemove());
    ASSERT_EQ(true, request.getBypassDocumentValidation());
}

TEST(FindAndModifyRequest, ParseWithWriteConcernAsArray) {
    BSONObj cmdObj(fromjson(R"json({
            findAndModify: 'user',
            query: { x: 1 },
            remove: true,
            writeConcern: []
        })json"));

    auto parseStatus = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_EQ(ErrorCodes::TypeMismatch, parseStatus.getStatus());
}

TEST(FindAndModifyRequest, ParsesAndSerializesPipelineUpdate) {
    setTestCommandsEnabled(true);
    BSONObj cmdObj(fromjson(R"json({
            query: { x: 1 },
            update: [{$replaceWith: {y: 1}}]
        })json"));

    auto request =
        unittest::assertGet(FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj));
    ASSERT(request.getUpdate());
    ASSERT(request.getUpdate()->type() == write_ops::UpdateModification::Type::kPipeline);
    auto serialized = request.toBSON({});
    ASSERT_BSONOBJ_EQ(serialized, fromjson(R"json({
      findAndModify: "b",
      query: {x: 1},
      update: [{$replaceWith: {y: 1}}],
      fields: {},
      sort: {},
      collation: {}
    })json"));
    ASSERT_OK(FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), serialized).getStatus());
}

TEST(FindAndModifyRequest, RejectsBothArrayFiltersAndPipelineUpdate) {
    setTestCommandsEnabled(true);
    BSONObj cmdObj(fromjson(R"json({
            query: { x: 1 },
            update: [{$replaceWith: {y: 1}}],
            arrayFilters: []
        })json"));

    auto swRequestNoFilters = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_EQ(swRequestNoFilters.getStatus(), ErrorCodes::FailedToParse);

    cmdObj = fromjson(R"json({
            query: { x: 1 },
            update: [{$replaceWith: {y: 1}}],
            arrayFilters: [{"i.x": 1}]
        })json");
    auto swRequestOneFilter = FindAndModifyRequest::parseFromBSON(NamespaceString("a.b"), cmdObj);
    ASSERT_EQ(swRequestOneFilter.getStatus(), ErrorCodes::FailedToParse);
}
}  // unnamed namespace
}  // namespace mongo
