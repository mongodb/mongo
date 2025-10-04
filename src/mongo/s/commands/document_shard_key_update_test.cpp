/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/commands/document_shard_key_update_util.h"
#include "mongo/unittest/unittest.h"

#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

using namespace documentShardKeyUpdateUtil;

class DocumentShardKeyUpdateTest : public unittest::Test {
public:
    DocumentShardKeyUpdateTest() {}
};

TEST_F(DocumentShardKeyUpdateTest, constructShardKeyDeleteCmdObj) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.foo");
    static const BSONObj kUpdatePreImage =
        BSON("x" << 4 << "y" << 3 << "z" << BSON("a" << 2 << "b" << 1) << "_id" << 20);

    for (auto shouldUpsert : {true, false}) {
        auto deleteCmdObj = constructShardKeyDeleteCmdObj(nss, kUpdatePreImage, shouldUpsert);

        auto deletesObj = deleteCmdObj["deletes"].Array();
        ASSERT_EQ(deletesObj.size(), 1U);

        auto predicate = deletesObj[0]["q"].Obj();
        ASSERT_BSONOBJ_EQ(kUpdatePreImage, predicate);

        ASSERT_EQ(deleteCmdObj["delete"].String(), nss.coll());
    }
}

TEST_F(DocumentShardKeyUpdateTest, constructShardKeyDeleteCmdObjWithDollarFields) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.foo");
    static const BSONObj kUpdatePreImage = BSON(
        "_id" << 1 << "array" << BSON_ARRAY(2 << BSON("$alpha" << 3)) << "obj" << BSON("$beta" << 4)
              << "obj2" << BSON("$charlie" << 5 << "$delta" << BSON("$foxtrot" << 6)) << "$golf"
              << 7 << "$hotel" << BSON("$india" << BSON("$juliett" << 9)) << "obj3"
              << BSON("subobj" << BSON("$kilo" << 10)) << "$mike" << BSON_ARRAY(11 << 12));

    for (auto shouldUpsert : {true, false}) {
        auto deleteCmdObj = constructShardKeyDeleteCmdObj(nss, kUpdatePreImage, shouldUpsert);

        auto deletesObj = deleteCmdObj["deletes"].Array();
        ASSERT_EQ(deletesObj.size(), 1U);

        static const BSONObj kPredicateFromDocument = BSON(
            "_id"
            << 1 << "array" << BSON_ARRAY(2 << BSON("$alpha" << 3)) << "obj"
            << BSON("$eq" << BSON("$beta" << 4)) << "obj2"
            << BSON("$eq" << BSON("$charlie" << 5 << "$delta" << BSON("$foxtrot" << 6))) << "obj3"
            << BSON("subobj" << BSON("$kilo" << 10)) << "$expr"
            << BSON("$and" << BSON_ARRAY(
                        BSON("$eq" << BSON_ARRAY(
                                 BSON("$getField"
                                      << BSON("input" << "$$ROOT"
                                                      << "field" << BSON("$literal" << "$golf")))
                                 << BSON("$literal" << 7)))
                        << BSON("$eq" << BSON_ARRAY(
                                    BSON("$getField" << BSON(
                                             "input" << "$$ROOT"
                                                     << "field" << BSON("$literal" << "$hotel")))
                                    << BSON("$literal" << BSON("$india" << BSON("$juliett" << 9)))))
                        << BSON("$eq" << BSON_ARRAY(
                                    BSON("$getField"
                                         << BSON("input" << "$$ROOT"
                                                         << "field" << BSON("$literal" << "$mike")))
                                    << BSON("$literal" << BSON_ARRAY(11 << 12)))))));

        auto predicate = deletesObj[0]["q"].Obj();
        if (shouldUpsert) {
            ASSERT_BSONOBJ_EQ(kUpdatePreImage, predicate);
        } else {
            ASSERT_BSONOBJ_EQ(kPredicateFromDocument, predicate);
        }

        ASSERT_EQ(deleteCmdObj["delete"].String(), nss.coll());
    }
}

TEST_F(DocumentShardKeyUpdateTest, constructShardKeyInsertCmdObj) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.foo");
    BSONObj updatePostImage = BSON("x" << 4 << "y" << 3 << "_id" << 20);

    auto insertCmdObj = constructShardKeyInsertCmdObj(nss, updatePostImage, false);

    auto insertsObj = insertCmdObj["documents"].Array();
    ASSERT_EQ(insertsObj.size(), 1U);

    auto insert = insertsObj[0];
    ASSERT_EQ(insert["x"].Int(), 4);
    ASSERT_EQ(insert["y"].Int(), 3);
    ASSERT_EQ(insert["_id"].Int(), 20);

    ASSERT_EQ(insertCmdObj["insert"].String(), nss.coll());
}
}  // namespace
}  // namespace mongo
