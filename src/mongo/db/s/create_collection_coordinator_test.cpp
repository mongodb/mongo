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

#include "mongo/unittest/unittest.h"

#include "mongo/db/s/create_collection_coordinator.h"

namespace mongo {
namespace {

static const auto kShardKey = BSON("x" << 1);
static const NamespaceString kNs{"db.test"};

TEST(CreateCollectionCoordinator, pre60CompatibleGetters) {
    const auto kUUID = UUID::gen();

    auto req = [&] {
        CreateCollectionRequest creq;
        creq.setShardKey(kShardKey.getOwned());
        creq.setCollectionUUID(kUUID);
        creq.setImplicitlyCreateIndex(false);
        creq.setEnforceUniquenessCheck(false);
        return creq;
    };

    auto pre60CompatDoc = [&] {
        auto doc = CreateCollectionCoordinatorDocumentPre60Compatible();
        doc.setShardingDDLCoordinatorMetadata(
            {{kNs, DDLCoordinatorTypeEnum::kCreateCollectionPre60Compatible}});
        doc.setCreateCollectionRequest(req());
        return doc;
    }();

    auto latestDoc = [&] {
        auto doc = CreateCollectionCoordinatorDocument();
        doc.setShardingDDLCoordinatorMetadata({{kNs, DDLCoordinatorTypeEnum::kCreateCollection}});
        doc.setCreateCollectionRequest(req());
        return doc;
    }();

    ASSERT(pre60CompatDoc.getShardKey());
    ASSERT(latestDoc.getShardKey());
    ASSERT_BSONOBJ_EQ(*pre60CompatDoc.getShardKey(), *latestDoc.getShardKey());
    ASSERT(pre60CompatDoc.getCollectionUUID());
    ASSERT(latestDoc.getCollectionUUID());
    ASSERT_EQ(*pre60CompatDoc.getCollectionUUID(), *latestDoc.getCollectionUUID());
    ASSERT_EQ(pre60CompatDoc.getImplicitlyCreateIndex(), latestDoc.getImplicitlyCreateIndex());
    ASSERT_EQ(pre60CompatDoc.getEnforceUniquenessCheck(), latestDoc.getEnforceUniquenessCheck());
}

TEST(CreateCollectionCoordinator, pre60CompatibleSerialization) {
    auto req = [&] {
        CreateCollectionRequest creq;
        creq.setShardKey(kShardKey.getOwned());
        creq.setCollectionUUID(UUID::gen());
        creq.setImplicitlyCreateIndex(false);
        creq.setEnforceUniquenessCheck(false);
        return creq;
    };

    auto pre60CompatDoc = [&] {
        auto doc = CreateCollectionCoordinatorDocumentPre60Compatible();
        doc.setShardingDDLCoordinatorMetadata(
            {{kNs, DDLCoordinatorTypeEnum::kCreateCollectionPre60Compatible}});
        doc.setCreateCollectionRequest(req());
        return doc;
    }();

    BSONObjBuilder builder;
    pre60CompatDoc.serialize(&builder);
    auto serialized = builder.asTempObj();

    ASSERT_BSONOBJ_EQ(
        BSONObj{},
        serialized.extractFieldsUndotted(
            CreateCollectionCoordinatorDocumentPre60Compatible::kPre60IncompatibleFields));
}

TEST(CreateCollectionCoordinator, pre60CompatibleToBSON) {

    auto req = [&] {
        CreateCollectionRequest creq;
        creq.setShardKey(kShardKey.getOwned());
        creq.setCollectionUUID(UUID::gen());
        creq.setImplicitlyCreateIndex(false);
        creq.setEnforceUniquenessCheck(false);
        return creq;
    };

    auto pre60CompatDoc = [&] {
        auto doc = CreateCollectionCoordinatorDocumentPre60Compatible();
        doc.setShardingDDLCoordinatorMetadata(
            {{kNs, DDLCoordinatorTypeEnum::kCreateCollectionPre60Compatible}});
        doc.setCreateCollectionRequest(req());
        return doc;
    }();

    auto serialized = pre60CompatDoc.toBSON();

    ASSERT_BSONOBJ_EQ(
        BSONObj{},
        serialized.extractFieldsUndotted(
            CreateCollectionCoordinatorDocumentPre60Compatible::kPre60IncompatibleFields));
}

}  // namespace
}  // namespace mongo
