// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index_builds/index_build_test_helpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/protocol.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/string_map.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace CommandTests {

TEST(CommandTests, InputDocumentSequeceWorksEndToEnd) {
    const auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "doc_seq");
    DBDirectClient db(opCtx);
    db.dropCollection(nss);
    ASSERT_EQ(db.count(nss), 0u);

    OpMsgRequest request;
    request.body = BSON("insert" << nss.coll() << "$db" << nss.db_forTest());
    request.sequences = {{"documents",
                          {
                              BSON("_id" << 1),
                              BSON("_id" << 2),
                              BSON("_id" << 3),
                              BSON("_id" << 4),
                              BSON("_id" << 5),
                          }}};

    const auto reply = db.runCommand(std::move(request));
    ASSERT_EQ(int(reply->getProtocol()), int(rpc::Protocol::kOpMsg));
    ASSERT_BSONOBJ_EQ(reply->getCommandReply(), BSON("n" << 5 << "ok" << 1.0));
    ASSERT_EQ(db.count(nss), 5u);
}

using std::string;

/**
 * Default suite base, unless otherwise overridden in test specific namespace.
 */
class Base {
public:
    Base() : db(&_opCtx) {
        db.dropCollection(nss());
    }

    NamespaceString nss() {
        return NamespaceString::createNamespaceString_forTest("test.testCollection");
    }
    DatabaseName nsDb() {
        return DatabaseName::createDatabaseName_forTest(boost::none, "test");
    }
    const char* nsColl() {
        return "testCollection";
    }

    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;
    DBDirectClient db;
};

// one namespace per command
namespace FileMD5 {
struct Base {
    Base() : db(&_opCtx) {
        db.dropCollection(nss());
        ASSERT_OK(createIndex(&_opCtx, nss().ns_forTest(), BSON("files_id" << 1 << "n" << 1)));
    }

    NamespaceString nss() {
        return NamespaceString::createNamespaceString_forTest("test.fs.chunks");
    }

    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;
    DBDirectClient db;
};
struct Type0 : Base {
    void run() {
        {
            BSONObjBuilder b;
            b.genOID();
            b.append("files_id", 0);
            b.append("n", 0);
            b.appendBinData("data", 6, BinDataGeneral, "hello ");
            db.insert(nss(), b.obj());
        }
        {
            BSONObjBuilder b;
            b.genOID();
            b.append("files_id", 0);
            b.append("n", 1);
            b.appendBinData("data", 5, BinDataGeneral, "world");
            db.insert(nss(), b.obj());
        }

        BSONObj result;
        ASSERT(db.runCommand(DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                             BSON("filemd5" << 0),
                             result));
        ASSERT_EQUALS(string("5eb63bbbe01eeed093cb22bb8f5acdc3"), result.getStringField("md5"));
    }
};
struct Type2 : Base {
    void run() {
        {
            BSONObjBuilder b;
            b.genOID();
            b.append("files_id", 0);
            b.append("n", 0);
            b.appendBinDataArrayDeprecated("data", "hello ", 6);
            db.insert(nss(), b.obj());
        }
        {
            BSONObjBuilder b;
            b.genOID();
            b.append("files_id", 0);
            b.append("n", 1);
            b.appendBinDataArrayDeprecated("data", "world", 5);
            db.insert(nss(), b.obj());
        }

        BSONObj result;
        ASSERT(db.runCommand(DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                             BSON("filemd5" << 0),
                             result));
        ASSERT_EQUALS(string("5eb63bbbe01eeed093cb22bb8f5acdc3"), result.getStringField("md5"));
    }
};
}  // namespace FileMD5

