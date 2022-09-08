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

#include "mongo/platform/basic.h"

#include "mongo/db/namespace_string.h"

#include <ostream>

#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

constexpr auto listCollectionsCursorCol = "$cmd.listCollections"_sd;
constexpr auto collectionlessAggregateCursorCol = "$cmd.aggregate"_sd;
constexpr auto dropPendingNSPrefix = "system.drop."_sd;

constexpr auto fle2Prefix = "enxcol_."_sd;
constexpr auto fle2EscSuffix = ".esc"_sd;
constexpr auto fle2EccSuffix = ".ecc"_sd;
constexpr auto fle2EcocSuffix = ".ecoc"_sd;

}  // namespace

constexpr StringData NamespaceString::kAdminDb;
constexpr StringData NamespaceString::kLocalDb;
constexpr StringData NamespaceString::kConfigDb;
constexpr StringData NamespaceString::kSystemDotViewsCollectionName;
constexpr StringData NamespaceString::kSystemDotJavascriptCollectionName;
constexpr StringData NamespaceString::kOrphanCollectionPrefix;
constexpr StringData NamespaceString::kOrphanCollectionDb;

const NamespaceString NamespaceString::kServerConfigurationNamespace(NamespaceString::kAdminDb,
                                                                     "system.version");
const NamespaceString NamespaceString::kLogicalSessionsNamespace(NamespaceString::kConfigDb,
                                                                 "system.sessions");

const NamespaceString NamespaceString::kConfigDatabasesNamespace(NamespaceString::kConfigDb,
                                                                 "databases");

// Persisted state for a shard participating in a transaction or retryable write.
const NamespaceString NamespaceString::kSessionTransactionsTableNamespace(
    NamespaceString::kConfigDb, "transactions");

// Persisted state for a shard coordinating a cross-shard transaction.
const NamespaceString NamespaceString::kTransactionCoordinatorsNamespace(
    NamespaceString::kConfigDb, "transaction_coordinators");

const NamespaceString NamespaceString::kConfigsvrRestoreNamespace(NamespaceString::kLocalDb,
                                                                  "system.collections_to_restore");

const NamespaceString NamespaceString::kMigrationCoordinatorsNamespace(NamespaceString::kConfigDb,
                                                                       "migrationCoordinators");

const NamespaceString NamespaceString::kMigrationRecipientsNamespace(NamespaceString::kConfigDb,
                                                                     "migrationRecipients");

const NamespaceString NamespaceString::kTenantMigrationDonorsNamespace(NamespaceString::kConfigDb,
                                                                       "tenantMigrationDonors");

const NamespaceString NamespaceString::kTenantMigrationRecipientsNamespace(
    NamespaceString::kConfigDb, "tenantMigrationRecipients");

const NamespaceString NamespaceString::kTenantMigrationOplogView(
    NamespaceString::kLocalDb, "system.tenantMigration.oplogView");

const NamespaceString NamespaceString::kShardSplitDonorsNamespace(NamespaceString::kConfigDb,
                                                                  "shardSplitDonors");

const NamespaceString NamespaceString::kShardConfigCollectionsNamespace(NamespaceString::kConfigDb,
                                                                        "cache.collections");
const NamespaceString NamespaceString::kShardConfigDatabasesNamespace(NamespaceString::kConfigDb,
                                                                      "cache.databases");
const NamespaceString NamespaceString::kKeysCollectionNamespace(NamespaceString::kAdminDb,
                                                                "system.keys");
const NamespaceString NamespaceString::kExternalKeysCollectionNamespace(NamespaceString::kConfigDb,
                                                                        "external_validation_keys");
const NamespaceString NamespaceString::kRsOplogNamespace(NamespaceString::kLocalDb, "oplog.rs");
const NamespaceString NamespaceString::kSystemReplSetNamespace(NamespaceString::kLocalDb,
                                                               "system.replset");
const NamespaceString NamespaceString::kLastVoteNamespace(NamespaceString::kLocalDb,
                                                          "replset.election");
const NamespaceString NamespaceString::kChangeStreamPreImagesNamespace(NamespaceString::kConfigDb,
                                                                       "system.preimages");
const NamespaceString NamespaceString::kIndexBuildEntryNamespace(NamespaceString::kConfigDb,
                                                                 "system.indexBuilds");
const NamespaceString NamespaceString::kRangeDeletionNamespace(NamespaceString::kConfigDb,
                                                               "rangeDeletions");
