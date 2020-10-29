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
#include "mongo/db/server_options.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

constexpr auto listCollectionsCursorCol = "$cmd.listCollections"_sd;
constexpr auto collectionlessAggregateCursorCol = "$cmd.aggregate"_sd;
constexpr auto dropPendingNSPrefix = "system.drop."_sd;

}  // namespace

constexpr StringData NamespaceString::kAdminDb;
constexpr StringData NamespaceString::kLocalDb;
constexpr StringData NamespaceString::kConfigDb;
constexpr StringData NamespaceString::kSystemDotViewsCollectionName;
constexpr StringData NamespaceString::kOrphanCollectionPrefix;
constexpr StringData NamespaceString::kOrphanCollectionDb;

const NamespaceString NamespaceString::kServerConfigurationNamespace(NamespaceString::kAdminDb,
                                                                     "system.version");
const NamespaceString NamespaceString::kLogicalSessionsNamespace(NamespaceString::kConfigDb,
                                                                 "system.sessions");

// Persisted state for a shard participating in a transaction or retryable write.
const NamespaceString NamespaceString::kSessionTransactionsTableNamespace(
    NamespaceString::kConfigDb, "transactions");

// Persisted state for a shard coordinating a cross-shard transaction.
const NamespaceString NamespaceString::kTransactionCoordinatorsNamespace(
    NamespaceString::kConfigDb, "transaction_coordinators");

const NamespaceString NamespaceString::kMigrationCoordinatorsNamespace(NamespaceString::kConfigDb,
                                                                       "migrationCoordinators");

const NamespaceString NamespaceString::kTenantMigrationDonorsNamespace(NamespaceString::kConfigDb,
                                                                       "tenantMigrationDonors");

const NamespaceString NamespaceString::kTenantMigrationRecipientsNamespace(
    NamespaceString::kConfigDb, "tenantMigrationRecipients");

const NamespaceString NamespaceString::kShardConfigCollectionsNamespace(NamespaceString::kConfigDb,
                                                                        "cache.collections");
const NamespaceString NamespaceString::kShardConfigDatabasesNamespace(NamespaceString::kConfigDb,
                                                                      "cache.databases");
const NamespaceString NamespaceString::kSystemKeysNamespace(NamespaceString::kAdminDb,
                                                            "system.keys");
const NamespaceString NamespaceString::kRsOplogNamespace(NamespaceString::kLocalDb, "oplog.rs");
const NamespaceString NamespaceString::kSystemReplSetNamespace(NamespaceString::kLocalDb,
                                                               "system.replset");
const NamespaceString NamespaceString::kIndexBuildEntryNamespace(NamespaceString::kConfigDb,
                                                                 "system.indexBuilds");
const NamespaceString NamespaceString::kRangeDeletionNamespace(NamespaceString::kConfigDb,
                                                               "rangeDeletions");
const NamespaceString NamespaceString::kConfigReshardingOperationsNamespace(
    NamespaceString::kConfigDb, "reshardingOperations");

const NamespaceString NamespaceString::kDonorReshardingOperationsNamespace(
    NamespaceString::kConfigDb, "localReshardingOperations.donor");

const NamespaceString NamespaceString::kRecipientReshardingOperationsNamespace(
    NamespaceString::kConfigDb, "localReshardingOperations.recipient");

const NamespaceString NamespaceString::kConfigSettingsNamespace(NamespaceString::kConfigDb,
                                                                "settings");
const NamespaceString NamespaceString::kVectorClockNamespace(NamespaceString::kConfigDb,
                                                             "vectorClock");

const NamespaceString NamespaceString::kReshardingApplierProgressNamespace(
    NamespaceString::kConfigDb, "localReshardingOperations.recipient.progress_applier");

const NamespaceString NamespaceString::kReshardingTxnClonerProgressNamespace(
    NamespaceString::kConfigDb, "localReshardingOperations.recipient.progress_txn_cloner");

bool NamespaceString::isListCollectionsCursorNS() const {
    return coll() == listCollectionsCursorCol;
}

bool NamespaceString::isCollectionlessAggregateNS() const {
    return coll() == collectionlessAggregateCursorCol;
}

bool NamespaceString::isLegalClientSystemNS() const {
    if (db() == kAdminDb) {
        if (coll() == "system.roles")
            return true;
        if (coll() == kServerConfigurationNamespace.coll())
            return true;
        if (coll() == kSystemKeysNamespace.coll())
            return true;
        if (coll() == "system.backup_users")
            return true;
    } else if (db() == kConfigDb) {
        if (coll() == "system.sessions")
            return true;
        if (coll() == kIndexBuildEntryNamespace.coll())
            return true;
        if (coll().find(".system.resharding.") != std::string::npos)
            return true;
    } else if (db() == kLocalDb) {
        if (coll() == kSystemReplSetNamespace.coll())
            return true;
        if (coll() == "system.healthlog")
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
    if (isTimeseriesBucketsCollection()) {
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
 */
bool NamespaceString::mustBeAppliedInOwnOplogBatch() const {
    return isSystemDotViews() || isServerConfigurationCollection() || isPrivilegeCollection();
}

NamespaceString NamespaceString::makeListCollectionsNSS(StringData dbName) {
    NamespaceString nss(dbName, listCollectionsCursorCol);
    dassert(nss.isValid());
    dassert(nss.isListCollectionsCursorNS());
    return nss;
}

NamespaceString NamespaceString::makeCollectionlessAggregateNSS(StringData dbname) {
    NamespaceString nss(dbname, collectionlessAggregateCursorCol);
    dassert(nss.isValid());
    dassert(nss.isCollectionlessAggregateNS());
    return nss;
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

    // Certain config collections can never be sharded
    if (ns() == kSessionTransactionsTableNamespace.ns() || ns() == kRangeDeletionNamespace.ns() ||
        ns() == kTransactionCoordinatorsNamespace.ns() || ns() == kVectorClockNamespace.ns() ||
        ns() == kMigrationCoordinatorsNamespace.ns() || ns() == kIndexBuildEntryNamespace.ns())
        return true;

    if (isSystemDotProfile())
        return true;

    if (isSystemDotViews())
        return true;

    if (ns() == "config.cache.databases" || ns() == "config.cache.collections" ||
        isConfigDotCacheDotChunks())
        return true;

    return false;
}

bool NamespaceString::isConfigDotCacheDotChunks() const {
    return db() == "config" && coll().startsWith("cache.chunks.");
}

bool NamespaceString::isTemporaryReshardingCollection() const {
    return coll().startsWith(kTemporaryReshardingCollectionPrefix);
}

bool NamespaceString::isTimeseriesBucketsCollection() const {
    return coll().startsWith(kTimeseriesBucketsCollectionPrefix);
}

NamespaceString NamespaceString::makeTimeseriesBucketsNamespace() const {
    auto bucketsColl = kTimeseriesBucketsCollectionPrefix.toString() + coll();
    return {db(), bucketsColl};
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

std::string NamespaceStringOrUUID::toString() const {
    if (_nss)
        return _nss->toString();
    else
        return _uuid->toString();
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
