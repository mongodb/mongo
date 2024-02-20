/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <boost/none.hpp>
#include <initializer_list>
#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/commands/bulk_write_crud_op.h"
#include "mongo/db/commands/bulk_write_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/ops/write_ops_parsers_test_helpers.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/index_version.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/write_ops/bulk_write_command_modifier.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

TEST(BulkWriteCommandModifier, AddInsert) {
    BSONArray insertArray = BSON_ARRAY(BSON("a" << 1) << BSON("b" << 1));

    BSONObj origInsertRequestObj = BSON("insert"
                                        << "test"
                                        << "documents" << insertArray << "writeConcern"
                                        << BSON("w" << 1) << "ordered" << true);

    for (auto docSeq : {false, true}) {
        const auto opMsgRequest(toOpMsg("TestDB", origInsertRequestObj, docSeq));

        BulkWriteCommandRequest request;
        BulkWriteCommandModifier builder(&request);
        builder.addInsert(opMsgRequest);
        builder.finishBuild();

        auto nsInfo = request.getNsInfo();
        ASSERT_EQ(1, nsInfo.size());
        ASSERT_EQ("TestDB", nsInfo[0].getNs().db_forTest());
        ASSERT_EQ("test", nsInfo[0].getNs().coll());
        ASSERT_EQ(2, request.getOps().size());
        ASSERT_EQ(boost::none, nsInfo[0].getShardVersion());
    }
}

TEST(BulkWriteCommandModifier, AddOpInsert) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "test");
    auto req = write_ops::InsertCommandRequest(nss);
    auto docs = std::vector<BSONObj>();
    docs.emplace_back(BSON("a" << 1));
    docs.emplace_back(BSON("b" << 1));
    req.setDocuments(docs);

    BulkWriteCommandRequest request;
    BulkWriteCommandModifier builder(&request);
    builder.addOp(req);
    builder.finishBuild();

    auto nsInfo = request.getNsInfo();
    ASSERT_EQ(1, nsInfo.size());
    ASSERT_EQ("TestDB", nsInfo[0].getNs().db_forTest());
    ASSERT_EQ("test", nsInfo[0].getNs().coll());
    ASSERT_EQ(2, request.getOps().size());
    ASSERT_EQ(boost::none, nsInfo[0].getShardVersion());
}

TEST(BulkWriteCommandModifier, AddInsertOps) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "test");
    auto docs = std::vector<BSONObj>();
    docs.emplace_back(BSON("a" << 1));
    docs.emplace_back(BSON("b" << 1));

    BulkWriteCommandRequest request;
    BulkWriteCommandModifier builder(&request);
    builder.addInsertOps(nss, docs);
    builder.finishBuild();

    auto nsInfo = request.getNsInfo();
    ASSERT_EQ(1, nsInfo.size());
    ASSERT_EQ("TestDB", nsInfo[0].getNs().db_forTest());
    ASSERT_EQ("test", nsInfo[0].getNs().coll());
    ASSERT_EQ(2, request.getOps().size());
    ASSERT_EQ(boost::none, nsInfo[0].getShardVersion());
}

TEST(BulkWriteCommandModifier, InsertWithShardVersion) {
    BSONArray insertArray = BSON_ARRAY(BSON("a" << 1) << BSON("b" << 1));

    const OID epoch = OID::gen();
    const Timestamp timestamp(2, 2);
    const Timestamp majorAndMinor(1, 2);

    BSONObj origInsertRequestObj = BSON("insert"
                                        << "test"
                                        << "documents" << insertArray << "writeConcern"
                                        << BSON("w" << 1) << "ordered" << true << "shardVersion"
                                        << BSON("e" << epoch << "t" << timestamp << "v"
                                                    << majorAndMinor));

    for (auto docSeq : {false, true}) {
        const auto opMsgRequest(toOpMsg("TestDB", origInsertRequestObj, docSeq));

        BulkWriteCommandRequest request = BulkWriteCommandRequest();
        BulkWriteCommandModifier builder(&request);
        builder.addInsert(opMsgRequest);
        builder.finishBuild();

        auto nsInfo = request.getNsInfo();
        ASSERT_EQ(1, nsInfo.size());
        ASSERT_EQ("TestDB", nsInfo[0].getNs().db_forTest());
        ASSERT_EQ("test", nsInfo[0].getNs().coll());
        ASSERT_NE(boost::none, nsInfo[0].getShardVersion());
        ASSERT_EQ(ShardVersionFactory::make(ChunkVersion({epoch, timestamp}, {1, 2}),
                                            boost::optional<CollectionIndexes>(boost::none))
                      .toString(),
                  (*nsInfo[0].getShardVersion()).toString());
    }
}

