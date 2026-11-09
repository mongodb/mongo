// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/replicated_record_ids_util.h"

#include "mongo/db/namespace_string.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ReplicatedRecordIdsInScope, PlainReplicatedCollection) {
    const auto nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    ASSERT_TRUE(isReplicatedRecordIdsInScope(nss, false));
}

TEST(ReplicatedRecordIdsInScope, ClusteredCollection) {
    // Clustered collections are out of scope, since their RecordId is derived from the '_id'.
    const auto nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    ASSERT_FALSE(isReplicatedRecordIdsInScope(nss, true));
}

TEST(ReplicatedRecordIdsInScope, ReplicatedSystemCollection) {
    ASSERT_TRUE(isReplicatedRecordIdsInScope(
        NamespaceString::createNamespaceString_forTest("admin", "system.version"), false));
}

TEST(ReplicatedRecordIdsInScope, NonReplicatedNamespaces) {
    // The local database and system.profile collections are not replicated.
    ASSERT_FALSE(isReplicatedRecordIdsInScope(NamespaceString::kRsOplogNamespace, false));
    ASSERT_FALSE(isReplicatedRecordIdsInScope(
        NamespaceString::createNamespaceString_forTest("test", "system.profile"), false));
}

TEST(ReplicatedRecordIdsInScope, ImplicitlyReplicatedNamespaces) {
    // Implicitly replicated namespaces are out of scope, since their replicated writes carry no
    // 'rid'.
    for (auto&& nss : {NamespaceString::kSessionTransactionsTableNamespace,
                       NamespaceString::kConfigImagesNamespace,
                       NamespaceString::kChangeStreamPreImagesNamespace}) {
        ASSERT_TRUE(nss.isReplicated()) << nss.toStringForErrorMsg();
        ASSERT_TRUE(nss.isImplicitlyReplicated()) << nss.toStringForErrorMsg();
        ASSERT_FALSE(isReplicatedRecordIdsInScope(nss, false)) << nss.toStringForErrorMsg();
        ASSERT_FALSE(isReplicatedRecordIdsInScope(nss, true)) << nss.toStringForErrorMsg();
    }
}

TEST(ReplicatedRecordIdsInScope, OtherConfigCollections) {
    ASSERT_TRUE(isReplicatedRecordIdsInScope(
        NamespaceString::createNamespaceString_forTest("config", "settings"), false));
}

}  // namespace
}  // namespace mongo
