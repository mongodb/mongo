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

/**
 * Unit tests of the ParsedPrivilege class.
 */

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/parsed_privilege_gen.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/database_name.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::auth {
namespace {
using namespace std::literals::string_view_literals;
constexpr auto kActions = "actions"sv;
constexpr auto kResource = "resource"sv;
const BSONObj kClusterResource = BSON("cluster"sv << true);
const BSONArray kFindActions = BSON_ARRAY("find"sv);

TEST(PrivilegeParserTest, IsNotValidTest) {
    IDLParserContext ctx("IsNotValidTest");

    // must have resource
    const BSONObj noRsrc = BSON(kActions << kFindActions);
    constexpr auto noRsrcExpect =
        "BSON field 'IsNotValidTest.resource' is missing but a required field"sv;
    ASSERT_THROWS_CODE_AND_WHAT(ParsedPrivilege::parse(noRsrc, ctx),
                                DBException,
                                ErrorCodes::IDLFailedToParse,
                                noRsrcExpect);

    // must have actions
    const BSONObj noActions = BSON(kResource << kClusterResource);
    constexpr auto noActionsExpect =
        "BSON field 'IsNotValidTest.actions' is missing but a required field"sv;
    ASSERT_THROWS_CODE_AND_WHAT(ParsedPrivilege::parse(noActions, ctx),
                                DBException,
                                ErrorCodes::IDLFailedToParse,
                                noActionsExpect);
}

Privilege resolvePrivilege(BSONObj obj, std::vector<std::string>* unrecognized = nullptr) {
    IDLParserContext ctx("resolvePrivilege");
    auto pp = ParsedPrivilege::parse(obj, ctx);
    return Privilege::resolvePrivilegeWithTenant(boost::none /* tenantId */, pp, unrecognized);
}

const std::set<std::string_view> kBoolResourceTypes = {
    "cluster"sv,
    "anyResource"sv,
};

const std::set<std::string_view> kAllResourceTypes = {
    "cluster"sv,
    "anyResource"sv,
    "db"sv,
    "collection"sv,
    "system_buckets"sv,
};

BSONObj makeResource(const boost::optional<std::string_view>& db,
                     const boost::optional<std::string_view>& collection,
                     const boost::optional<std::string_view>& system_buckets) {
    BSONObjBuilder builder;
    if (db) {
        builder.append("db"sv, db.get());
    }
    if (collection) {
        builder.append("collection"sv, collection.get());
    }
    if (system_buckets) {
        builder.append("system_buckets"sv, system_buckets.get());
    }
    return builder.obj();
}

TEST(PrivilegeParserTest, CombiningTypesNegative) {
    // resource can't have cluster or anyResource with other keys
    for (std::string_view primary : {"cluster"sv, "anyResource"sv}) {
        for (std::string_view secondary : kAllResourceTypes) {
            if (primary == secondary) {
                continue;
            }

            BSONObjBuilder builder;
            {
                BSONObjBuilder rsrcBuilder(builder.subobjStart("resource"sv));
                rsrcBuilder.append(primary, true);
                if (kBoolResourceTypes.count(secondary) > 0) {
                    rsrcBuilder.append(secondary, true);
                } else {
                    rsrcBuilder.append(secondary, "foo"sv);
                }
                rsrcBuilder.doneFast();
            }
            builder.append("actions"sv, kFindActions);

            if (secondary == "cluster"sv) {
                // Error messages treat cluster as always primary.
                std::swap(primary, secondary);
            }
            const std::string expect = str::stream()
                << "resource: {" << primary << ": true} conflicts with resource type '" << secondary
                << "'";

            ASSERT_THROWS_CODE_AND_WHAT(
                resolvePrivilege(builder.obj()), DBException, ErrorCodes::BadValue, expect);
        }
    }

    // collection and system_buckets may not co-exist
    ASSERT_THROWS_CODE_AND_WHAT(
        resolvePrivilege(BSON("resource"sv << makeResource("db"sv, "coll"sv, "bucket"sv)
                                           << "actions"sv << kFindActions)),
        DBException,
        ErrorCodes::BadValue,
        "resource: {collection: '...'} conflicts with resource type 'system_buckets'");

    // db requires collection (or system_buckets)
    ASSERT_THROWS_CODE_AND_WHAT(
        resolvePrivilege(BSON("resource"sv << makeResource("db"sv, boost::none, boost::none)
                                           << "actions"sv << kFindActions)),
        DBException,
        ErrorCodes::BadValue,
        "resource pattern must contain 'collection' or 'systemBuckets' specifier");

    // resource can't have collection without db
    ASSERT_THROWS_CODE_AND_WHAT(
        resolvePrivilege(BSON("resource"sv << makeResource(boost::none, "coll"sv, boost::none)
                                           << "actions"sv << kFindActions)),
        DBException,
        ErrorCodes::BadValue,
        "resource {collection: '...'} must include 'db' field as well");
}

TEST(PrivilegeParserTest, IsValidTest) {
    // Works with cluster resource
    auto clusterPriv =
        resolvePrivilege(BSON("resource"sv << kClusterResource << "actions"sv << kFindActions));
    ASSERT_TRUE(clusterPriv.getResourcePattern().isClusterResourcePattern());

    // Works with anyResource resource
    auto anyResourcePriv = resolvePrivilege(
        BSON("resource"sv << BSON("anyResource"sv << true) << "actions"sv << kFindActions));
    ASSERT_TRUE(anyResourcePriv.getResourcePattern().isAnyResourcePattern());

    // Works with wildcard db and resource
    auto anyNormalPriv = resolvePrivilege(
        BSON("resource"sv << makeResource(""sv, ""sv, boost::none) << "actions"sv << kFindActions));
    ASSERT_TRUE(anyNormalPriv.getResourcePattern().isAnyNormalResourcePattern());

    // Works with real db and collection
    auto exactNSSPriv =
        resolvePrivilege(BSON("resource"sv << makeResource("db1"sv, "coll"sv, boost::none)
                                           << "actions"sv << kFindActions));
    ASSERT_TRUE(exactNSSPriv.getResourcePattern().isExactNamespacePattern());
    ASSERT_EQ(exactNSSPriv.getResourcePattern().dbNameToMatch().toString_forTest(), "db1"sv);
    ASSERT_EQ(exactNSSPriv.getResourcePattern().collectionToMatch(), "coll"sv);

    // Works with any bucket in any db (implicit)
    auto anyBucketImplicit =
        resolvePrivilege(BSON("resource"sv << makeResource(boost::none, boost::none, ""sv)
                                           << "actions"sv << kFindActions));
    ASSERT_TRUE(anyBucketImplicit.getResourcePattern().isAnySystemBucketsCollection());
    ASSERT_EQ(anyBucketImplicit.getResourcePattern().dbNameToMatch().toString_forTest(), ""sv);
    ASSERT_EQ(anyBucketImplicit.getResourcePattern().collectionToMatch(), ""sv);

    // Works with any bucket in any db (explicit)
    auto anyBucketExplicit = resolvePrivilege(
        BSON("resource"sv << makeResource(""sv, boost::none, ""sv) << "actions"sv << kFindActions));
    ASSERT_TRUE(anyBucketExplicit.getResourcePattern().isAnySystemBucketsCollection());
    ASSERT_EQ(anyBucketExplicit.getResourcePattern().dbNameToMatch().toString_forTest(), ""sv);
    ASSERT_EQ(anyBucketExplicit.getResourcePattern().collectionToMatch(), ""sv);

    // Works with system_buckets in any db (implicit)
    auto bucketAnyDBImplicit =
        resolvePrivilege(BSON("resource"sv << makeResource(boost::none, boost::none, "bucket"sv)
                                           << "actions"sv << kFindActions));
    ASSERT_TRUE(bucketAnyDBImplicit.getResourcePattern().isAnySystemBucketsCollectionInAnyDB());
    ASSERT_EQ(bucketAnyDBImplicit.getResourcePattern().dbNameToMatch().toString_forTest(), ""sv);
    ASSERT_EQ(bucketAnyDBImplicit.getResourcePattern().collectionToMatch(), "bucket"sv);

    // Works with system_buckets in any db (explicit)
    auto bucketAnyDBExplicit =
        resolvePrivilege(BSON("resource"sv << makeResource(""sv, boost::none, "bucket"sv)
                                           << "actions"sv << kFindActions));
    ASSERT_TRUE(bucketAnyDBExplicit.getResourcePattern().isAnySystemBucketsCollectionInAnyDB());
    ASSERT_EQ(bucketAnyDBExplicit.getResourcePattern().dbNameToMatch().toString_forTest(), ""sv);
    ASSERT_EQ(bucketAnyDBExplicit.getResourcePattern().collectionToMatch(), "bucket"sv);

    // Works with any system_bucket in specific db
    auto bucketInDB = resolvePrivilege(BSON("resource"sv << makeResource("db1"sv, boost::none, ""sv)
                                                         << "actions"sv << kFindActions));
    ASSERT_TRUE(bucketInDB.getResourcePattern().isAnySystemBucketsCollectionInDB());
    ASSERT_EQ(bucketInDB.getResourcePattern().dbNameToMatch().toString_forTest(), "db1"sv);
    ASSERT_EQ(bucketInDB.getResourcePattern().collectionToMatch(), ""sv);

    // Works with exact system buckets namespace.
    auto exactBucket =
        resolvePrivilege(BSON("resource"sv << makeResource("db1"sv, boost::none, "bucket"sv)
                                           << "actions"sv << kFindActions));
    ASSERT_TRUE(exactBucket.getResourcePattern().isExactSystemBucketsCollection());
    ASSERT_EQ(exactBucket.getResourcePattern().dbNameToMatch().toString_forTest(), "db1"sv);
    ASSERT_EQ(exactBucket.getResourcePattern().collectionToMatch(), "bucket"sv);
}

TEST(PrivilegeParserTest, RoundTrip) {
    const std::vector<BSONObj> resourcePatterns = {
        BSON("cluster"sv << true),
        BSON("anyResource"sv << true),
        BSON("db"sv << ""
                    << "collection"sv
                    << ""),
        BSON("db"sv << ""
                    << "collection"sv
                    << "coll1"),
        BSON("db"sv << "db1"
                    << "collection"sv
                    << ""),
        BSON("db"sv << "db1"
                    << "collection"sv
                    << "coll1"),
        BSON("system_buckets"sv << "bucket"sv),
        BSON("db"sv << "db1"
                    << "system_buckets"sv
                    << "bucket"sv),
    };
    const std::vector<BSONArray> actionTypes = {
        BSON_ARRAY("find"sv),
        BSON_ARRAY("anyAction"sv),
        BSON_ARRAY("find"sv << "insert"sv
                            << "remove"sv
                            << "update"sv),
    };

    for (const auto& pattern : resourcePatterns) {
        for (const auto& actions : actionTypes) {
            auto obj = BSON("resource"sv << pattern << "actions"sv << actions);
            auto priv = resolvePrivilege(obj);
            auto serialized = priv.toBSON();
            ASSERT_BSONOBJ_EQ(obj, serialized);
        }
    }
}

TEST(PrivilegeParserTest, ParseInvalidActionsTest) {
    auto obj = BSON("resource"sv << kClusterResource << "actions"sv
                                 << BSON_ARRAY("find"sv << "fakeAction"sv));
    std::vector<std::string> unrecognized;
    auto priv = resolvePrivilege(obj, &unrecognized);

    ASSERT_TRUE(priv.getResourcePattern().isClusterResourcePattern());
    ASSERT_TRUE(priv.getActions().contains(ActionType::find));
    ASSERT_FALSE(priv.getActions().contains(ActionType::insert));
    ASSERT_EQUALS(1U, unrecognized.size());
    ASSERT_EQUALS("fakeAction", unrecognized[0]);
}

}  // namespace
}  // namespace mongo::auth