TEST(BulkWriteCommandModifier, AddUpdate) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "test");
    const BSONObj query = BSON("x" << 1);
    const BSONObj update = BSON("$inc" << BSON("x" << 1));
    const BSONObj collation = BSON("locale"
                                   << "en_US");
    const BSONObj arrayFilter = BSON("i" << 0);
    const BSONObj sort = BSON("a" << 1);
    for (bool upsert : {false, true}) {
        for (bool multi : {false, true}) {
            auto rawUpdate = BSON("q" << query << "u" << update << "arrayFilters"
                                      << BSON_ARRAY(arrayFilter) << "multi" << multi << "upsert"
                                      << upsert << "collation" << collation << "sort" << sort);
            auto cmd = BSON("update" << nss.coll() << "updates" << BSON_ARRAY(rawUpdate));
            for (bool seq : {false, true}) {
                auto opMsgRequest = toOpMsg(nss.db_forTest(), cmd, seq);

                BulkWriteCommandRequest request;
                BulkWriteCommandModifier builder(&request);
                builder.addUpdate(opMsgRequest);
                builder.finishBuild();

                auto nsInfo = request.getNsInfo();
                ASSERT_EQ(1, nsInfo.size());
                ASSERT_EQ("TestDB", nsInfo[0].getNs().db_forTest());
                ASSERT_EQ("test", nsInfo[0].getNs().coll());
                ASSERT_EQ(boost::none, nsInfo[0].getShardVersion());

                ASSERT_EQ(1, request.getOps().size());
                auto op = BulkWriteCRUDOp(request.getOps()[0]);
                ASSERT_EQ(upsert, op.getUpdate()->getUpsert());
                ASSERT_EQ(multi, op.getUpdate()->getMulti());
                ASSERT_BSONOBJ_EQ(query, op.getUpdate()->getFilter());
                ASSERT_BSONOBJ_EQ(update, op.getUpdate()->getUpdateMods().getUpdateModifier());
                ASSERT_BSONOBJ_EQ(collation, op.getUpdate()->getCollation().value_or(BSONObj()));
                ASSERT_BSONOBJ_EQ(sort, op.getUpdate()->getSort().value_or(BSONObj()));
                ASSERT(op.getUpdate()->getArrayFilters());
                auto filter = (*op.getUpdate()->getArrayFilters())[0];
                ASSERT_BSONOBJ_EQ(arrayFilter, filter);
            }
        }
    }
}

TEST(BulkWriteCommandModifier, AddOpUpdate) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "test");
    const BSONObj query = BSON("x" << 1);
    const BSONObj update = BSON("$inc" << BSON("x" << 1));
    const BSONObj collation = BSON("locale"
                                   << "en_US");
    const BSONObj arrayFilter = BSON("i" << 0);
    const BSONObj sort = BSON("a" << 1);

    auto updateOp = write_ops::UpdateOpEntry();
    updateOp.setQ(query);
    updateOp.setU(update);
    updateOp.setCollation(collation);
    updateOp.setArrayFilters({{arrayFilter}});
    updateOp.setSort(sort);

    for (bool upsert : {false, true}) {
        for (bool multi : {false, true}) {
            auto req = write_ops::UpdateCommandRequest(nss);
            updateOp.setMulti(multi);
            updateOp.setUpsert(upsert);
            req.setUpdates({updateOp});

            BulkWriteCommandRequest request;
            BulkWriteCommandModifier builder(&request);
            builder.addOp(req);
            builder.finishBuild();

            auto nsInfo = request.getNsInfo();
            ASSERT_EQ(1, nsInfo.size());
            ASSERT_EQ("TestDB", nsInfo[0].getNs().db_forTest());
            ASSERT_EQ("test", nsInfo[0].getNs().coll());
            ASSERT_EQ(boost::none, nsInfo[0].getShardVersion());

            ASSERT_EQ(1, request.getOps().size());
            auto op = BulkWriteCRUDOp(request.getOps()[0]);
            ASSERT_EQ(upsert, op.getUpdate()->getUpsert());
            ASSERT_EQ(multi, op.getUpdate()->getMulti());
            ASSERT_BSONOBJ_EQ(query, op.getUpdate()->getFilter());
            ASSERT_BSONOBJ_EQ(update, op.getUpdate()->getUpdateMods().getUpdateModifier());
            ASSERT_BSONOBJ_EQ(collation, op.getUpdate()->getCollation().value_or(BSONObj()));
            ASSERT_BSONOBJ_EQ(sort, op.getUpdate()->getSort().value_or(BSONObj()));
            ASSERT(op.getUpdate()->getArrayFilters());
            auto filter = (*op.getUpdate()->getArrayFilters())[0];
            ASSERT_BSONOBJ_EQ(arrayFilter, filter);
        }
    }
}

