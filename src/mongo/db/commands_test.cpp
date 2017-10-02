/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/db/commands.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/platform/basic.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(Commands, appendCommandStatusOK) {
    BSONObjBuilder actualResult;
    Command::appendCommandStatus(actualResult, Status::OK());

    BSONObjBuilder expectedResult;
    expectedResult.append("ok", 1.0);

    ASSERT_BSONOBJ_EQ(actualResult.obj(), expectedResult.obj());
}

TEST(Commands, appendCommandStatusError) {
    BSONObjBuilder actualResult;
    const Status status(ErrorCodes::InvalidLength, "Response payload too long");
    Command::appendCommandStatus(actualResult, status);

    BSONObjBuilder expectedResult;
    expectedResult.append("ok", 0.0);
    expectedResult.append("errmsg", status.reason());
    expectedResult.append("code", status.code());
    expectedResult.append("codeName", ErrorCodes::errorString(status.code()));

    ASSERT_BSONOBJ_EQ(actualResult.obj(), expectedResult.obj());
}

TEST(Commands, appendCommandStatusNoOverwrite) {
    BSONObjBuilder actualResult;
    actualResult.append("a", "b");
    actualResult.append("c", "d");
    actualResult.append("ok", "not ok");
    const Status status(ErrorCodes::InvalidLength, "Response payload too long");
    Command::appendCommandStatus(actualResult, status);

    BSONObjBuilder expectedResult;
    expectedResult.append("a", "b");
    expectedResult.append("c", "d");
    expectedResult.append("ok", "not ok");
    expectedResult.append("errmsg", status.reason());
    expectedResult.append("code", status.code());
    expectedResult.append("codeName", ErrorCodes::errorString(status.code()));

    ASSERT_BSONOBJ_EQ(actualResult.obj(), expectedResult.obj());
}

class ParseNsOrUUID : public unittest::Test {
public:
    ParseNsOrUUID()
        : client(service.makeClient("test")),
          opCtxPtr(client->makeOperationContext()),
          opCtx(opCtxPtr.get()) {}
    ServiceContextNoop service;
    ServiceContext::UniqueClient client;
    ServiceContext::UniqueOperationContext opCtxPtr;
    OperationContext* opCtx;
};

TEST_F(ParseNsOrUUID, FailWrongType) {
    auto cmd = BSON("query" << BSON("a" << BSON("$gte" << 11)));
    ASSERT_THROWS_CODE(
        Command::parseNsOrUUID(opCtx, "db", cmd), DBException, ErrorCodes::InvalidNamespace);
}

TEST_F(ParseNsOrUUID, FailEmptyDbName) {
    auto cmd = BSON("query"
                    << "coll");
    ASSERT_THROWS_CODE(
        Command::parseNsOrUUID(opCtx, "", cmd), DBException, ErrorCodes::InvalidNamespace);
}

TEST_F(ParseNsOrUUID, FailInvalidDbName) {
    auto cmd = BSON("query"
                    << "coll");
    ASSERT_THROWS_CODE(
        Command::parseNsOrUUID(opCtx, "test.coll", cmd), DBException, ErrorCodes::InvalidNamespace);
}

TEST_F(ParseNsOrUUID, ParseUnknownUUID) {
    auto cmd = BSON("query" << UUID::gen());
    ASSERT_THROWS_CODE(Command::parseNsOrUUID(opCtx, "test.coll", cmd),
                       DBException,
                       ErrorCodes::NamespaceNotFound);
}

TEST_F(ParseNsOrUUID, ParseValidColl) {
    auto cmd = BSON("query"
                    << "coll");
    auto parsedNss = Command::parseNsOrUUID(opCtx, "test", cmd);
    ASSERT_EQ(parsedNss, NamespaceString("test.coll"));
}

TEST_F(ParseNsOrUUID, ParseValidUUID) {
    // Register a UUID/Collection pair in the UUIDCatalog.
    const CollectionUUID uuid = UUID::gen();
    const NamespaceString nss("test.coll");
    Collection coll(stdx::make_unique<CollectionMock>(nss));
    UUIDCatalog& catalog = UUIDCatalog::get(opCtx);
    catalog.onCreateCollection(opCtx, &coll, uuid);

    auto cmd = BSON("query" << uuid);
    auto parsedNss = Command::parseNsOrUUID(opCtx, "test", cmd);
    ASSERT_EQUALS(nss, parsedNss);
}
}  // namespace mongo