const NamespaceString NamespaceString::kRangeDeletionForRenameNamespace(NamespaceString::kConfigDb,
                                                                        "rangeDeletionsForRename");
const NamespaceString NamespaceString::kConfigReshardingOperationsNamespace(
    NamespaceString::kConfigDb, "reshardingOperations");

const NamespaceString NamespaceString::kDonorReshardingOperationsNamespace(
    NamespaceString::kConfigDb, "localReshardingOperations.donor");

const NamespaceString NamespaceString::kRecipientReshardingOperationsNamespace(
    NamespaceString::kConfigDb, "localReshardingOperations.recipient");

const NamespaceString NamespaceString::kShardingDDLCoordinatorsNamespace(
    NamespaceString::kConfigDb, "system.sharding_ddl_coordinators");

const NamespaceString NamespaceString::kShardingRenameParticipantsNamespace(
    NamespaceString::kConfigDb, "localRenameParticipants");

const NamespaceString NamespaceString::kConfigSettingsNamespace(NamespaceString::kConfigDb,
                                                                "settings");

const NamespaceString NamespaceString::kVectorClockNamespace(NamespaceString::kConfigDb,
                                                             "vectorClock");

const NamespaceString NamespaceString::kReshardingApplierProgressNamespace(
    NamespaceString::kConfigDb, "localReshardingOperations.recipient.progress_applier");

const NamespaceString NamespaceString::kReshardingTxnClonerProgressNamespace(
    NamespaceString::kConfigDb, "localReshardingOperations.recipient.progress_txn_cloner");

const NamespaceString NamespaceString::kCollectionCriticalSectionsNamespace(
    NamespaceString::kConfigDb, "collection_critical_sections");

const NamespaceString NamespaceString::kForceOplogBatchBoundaryNamespace(
    NamespaceString::kConfigDb, "system.forceOplogBatchBoundary");

const NamespaceString NamespaceString::kConfigImagesNamespace(NamespaceString::kConfigDb,
                                                              "image_collection");

const NamespaceString NamespaceString::kConfigsvrCoordinatorsNamespace(
    NamespaceString::kConfigDb, "sharding_configsvr_coordinators");

const NamespaceString NamespaceString::kUserWritesCriticalSectionsNamespace(
    NamespaceString::kConfigDb, "user_writes_critical_sections");

const NamespaceString NamespaceString::kCompactStructuredEncryptionCoordinatorNamespace(
    NamespaceString::kConfigDb, "compact_structured_encryption_coordinator");

const NamespaceString NamespaceString::kClusterParametersNamespace(NamespaceString::kConfigDb,
                                                                   "clusterParameters");

const NamespaceString NamespaceString::kConfigsvrShardsNamespace(NamespaceString::kConfigDb,
                                                                 "shards");

const NamespaceString NamespaceString::kConfigsvrCollectionsNamespace(NamespaceString::kConfigDb,
                                                                      "collections");

const NamespaceString NamespaceString::kConfigsvrIndexCatalogNamespace(NamespaceString::kConfigDb,
                                                                       "csrs.indexes");

const NamespaceString NamespaceString::kShardIndexCatalogNamespace(NamespaceString::kConfigDb,
                                                                   "shard.indexes");

const NamespaceString NamespaceString::kShardCollectionCatalogNamespace(NamespaceString::kConfigDb,
                                                                        "shard.collections");

const NamespaceString NamespaceString::kConfigsvrPlacementHistoryNamespace(
    NamespaceString::kConfigDb, "placementHistory");

const NamespaceString NamespaceString::kLockpingsNamespace(NamespaceString::kConfigDb, "lockpings");
const NamespaceString NamespaceString::kDistLocksNamepsace(NamespaceString::kConfigDb, "locks");

const NamespaceString NamespaceString::kSetChangeStreamStateCoordinatorNamespace(
    NamespaceString::kConfigDb, "change_stream_coordinator");

const NamespaceString NamespaceString::kGlobalIndexClonerNamespace(
    NamespaceString::kConfigDb, "localGlobalIndexOperations.cloner");