TEST(BulkWriteCommandModifier, AddUpdateOps) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "test");
    const BSONObj query = BSON("x" << 1);
    const BSONObj update = BSON("$inc" << BSON("x" << 1));
    const BSONObj collation = BSON("locale"
                                   << "en_US");
    const BSONObj arrayFilter = BSON("i" << 0);
    const BSONObj sort = BSON("a" << 1);

    for (bool upsert : {false, true}) {
        for (bool multi : {false, true}) {
            BulkWriteCommandRequest request;
            BulkWriteCommandModifier builder(&request);
            builder.addUpdateOp(
                nss, query, update, upsert, multi, {{arrayFilter}}, sort, collation, boost::none);
            builder.finishBuild();

            auto nsInfo = request.getNsInfo();
            ASSERT_EQ(1, nsInfo.size());
            ASSERT_EQ("TestDB", nsInfo[0].getNs().db_forTest());
            ASSERT_EQ("test", nsInfo[0].getNs().coll());
            ASSERT_EQ(boost::none, nsInfo[0].getShardVersion());

            ASSERT_EQ(1, request.getOps().size());
            auto op = BulkWriteCRUDOp(request.getOps()[0]);
            ASSERT_EQ(upsert, op.getUpdate()->getUpsert());
            ASSERT_EQ(multi, op.getUpdate()->getMulti());
            ASSERT_BSONOBJ_EQ(query, op.getUpdate()->getFilter());
            ASSERT_BSONOBJ_EQ(update, op.getUpdate()->getUpdateMods().getUpdateModifier());
            ASSERT_BSONOBJ_EQ(collation, op.getUpdate()->getCollation().value_or(BSONObj()));
            ASSERT_BSONOBJ_EQ(sort, op.getUpdate()->getSort().value_or(BSONObj()));
            ASSERT(op.getUpdate()->getArrayFilters());
            auto filter = (*op.getUpdate()->getArrayFilters())[0];
            ASSERT_BSONOBJ_EQ(arrayFilter, filter);
        }
    }
}

TEST(CommandWriteOpsParsers, BulkWriteUpdateWithPipeline) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "test");
    const BSONObj query = BSON("q" << BSON("x" << 1));
    std::vector<BSONObj> pipeline{BSON("$addFields" << BSON("x" << 1))};
    const BSONObj update = BSON("u" << pipeline);
    for (bool upsert : {false, true}) {
        for (bool multi : {false, true}) {

            BulkWriteCommandRequest request;
            BulkWriteCommandModifier builder(&request);
            builder.addPipelineUpdateOps(nss, query, pipeline, upsert, multi);
            builder.finishBuild();

            auto nsInfo = request.getNsInfo();
            ASSERT_EQ(1, nsInfo.size());
            ASSERT_EQ("TestDB", nsInfo[0].getNs().db_forTest());
            ASSERT_EQ("test", nsInfo[0].getNs().coll());
            ASSERT_EQ(boost::none, nsInfo[0].getShardVersion());

            ASSERT_EQ(1, request.getOps().size());
            auto op = BulkWriteCRUDOp(request.getOps()[0]);
            ASSERT_EQ(upsert, op.getUpdate()->getUpsert());
            ASSERT_EQ(multi, op.getUpdate()->getMulti());
            ASSERT_BSONOBJ_EQ(query, op.getUpdate()->getFilter());
            ASSERT_BSONOBJ_EQ(pipeline[0], op.getUpdate()->getUpdateMods().getUpdatePipeline()[0]);
        }
    }
}

