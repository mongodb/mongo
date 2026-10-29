// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


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