NamespaceString NamespaceString::parseFromStringExpectTenantIdInMultitenancyMode(StringData ns) {
    if (!gMultitenancySupport) {
        return NamespaceString(ns, boost::none);
    }

    auto tenantDelim = ns.find('_');
    auto collDelim = ns.find('.');
    // If the first '_' is after the '.' that separates the db and coll names, the '_' is part
    // of the coll name and is not a db prefix.
    if (tenantDelim == std::string::npos || collDelim < tenantDelim) {
        return NamespaceString(ns, boost::none);
    }

    const TenantId tenantId(OID(ns.substr(0, tenantDelim)));
    return NamespaceString(ns.substr(tenantDelim + 1, ns.size() - 1 - tenantDelim), tenantId);
}

bool NamespaceString::isListCollectionsCursorNS() const {
    return coll() == listCollectionsCursorCol;
}

bool NamespaceString::isCollectionlessAggregateNS() const {
    return coll() == collectionlessAggregateCursorCol;
}

bool NamespaceString::isLegalClientSystemNS(
    const ServerGlobalParams::FeatureCompatibility& currentFCV) const {
    auto dbname = dbName().db();

    NamespaceString parsedNSS;
    if (gMultitenancySupport && !tenantId()) {
        // TODO (SERVER-67423) Remove support for mangled dbname in isLegalClientSystemNS check
        // Transitional support for accepting tenantId as a mangled database name.
        try {
            parsedNSS = parseFromStringExpectTenantIdInMultitenancyMode(ns());
            if (parsedNSS.tenantId()) {
                dbname = parsedNSS.dbName().db();
            }
        } catch (const DBException&) {
            // Swallow exception.
        }
    }

    if (dbname == kAdminDb) {
        if (coll() == "system.roles")
            return true;
        if (coll() == kServerConfigurationNamespace.coll())
            return true;
        if (coll() == kKeysCollectionNamespace.coll())
            return true;
        if (coll() == "system.backup_users")
            return true;
    } else if (dbname == kConfigDb) {
        if (coll() == "system.sessions")
            return true;
        if (coll() == kIndexBuildEntryNamespace.coll())
            return true;
        if (coll().find(".system.resharding.") != std::string::npos)
            return true;
        if (coll() == kShardingDDLCoordinatorsNamespace.coll())
            return true;
        if (coll() == kConfigsvrCoordinatorsNamespace.coll())
            return true;
    } else if (dbname == kLocalDb) {
        if (coll() == kSystemReplSetNamespace.coll())
            return true;
        if (coll() == "system.healthlog")
            return true;
        if (coll() == kConfigsvrRestoreNamespace.coll())
            return true;
    }

    if (coll() == "system.users")
        return true;
    if (coll() == "system.js")
        return true;
    if (coll() == kSystemDotViewsCollectionName)
        return true;
    if (isTemporaryReshardingCollection()) {
        return true;
    }
    if (isTimeseriesBucketsCollection() &&
        validCollectionName(coll().substr(kTimeseriesBucketsCollectionPrefix.size()))) {
        return true;
    }
    if (isChangeStreamPreImagesCollection()) {
        return true;
    }

    if (isChangeCollection()) {
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
 * Process updates to 'config.tenantMigrationRecipients' individually so they serialize after
 * inserts into 'config.donatedFiles.<migrationId>'.
 *
 * Oplog entries on 'config.shards' should be processed one at a time, otherwise the in-memory state
 * that its kept on the TopologyTimeTicker might be wrong.
 *
 * Serialize updates to 'config.tenantMigrationDonors' and 'config.shardSplitDonors' to avoid races
 * with creating tenant access blockers on secondaries.
 */
bool NamespaceString::mustBeAppliedInOwnOplogBatch() const {
    return isSystemDotViews() || isServerConfigurationCollection() || isPrivilegeCollection() ||
        _ns == kDonorReshardingOperationsNamespace.ns() ||
        _ns == kForceOplogBatchBoundaryNamespace.ns() ||
        _ns == kTenantMigrationDonorsNamespace.ns() ||
        _ns == kTenantMigrationRecipientsNamespace.ns() || _ns == kShardSplitDonorsNamespace.ns() ||
        _ns == kConfigsvrShardsNamespace.ns();
}

NamespaceString NamespaceString::makeClusterParametersNSS(
    const boost::optional<TenantId>& tenantId) {
    return tenantId ? NamespaceString(tenantId, kConfigDb, "clusterParameters")
                    : kClusterParametersNamespace;
}

NamespaceString NamespaceString::makeListCollectionsNSS(const DatabaseName& dbName) {
    NamespaceString nss(dbName, listCollectionsCursorCol);
    dassert(nss.isValid());
    dassert(nss.isListCollectionsCursorNS());
    return nss;
}

NamespaceString NamespaceString::makeCollectionlessAggregateNSS(const DatabaseName& dbName) {
    NamespaceString nss(dbName, collectionlessAggregateCursorCol);
    dassert(nss.isValid());
    dassert(nss.isCollectionlessAggregateNS());
    return nss;
}

NamespaceString NamespaceString::makeChangeCollectionNSS(
    const boost::optional<TenantId>& tenantId) {
    // TODO: SERVER-65950 create namespace for a particular tenant.
    return NamespaceString{NamespaceString::kConfigDb, NamespaceString::kChangeCollectionName};
}

NamespaceString NamespaceString::makeGlobalIndexNSS(const UUID& id) {
    return NamespaceString(
        kSystemDb,
        fmt::format("{}{}", NamespaceString::kGlobalIndexCollectionPrefix, id.toString()));
}

NamespaceString NamespaceString::makePreImageCollectionNSS(
    const boost::optional<TenantId>& tenantId) {
    return tenantId ? NamespaceString(tenantId, kConfigDb, "system.preimages")
                    : kChangeStreamPreImagesNamespace;
}

std::string NamespaceString::getSisterNS(StringData local) const {
    verify(local.size() && local[0] != '.');
    return db().toString() + "." + local.toString();
}

void NamespaceString::serializeCollectionName(BSONObjBuilder* builder, StringData fieldName) const {
    if (isCollectionlessAggregateNS()) {
        builder->append(fieldName, 1);
    } else {
        builder->append(fieldName, coll());
    }
}

bool NamespaceString::isDropPendingNamespace() const {
    return coll().startsWith(dropPendingNSPrefix);
}

NamespaceString NamespaceString::makeDropPendingNamespace(const repl::OpTime& opTime) const {
    StringBuilder ss;
    ss << db() << "." << dropPendingNSPrefix;
    ss << opTime.getSecs() << "i" << opTime.getTimestamp().getInc() << "t" << opTime.getTerm();
    ss << "." << coll();
    return NamespaceString(ss.stringData());
}

StatusWith<repl::OpTime> NamespaceString::getDropPendingNamespaceOpTime() const {
    if (!isDropPendingNamespace()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Not a drop-pending namespace: " << _ns);
    }

    auto collectionName = coll();
    auto opTimeBeginIndex = dropPendingNSPrefix.size();
    auto opTimeEndIndex = collectionName.find('.', opTimeBeginIndex);
    auto opTimeStr = std::string::npos == opTimeEndIndex
        ? collectionName.substr(opTimeBeginIndex)
        : collectionName.substr(opTimeBeginIndex, opTimeEndIndex - opTimeBeginIndex);

    auto incrementSeparatorIndex = opTimeStr.find('i');
    if (std::string::npos == incrementSeparatorIndex) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Missing 'i' separator in drop-pending namespace: " << _ns);
    }

    auto termSeparatorIndex = opTimeStr.find('t', incrementSeparatorIndex);
    if (std::string::npos == termSeparatorIndex) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Missing 't' separator in drop-pending namespace: " << _ns);
    }

    long long seconds;
    auto status = NumberParser{}(opTimeStr.substr(0, incrementSeparatorIndex), &seconds);
    if (!status.isOK()) {
        return status.withContext(
            str::stream() << "Invalid timestamp seconds in drop-pending namespace: " << _ns);
    }

    unsigned int increment;
    status = NumberParser{}(opTimeStr.substr(incrementSeparatorIndex + 1,
                                             termSeparatorIndex - (incrementSeparatorIndex + 1)),
                            &increment);
    if (!status.isOK()) {
        return status.withContext(
            str::stream() << "Invalid timestamp increment in drop-pending namespace: " << _ns);
    }

    long long term;
    status = mongo::NumberParser{}(opTimeStr.substr(termSeparatorIndex + 1), &term);
    if (!status.isOK()) {
        return status.withContext(str::stream()
                                  << "Invalid term in drop-pending namespace: " << _ns);
    }

    return repl::OpTime(Timestamp(Seconds(seconds), increment), term);
}