TEST(BulkWriteCommandModifier, AddDelete) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "test");
    const BSONObj query = BSON("x" << 1);
    const BSONObj collation = BSON("locale"
                                   << "en_US");
    for (bool multi : {false, true}) {
        auto rawDelete =
            BSON("q" << query << "limit" << (multi ? 0 : 1) << "collation" << collation);
        auto cmd = BSON("delete" << nss.coll() << "deletes" << BSON_ARRAY(rawDelete));
        for (bool seq : {false, true}) {
            auto opMsgRequest = toOpMsg(nss.db_forTest(), cmd, seq);


            BulkWriteCommandRequest request;
            BulkWriteCommandModifier builder(&request);
            builder.addDelete(opMsgRequest);
            builder.finishBuild();

            auto nsInfo = request.getNsInfo();
            ASSERT_EQ(1, nsInfo.size());
            ASSERT_EQ("TestDB", nsInfo[0].getNs().db_forTest());
            ASSERT_EQ("test", nsInfo[0].getNs().coll());
            ASSERT_EQ(boost::none, nsInfo[0].getShardVersion());

            ASSERT_EQ(1, request.getOps().size());
            auto op = BulkWriteCRUDOp(request.getOps()[0]);
            ASSERT_EQ(multi, op.getDelete()->getMulti());
            ASSERT_BSONOBJ_EQ(query, op.getDelete()->getFilter());
            ASSERT_BSONOBJ_EQ(collation, op.getDelete()->getCollation().value_or(BSONObj()));
        }
    }
}

TEST(BulkWriteCommandModifier, AddOpDelete) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "test");
    const BSONObj query = BSON("x" << 1);
    const BSONObj collation = BSON("locale"
                                   << "en_US");

    auto delOp = write_ops::DeleteOpEntry();
    delOp.setCollation(collation);
    delOp.setQ(query);
    for (bool multi : {false, true}) {
        auto delReq = write_ops::DeleteCommandRequest(nss);
        delOp.setMulti(multi);
        delReq.setDeletes({delOp});

        BulkWriteCommandRequest request;
        BulkWriteCommandModifier builder(&request);
        builder.addOp(delReq);
        builder.finishBuild();

        auto nsInfo = request.getNsInfo();
        ASSERT_EQ(1, nsInfo.size());
        ASSERT_EQ("TestDB", nsInfo[0].getNs().db_forTest());
        ASSERT_EQ("test", nsInfo[0].getNs().coll());
        ASSERT_EQ(boost::none, nsInfo[0].getShardVersion());

        ASSERT_EQ(1, request.getOps().size());
        auto op = BulkWriteCRUDOp(request.getOps()[0]);
        ASSERT_EQ(multi, op.getDelete()->getMulti());
        ASSERT_BSONOBJ_EQ(query, op.getDelete()->getFilter());
        ASSERT_BSONOBJ_EQ(collation, op.getDelete()->getCollation().value_or(BSONObj()));
    }
}

// Add delete ops
TEST(BulkWriteCommandModifier, AddDeleteOps) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "test");
    const BSONObj query = BSON("x" << 1);
    const BSONObj collation = BSON("locale"
                                   << "en_US");
    for (bool multi : {false, true}) {
        BulkWriteCommandRequest request;
        BulkWriteCommandModifier builder(&request);
        builder.addDeleteOp(nss, query, multi, collation, boost::none);
        builder.finishBuild();

        auto nsInfo = request.getNsInfo();
        ASSERT_EQ(1, nsInfo.size());
        ASSERT_EQ("TestDB", nsInfo[0].getNs().db_forTest());
        ASSERT_EQ("test", nsInfo[0].getNs().coll());
        ASSERT_EQ(boost::none, nsInfo[0].getShardVersion());

        ASSERT_EQ(1, request.getOps().size());
        auto op = BulkWriteCRUDOp(request.getOps()[0]);
        ASSERT_EQ(multi, op.getDelete()->getMulti());
        ASSERT_BSONOBJ_EQ(query, op.getDelete()->getFilter());
        ASSERT_BSONOBJ_EQ(collation, op.getDelete()->getCollation().value_or(BSONObj()));
    }
}

