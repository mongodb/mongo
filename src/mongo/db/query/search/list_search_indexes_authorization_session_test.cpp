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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/oid.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session_test_fixture.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/search/document_source_list_search_indexes.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {
using ListSearchIndexesAuthTest = AuthorizationSessionTestFixture;

const TenantId kTenantId1(OID("12345678901234567890aaaa"));
const NamespaceString nss =
    NamespaceString::createNamespaceString_forTest(kTenantId1, "test", "foo");

TEST_F(ListSearchIndexesAuthTest, CanAggregateListSearchIndexesWithSearchIndexesAction) {
    auto rsrc = ResourcePattern::forDatabaseName(nss.dbName());

    authzSession->assumePrivilegesForDB(Privilege(rsrc, ActionType::listSearchIndexes),
                                        nss.dbName());

    BSONArray pipeline = BSON_ARRAY(BSON("$listSearchIndexes" << BSONObj()));
    auto aggReq = buildAggReq(nss, pipeline);
    PrivilegeVector privileges =
        uassertStatusOK(auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq, false));
    ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(ListSearchIndexesAuthTest, CannotAggregateListSearchIndexesWithoutSearchIndexesAction) {
    auto rsrc = ResourcePattern::forDatabaseName(nss.dbName());

    authzSession->assumePrivilegesForDB(Privilege(rsrc, ActionType::find), nss.dbName());

    BSONArray pipeline = BSON_ARRAY(BSON("$listSearchIndexes" << BSONObj()));
    auto aggReq = buildAggReq(nss, pipeline);
    PrivilegeVector privileges =
        uassertStatusOK(auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq, false));
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
}

}  // namespace
}  // namespace mongo
