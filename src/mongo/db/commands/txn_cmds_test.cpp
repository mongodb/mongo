// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * IDL-parse-time validation tests for the 'commitTransaction' oplog object. These exercise only
 * the IDL parser generated from src/mongo/db/commands/txn_cmds.idl -- no opCtx, no catalog, no
 * fixture.
 */

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(CommitTransactionOplogObjectParseTest, ParsesWithoutEndOfTransactionScopesField) {
    auto obj = BSON("commitTransaction" << 1 << "count" << 5LL);
    auto parsed =
        CommitTransactionOplogObject::parse(obj, IDLParserContext("commitTransactionOplogEntry"));
    ASSERT_FALSE(parsed.getEndOfTransactionScopes().has_value());
}

TEST(CommitTransactionOplogObjectParseTest, ParsesWithEmptyEndOfTransactionScopesField) {
    auto obj = BSON("commitTransaction" << 1 << "endOfTransactionScopes" << BSONObj());
    auto parsed =
        CommitTransactionOplogObject::parse(obj, IDLParserContext("commitTransactionOplogEntry"));
    ASSERT_TRUE(parsed.getEndOfTransactionScopes().has_value());
}

TEST(CommitTransactionOplogObjectParseTest,
     IgnoresArbitrarySubFieldsInEndOfTransactionScopesField) {
    auto scopes = BSON("someFutureScope" << "collectionA" << "anotherFutureField" << 42
                                         << "nestedFutureField" << BSON("x" << 1 << "y" << 2));
    auto obj = BSON("commitTransaction" << 1 << "endOfTransactionScopes" << scopes);
    auto parsed =
        CommitTransactionOplogObject::parse(obj, IDLParserContext("commitTransactionOplogEntry"));
    ASSERT_TRUE(parsed.getEndOfTransactionScopes().has_value());
}

}  // namespace
}  // namespace mongo