TEST(BulkWriteCommandModifier, TestMultiOpsSameNs) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "test");
    auto docs = std::vector<BSONObj>();
    docs.emplace_back(BSON("a" << 1));
    docs.emplace_back(BSON("b" << 1));

    const BSONObj query = BSON("x" << 1);
    const BSONObj collation = BSON("locale"
                                   << "en_US");

    BulkWriteCommandRequest request;
    BulkWriteCommandModifier builder(&request);
    builder.addInsertOps(nss, docs);
    builder.addDeleteOp(nss, query, true, collation, boost::none);
    builder.finishBuild();

    auto nsInfo = request.getNsInfo();
    ASSERT_EQ(1, nsInfo.size());
    ASSERT_EQ("TestDB", nsInfo[0].getNs().db_forTest());
    ASSERT_EQ("test", nsInfo[0].getNs().coll());
    ASSERT_EQ(boost::none, nsInfo[0].getShardVersion());

    ASSERT_EQ(3, request.getOps().size());
    {
        auto op = BulkWriteCRUDOp(request.getOps()[0]);
        ASSERT_EQ(BulkWriteCRUDOp::kInsert, op.getType());
    }
    {
        auto op = BulkWriteCRUDOp(request.getOps()[1]);
        ASSERT_EQ(BulkWriteCRUDOp::kInsert, op.getType());
    }
    {
        auto op = BulkWriteCRUDOp(request.getOps()[2]);
        ASSERT_EQ(BulkWriteCRUDOp::kDelete, op.getType());
        ASSERT_EQ(true, op.getDelete()->getMulti());
        ASSERT_BSONOBJ_EQ(query, op.getDelete()->getFilter());
        ASSERT_BSONOBJ_EQ(collation, op.getDelete()->getCollation().value_or(BSONObj()));
    }
}

// Multiple ops (different types) different namespaces
TEST(BulkWriteCommandModifier, TestMultiOpsDifferentNs) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "test");
    auto nss2 = NamespaceString::createNamespaceString_forTest("TestDB", "test1");
    auto docs = std::vector<BSONObj>();
    docs.emplace_back(BSON("a" << 1));
    docs.emplace_back(BSON("b" << 1));

    const BSONObj query = BSON("x" << 1);
    const BSONObj collation = BSON("locale"
                                   << "en_US");

    BulkWriteCommandRequest request;
    BulkWriteCommandModifier builder(&request);
    builder.addInsertOps(nss, docs);
    builder.addDeleteOp(nss2, query, true, collation, boost::none);
    builder.finishBuild();

    auto nsInfo = request.getNsInfo();
    ASSERT_EQ(2, nsInfo.size());
    ASSERT_EQ("TestDB", nsInfo[0].getNs().db_forTest());
    ASSERT_EQ("test", nsInfo[0].getNs().coll());
    ASSERT_EQ("TestDB", nsInfo[1].getNs().db_forTest());
    ASSERT_EQ("test1", nsInfo[1].getNs().coll());
    ASSERT_EQ(boost::none, nsInfo[0].getShardVersion());

    ASSERT_EQ(3, request.getOps().size());
    {
        auto op = BulkWriteCRUDOp(request.getOps()[0]);
        ASSERT_EQ(BulkWriteCRUDOp::kInsert, op.getType());
        ASSERT_EQ(0, op.getNsInfoIdx());
    }
    {
        auto op = BulkWriteCRUDOp(request.getOps()[1]);
        ASSERT_EQ(BulkWriteCRUDOp::kInsert, op.getType());
        ASSERT_EQ(0, op.getNsInfoIdx());
    }
    {
        auto op = BulkWriteCRUDOp(request.getOps()[2]);
        ASSERT_EQ(BulkWriteCRUDOp::kDelete, op.getType());
        ASSERT_EQ(1, op.getNsInfoIdx());
        ASSERT_EQ(true, op.getDelete()->getMulti());
        ASSERT_BSONOBJ_EQ(query, op.getDelete()->getFilter());
        ASSERT_BSONOBJ_EQ(collation, op.getDelete()->getCollation().value_or(BSONObj()));
    }
}

}  // namespace
}  // namespace mongo