bool NamespaceString::isNamespaceAlwaysUnsharded() const {
    // Local and admin never have sharded collections
    if (db() == NamespaceString::kLocalDb || db() == NamespaceString::kAdminDb)
        return true;

    // Config can only have the system.sessions as sharded
    if (db() == NamespaceString::kConfigDb)
        return *this != NamespaceString::kLogicalSessionsNamespace;

    if (isSystemDotProfile())
        return true;

    if (isSystemDotViews())
        return true;

    return false;
}

bool NamespaceString::isConfigDotCacheDotChunks() const {
    return db() == "config" && coll().startsWith("cache.chunks.");
}

bool NamespaceString::isReshardingLocalOplogBufferCollection() const {
    return db() == "config" && coll().startsWith(kReshardingLocalOplogBufferPrefix);
}

bool NamespaceString::isReshardingConflictStashCollection() const {
    return db() == "config" && coll().startsWith(kReshardingConflictStashPrefix);
}

bool NamespaceString::isTemporaryReshardingCollection() const {
    return coll().startsWith(kTemporaryReshardingCollectionPrefix);
}

bool NamespaceString::isTimeseriesBucketsCollection() const {
    return coll().startsWith(kTimeseriesBucketsCollectionPrefix);
}

bool NamespaceString::isChangeStreamPreImagesCollection() const {
    return ns() == kChangeStreamPreImagesNamespace.ns();
}

