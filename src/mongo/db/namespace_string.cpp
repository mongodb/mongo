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

#include "mongo/db/namespace_string.h"

#include <boost/optional.hpp>

namespace mongo {
namespace {

constexpr auto listCollectionsCursorCol = "$cmd.listCollections"_sd;
constexpr auto bulkWriteCursorCol = "$cmd.bulkWrite"_sd;
constexpr auto collectionlessShardsvrParticipantBlockCollection =
    "$cmd.shardsvrParticipantBlock"_sd;
constexpr auto dropPendingNSPrefix = "system.drop."_sd;

constexpr auto fle2Prefix = "enxcol_."_sd;
constexpr auto fle2EscSuffix = ".esc"_sd;
constexpr auto fle2EcocSuffix = ".ecoc"_sd;
constexpr auto fle2EcocCompactSuffix = ".ecoc.compact"_sd;

// The following are namespaces in the form of config.xxx for which only one instance exist globally
// within the cluster.
static const absl::flat_hash_set<NamespaceString> globallyUniqueConfigDbCollections = {
    NamespaceString::kConfigsvrCollectionsNamespace,
    NamespaceString::kConfigsvrChunksNamespace,
    NamespaceString::kConfigDatabasesNamespace,
    NamespaceString::kConfigsvrShardsNamespace,
    NamespaceString::kConfigsvrPlacementHistoryNamespace,
    NamespaceString::kConfigChangelogNamespace,
    NamespaceString::kConfigsvrTagsNamespace,
    NamespaceString::kConfigVersionNamespace,
    NamespaceString::kConfigMongosNamespace,
    NamespaceString::kLogicalSessionsNamespace};

}  // namespace

bool NamespaceString::isListCollectionsCursorNS() const {
    return coll() == listCollectionsCursorCol;
}

bool NamespaceString::isCollectionlessShardsvrParticipantBlockNS() const {
    return coll() == collectionlessShardsvrParticipantBlockCollection;
}

bool NamespaceString::isCollectionlessAggregateNS() const {
    return coll() == kCollectionlessAggregateCollection;
}

bool NamespaceString::isLegalClientSystemNS() const {
    auto collectionName = coll();
    if (isAdminDB()) {
        if (collectionName == "system.roles")
            return true;
        if (collectionName == kServerConfigurationNamespace.coll())
            return true;
        if (collectionName == kKeysCollectionNamespace.coll())
            return true;
        if (collectionName == "system.backup_users")
            return true;
        if (collectionName == "system.new_users")
            return true;
    } else if (isConfigDB()) {
        if (collectionName == "system.sessions")
            return true;
        if (collectionName == kIndexBuildEntryNamespace.coll())
            return true;
        if (collectionName.find(".system.resharding.") != std::string::npos)
            return true;
        if (collectionName == kShardingDDLCoordinatorsNamespace.coll())
            return true;
        if (collectionName == kConfigsvrCoordinatorsNamespace.coll())
            return true;
        if (collectionName == kBlockFCVChangesNamespace.coll())
            return true;
    } else if (isLocalDB()) {
        if (collectionName == kSystemReplSetNamespace.coll())
            return true;
        if (collectionName == kLocalHealthLogNamespace.coll())
            return true;
        if (collectionName == kConfigsvrRestoreNamespace.coll())
            return true;
    }

    if (collectionName == "system.users")
        return true;
    if (collectionName == "system.js")
        return true;
    if (collectionName == kSystemDotViewsCollectionName)
        return true;
    if (isTemporaryReshardingCollection()) {
        return true;
    }
    if (isTimeseriesBucketsCollection() &&
        validCollectionName(collectionName.substr(kTimeseriesBucketsCollectionPrefix.size()))) {
        return true;
    }
    if (isChangeStreamPreImagesCollection()) {
        return true;
    }

    if (isChangeCollection()) {
        return true;
    }

    if (isSystemStatsCollection()) {
        return true;
    }

    return false;
}

/**
 * Oplog entries on 'system.views' should also be processed one at a time. View catalog immediately
 * reflects changes for each oplog entry so we can see inconsistent view catalog if multiple oplog
 * entries on 'system.views' are being applied out of the original order.
 *
 * Process updates to 'admin.system.version' individually as well so the secondary's FCV when
 * processing each operation matches the primary's when committing that operation.
 *
 * Oplog entries on 'config.shards' should be processed one at a time, otherwise the in-memory state
 * that its kept on the TopologyTimeTicker might be wrong.
 *
 */
bool NamespaceString::mustBeAppliedInOwnOplogBatch() const {
    auto ns = this->ns();
    return isSystemDotViews() || isServerConfigurationCollection() || isPrivilegeCollection() ||
        ns == kDonorReshardingOperationsNamespace.ns() ||
        ns == kForceOplogBatchBoundaryNamespace.ns() || ns == kConfigsvrShardsNamespace.ns();
}

NamespaceString NamespaceString::makeBulkWriteNSS(const boost::optional<TenantId>& tenantId) {
    return NamespaceString(tenantId, DatabaseName::kAdmin.db(omitTenant), bulkWriteCursorCol);
}

NamespaceString NamespaceString::makeClusterParametersNSS(
    const boost::optional<TenantId>& tenantId) {
    return tenantId
        ? NamespaceString(tenantId, DatabaseName::kConfig.db(omitTenant), "clusterParameters")
        : kClusterParametersNamespace;
}

NamespaceString NamespaceString::makeSystemDotViewsNamespace(const DatabaseName& dbName) {
    return NamespaceString(dbName, kSystemDotViewsCollectionName);
}

NamespaceString NamespaceString::makeSystemDotProfileNamespace(const DatabaseName& dbName) {
    return NamespaceString(dbName, kSystemDotProfileCollectionName);
}

NamespaceString NamespaceString::makeListCollectionsNSS(const DatabaseName& dbName) {
    NamespaceString nss(dbName, listCollectionsCursorCol);
    dassert(nss.isValid());
    dassert(nss.isListCollectionsCursorNS());
    return nss;
}

NamespaceString NamespaceString::makeCollectionlessShardsvrParticipantBlockNSS(
    const DatabaseName& dbName) {
    NamespaceString nss(dbName, collectionlessShardsvrParticipantBlockCollection);
    dassert(nss.isValid());
    dassert(nss.isCollectionlessShardsvrParticipantBlockNS());
    return nss;
}

NamespaceString NamespaceString::makeGlobalConfigCollection(StringData collName) {
    return NamespaceString(DatabaseName::kConfig, collName);
}

NamespaceString NamespaceString::makeLocalCollection(StringData collName) {
    return NamespaceString(DatabaseName::kLocal, collName);
}

NamespaceString NamespaceString::makeCollectionlessAggregateNSS(const DatabaseName& dbName) {
    NamespaceString nss(dbName, kCollectionlessAggregateCollection);
    dassert(nss.isValid());
    dassert(nss.isCollectionlessAggregateNS());
    return nss;
}

NamespaceString NamespaceString::makeChangeCollectionNSS(
    const boost::optional<TenantId>& tenantId) {
    return NamespaceString{tenantId, DatabaseName::kConfig.db(omitTenant), kChangeCollectionName};
}

NamespaceString NamespaceString::makeReshardingLocalOplogBufferNSS(
    const UUID& existingUUID, const std::string& donorShardId) {
    return NamespaceString(DatabaseName::kConfig,
                           "localReshardingOplogBuffer." + existingUUID.toString() + "." +
                               donorShardId);
}

NamespaceString NamespaceString::makeReshardingLocalConflictStashNSS(
    const UUID& existingUUID, const std::string& donorShardId) {
    return NamespaceString(DatabaseName::kConfig,
                           "localReshardingConflictStash." + existingUUID.toString() + "." +
                               donorShardId);
}

NamespaceString NamespaceString::makeTenantUsersCollection(
    const boost::optional<TenantId>& tenantId) {
    return NamespaceString(
        tenantId, DatabaseName::kAdmin.db(omitTenant), NamespaceString::kSystemUsers);
}

NamespaceString NamespaceString::makeTenantRolesCollection(
    const boost::optional<TenantId>& tenantId) {
    return NamespaceString(
        tenantId, DatabaseName::kAdmin.db(omitTenant), NamespaceString::kSystemRoles);
}

NamespaceString NamespaceString::makeCommandNamespace(const DatabaseName& dbName) {
    return NamespaceString(dbName, "$cmd");
}

std::string NamespaceString::getSisterNS(StringData local) const {
    MONGO_verify(local.size() && local[0] != '.');
    return std::string{db_deprecated()} + "." + std::string{local};
}

void NamespaceString::serializeCollectionName(BSONObjBuilder* builder, StringData fieldName) const {
    if (isCollectionlessAggregateNS()) {
        builder->append(fieldName, 1);
    } else {
        builder->append(fieldName, coll());
    }
}

bool NamespaceString::isNamespaceAlwaysUntracked() const {
    // Local and admin never have sharded collections
    if (isLocalDB() || isAdminDB())
        return true;

    // Config can only have the system.sessions as sharded
    if (isConfigDB())
        return *this != NamespaceString::kLogicalSessionsNamespace;

    if (isSystem()) {
        // Only some system collections (<DB>.system.<COLL>) can be sharded,
        // all the others are always unsharded.
        // This list does not contain 'config.system.sessions' because we already check it above
        return !isTemporaryReshardingCollection() && !isTimeseriesBucketsCollection();
    }

    return false;
}

bool NamespaceString::isShardLocalNamespace() const {
    if (isLocalDB() || isAdminDB()) {
        return true;
    }

    if (isConfigDB()) {
        return !globallyUniqueConfigDbCollections.contains(*this);
    }

    if (isSystem()) {
        // Only some db.system.xxx collections are cluster global.
        const bool isUniqueInstanceSystemCollection =
            isTemporaryReshardingCollection() || isTimeseriesBucketsCollection();
        return !isUniqueInstanceSystemCollection;
    }

    return false;
}

bool NamespaceString::isConfigDotCacheDotChunks() const {
    return db_deprecated() == "config" && coll().starts_with("cache.chunks.");
}

bool NamespaceString::isReshardingLocalOplogBufferCollection() const {
    return db_deprecated() == "config" && coll().starts_with(kReshardingLocalOplogBufferPrefix);
}

bool NamespaceString::isReshardingConflictStashCollection() const {
    return db_deprecated() == "config" && coll().starts_with(kReshardingConflictStashPrefix);
}

bool NamespaceString::isTemporaryReshardingCollection() const {
    return coll().starts_with(kTemporaryTimeseriesReshardingCollectionPrefix) ||
        coll().starts_with(kTemporaryReshardingCollectionPrefix);
}

// TODO SERVER-101784: Remove this once 9.0 is LTS and viewful time-series collections no longer
// exist.
bool NamespaceString::isTimeseriesBucketsCollection() const {
    return coll().starts_with(kTimeseriesBucketsCollectionPrefix);
}

bool NamespaceString::isChangeStreamPreImagesCollection() const {
    return ns() == kChangeStreamPreImagesNamespace.ns();
}

bool NamespaceString::isChangeCollection() const {
    return isConfigDB() && coll() == kChangeCollectionName;
}

bool NamespaceString::isConfigImagesCollection() const {
    return ns() == kConfigImagesNamespace.ns();
}

bool NamespaceString::isConfigTransactionsCollection() const {
    return ns() == kSessionTransactionsTableNamespace.ns();
}

bool NamespaceString::isFLE2StateCollection() const {
    return coll().starts_with(fle2Prefix) &&
        (coll().ends_with(fle2EscSuffix) || coll().ends_with(fle2EcocSuffix) ||
         coll().ends_with(fle2EcocCompactSuffix));
}

bool NamespaceString::isFLE2StateCollection(StringData coll) {
    return coll.starts_with(fle2Prefix) &&
        (coll.ends_with(fle2EscSuffix) || coll.ends_with(fle2EcocSuffix));
}

bool NamespaceString::isOplogOrChangeCollection() const {
    return isOplog() || isChangeCollection();
}

bool NamespaceString::isSystemStatsCollection() const {
    return coll().starts_with(kStatisticsCollectionPrefix);
}

bool NamespaceString::isOutTmpBucketsCollection() const {
    return isTimeseriesBucketsCollection() &&
        getTimeseriesViewNamespace().coll().starts_with(kOutTmpCollectionPrefix);
}

// TODO SERVER-101784: Remove this once 9.0 is LTS and viewful time-series collections no longer
// exist.
NamespaceString NamespaceString::makeTimeseriesBucketsNamespace() const {
    return {dbName(), std::string{kTimeseriesBucketsCollectionPrefix} + coll()};
}

// TODO SERVER-101784: Remove this once 9.0 is LTS and viewful time-series collections no longer
// exist.
NamespaceString NamespaceString::getTimeseriesViewNamespace() const {
    invariant(isTimeseriesBucketsCollection(), ns());
    return {dbName(), coll().substr(kTimeseriesBucketsCollectionPrefix.size())};
}

bool NamespaceString::isImplicitlyReplicated() const {
    if (db_deprecated() == DatabaseName::kConfig.db(omitTenant)) {
        if (isChangeStreamPreImagesCollection() || isConfigImagesCollection() ||
            isChangeCollection()) {
            // Implicitly replicated namespaces are replicated, although they only replicate a
            // subset of writes.
            invariant(isReplicated());
            return true;
        }
    }

    return false;
}

bool NamespaceString::isReplicated() const {
    if (isLocalDB()) {
        return false;
    }

    // Of collections not in the `local` database, only `system` collections might not be
    // replicated.
    if (!isSystem()) {
        return true;
    }

    if (isSystemDotProfile()) {
        return false;
    }

    // E.g: `system.version` is replicated.
    return true;
}

std::string NamespaceStringOrUUID::toStringForErrorMsg() const {
    if (isNamespaceString()) {
        return nss().toStringForErrorMsg();
    }

    return uuid().toString();
}

std::string toStringForLogging(const NamespaceStringOrUUID& nssOrUUID) {
    if (nssOrUUID.isNamespaceString()) {
        return toStringForLogging(nssOrUUID.nss());
    }

    return nssOrUUID.uuid().toString();
}

void NamespaceStringOrUUID::serialize(BSONObjBuilder* builder, StringData fieldName) const {
    if (const NamespaceString* nss = get_if<NamespaceString>(&_nssOrUUID)) {
        builder->append(fieldName, nss->coll());
    } else {
        get<1>(get<UUIDWithDbName>(_nssOrUUID)).appendToBuilder(builder, fieldName);
    }
}

}  // namespace mongo