namespace SymbolArgument {
// SERVER-16260
// The Ruby driver expects server commands to accept the Symbol BSON type as a collection name.
// This is a historical quirk that we shall support until corrected versions of the Ruby driver
// can be distributed. Retain these tests until MongoDB 3.0

class Drop : Base {
public:
    void run() {
        ASSERT(db.createCollection(nss()));
        {
            BSONObjBuilder cmd;
            cmd.appendSymbol("drop", nsColl());  // Use Symbol for SERVER-16260

            BSONObj result;
            bool ok = db.runCommand(nsDb(), cmd.obj(), result);
            LOGV2(24181, "{result_jsonString}", "result_jsonString"_attr = result.jsonString());
            ASSERT(ok);
        }
    }
};

class DropIndexes : Base {
public:
    void run() {
        ASSERT(db.createCollection(nss()));

        BSONObjBuilder cmd;
        cmd.appendSymbol("dropIndexes", nsColl());  // Use Symbol for SERVER-16260
        cmd.append("index", "*");

        BSONObj result;
        bool ok = db.runCommand(nsDb(), cmd.obj(), result);
        LOGV2(24182, "{result_jsonString}", "result_jsonString"_attr = result.jsonString());
        ASSERT(ok);
    }
};

class CreateIndexWithNoKey : Base {
public:
    void run() {
        ASSERT(db.createCollection(nss()));

        BSONObjBuilder indexSpec;

        BSONArrayBuilder indexes;
        indexes.append(indexSpec.obj());

        BSONObjBuilder cmd;
        cmd.append("createIndexes", nsColl());
        cmd.append("indexes", indexes.arr());

        BSONObj result;
        bool ok = db.runCommand(nsDb(), cmd.obj(), result);
        LOGV2(24183, "{result_jsonString}", "result_jsonString"_attr = result.jsonString());
        ASSERT(!ok);
    }
};

class CreateIndexWithDuplicateKey : Base {
public:
    void run() {
        ASSERT(db.createCollection(nss()));

        BSONObjBuilder indexSpec;
        indexSpec.append("key", BSON("a" << 1 << "a" << 1 << "b" << 1));

        BSONArrayBuilder indexes;
        indexes.append(indexSpec.obj());

        BSONObjBuilder cmd;
        cmd.append("createIndexes", nsColl());
        cmd.append("indexes", indexes.arr());

        BSONObj result;
        bool ok = db.runCommand(nsDb(), cmd.obj(), result);
        LOGV2(24184, "{result_jsonString}", "result_jsonString"_attr = result.jsonString());
        ASSERT(!ok);
    }
};


class CreateIndexWithEmptyStringAsValue : Base {
public:
    void run() {
        ASSERT(db.createCollection(nss()));

        BSONObjBuilder indexSpec;
        indexSpec.append("key", BSON("a" << ""));

        BSONArrayBuilder indexes;
        indexes.append(indexSpec.obj());

        BSONObjBuilder cmd;
        cmd.append("createIndexes", nsColl());
        cmd.append("indexes", indexes.arr());

        BSONObj result;
        bool ok = db.runCommand(nsDb(), cmd.obj(), result);
        LOGV2(24185, "{result_jsonString}", "result_jsonString"_attr = result.jsonString());
        ASSERT(!ok);
    }
};

class FindAndModify : Base {
public:
    void run() {
        ASSERT(db.createCollection(nss()));
        {
            BSONObjBuilder b;
            b.genOID();
            b.append("name", "Tom");
            b.append("rating", 0);
            db.insert(nss(), b.obj());
        }

        BSONObjBuilder cmd;
        cmd.appendSymbol("findAndModify", nsColl());  // Use Symbol for SERVER-16260
        cmd.append("update", BSON("$inc" << BSON("score" << 1)));
        cmd.append("new", true);

        BSONObj result;
        bool ok = db.runCommand(nsDb(), cmd.obj(), result);
        LOGV2(24186, "{result_jsonString}", "result_jsonString"_attr = result.jsonString());
        ASSERT(ok);
        // TODO(kangas) test that Tom's score is 1
    }
};
}  // namespace SymbolArgument

/**
 * Tests that the 'rolesInfo' command does not return duplicate field names.
 */
class RolesInfoShouldNotReturnDuplicateFieldNames : Base {
public:
    void run() {
        BSONObj result;
        bool ok = db.runCommand(nsDb(), BSON("rolesInfo" << 1), result);
        ASSERT(ok);

        StringSet observedFields;
        for (const auto& field : result) {
            ASSERT(observedFields.find(field.fieldNameStringData()) == observedFields.end());
            observedFields.insert(field);
        }
    }
};

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("commands") {}

    void setupTests() override {
        add<FileMD5::Type0>();
        add<FileMD5::Type2>();
        add<FileMD5::Type2>();
        add<SymbolArgument::DropIndexes>();
        add<SymbolArgument::FindAndModify>();
        add<SymbolArgument::Drop>();
        add<SymbolArgument::CreateIndexWithNoKey>();
        add<SymbolArgument::CreateIndexWithDuplicateKey>();
        add<SymbolArgument::CreateIndexWithEmptyStringAsValue>();
        add<RolesInfoShouldNotReturnDuplicateFieldNames>();
    }
};

unittest::OldStyleSuiteInitializer<All> all;

}  // namespace CommandTests
}  // namespace mongo