bool NamespaceString::isChangeCollection() const {
    return db() == kConfigDb && coll() == kChangeCollectionName;
}

bool NamespaceString::isConfigImagesCollection() const {
    return ns() == kConfigImagesNamespace.ns();
}

bool NamespaceString::isConfigTransactionsCollection() const {
    return ns() == kSessionTransactionsTableNamespace.ns();
}

bool NamespaceString::isFLE2StateCollection() const {
    return coll().startsWith(fle2Prefix) &&
        (coll().endsWith(fle2EscSuffix) || coll().endsWith(fle2EccSuffix) ||
         coll().endsWith(fle2EcocSuffix));
}

bool NamespaceString::isOplogOrChangeCollection() const {
    return isOplog() || isChangeCollection();
}

NamespaceString NamespaceString::makeTimeseriesBucketsNamespace() const {
    return {db(), kTimeseriesBucketsCollectionPrefix.toString() + coll()};
}

NamespaceString NamespaceString::getTimeseriesViewNamespace() const {
    invariant(isTimeseriesBucketsCollection(), ns());
    return {db(), coll().substr(kTimeseriesBucketsCollectionPrefix.size())};
}

bool NamespaceString::isImplicitlyReplicated() const {
    if (isChangeStreamPreImagesCollection() || isConfigImagesCollection() || isChangeCollection()) {
        // Implicitly replicated namespaces are replicated, although they only replicate a subset of
        // writes.
        invariant(isReplicated());
        return true;
    }

    return false;
}

bool NamespaceString::isReplicated() const {
    if (isLocal()) {
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

Status NamespaceStringOrUUID::isNssValid() const {
    if (!_nss || _nss->isValid()) {
        return Status::OK();
    }

    // _nss is set and not valid.
    return {ErrorCodes::InvalidNamespace,
            str::stream() << "Namespace " << _nss << " is not a valid collection name"};
}

std::string NamespaceStringOrUUID::toString() const {
    if (_nss)
        return _nss->toString();
    else
        return _uuid->toString();
}

void NamespaceStringOrUUID::serialize(BSONObjBuilder* builder, StringData fieldName) const {
    invariant(_uuid || _nss);
    if (_preferNssForSerialization) {
        if (_nss) {
            builder->append(fieldName, _nss->coll());
        } else {
            _uuid->appendToBuilder(builder, fieldName);
        }
    } else {
        if (_uuid) {
            _uuid->appendToBuilder(builder, fieldName);
        } else {
            builder->append(fieldName, _nss->coll());
        }
    }
}

std::ostream& operator<<(std::ostream& stream, const NamespaceString& nss) {
    return stream << nss.toString();
}

std::ostream& operator<<(std::ostream& stream, const NamespaceStringOrUUID& nsOrUUID) {
    return stream << nsOrUUID.toString();
}

StringBuilder& operator<<(StringBuilder& builder, const NamespaceString& nss) {
    return builder << nss.toString();
}

StringBuilder& operator<<(StringBuilder& builder, const NamespaceStringOrUUID& nsOrUUID) {
    return builder << nsOrUUID.toString();
}

}  // namespace mongo
