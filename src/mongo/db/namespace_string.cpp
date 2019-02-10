
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
#include "mongo/util/mongoutils/str.h"

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

bool NamespaceString::isListCollectionsCursorNS() const {
    return coll() == listCollectionsCursorCol;
}

bool NamespaceString::isCollectionlessAggregateNS() const {
    return coll() == collectionlessAggregateCursorCol;
}

bool NamespaceString::isLegalClientSystemNS() const {
    if (db() == "admin") {
        if (ns() == "admin.system.roles")
            return true;
        if (ns() == kServerConfigurationNamespace.ns())
            return true;
        if (ns() == kSystemKeysNamespace.ns())
            return true;
        if (ns() == "admin.system.new_users")
            return true;
        if (ns() == "admin.system.backup_users")
            return true;
    } else if (db() == "config") {
        if (ns() == "config.system.sessions")
            return true;
    }

    if (ns() == "local.system.replset")
        return true;

    if (coll() == "system.users")
        return true;
    if (coll() == "system.js")
        return true;

    if (coll() == kSystemDotViewsCollectionName)
        return true;

    return false;
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

bool NamespaceString::isDropPendingNamespace() const {
    return coll().startsWith(dropPendingNSPrefix);
}

NamespaceString NamespaceString::makeDropPendingNamespace(const repl::OpTime& opTime) const {
    StringBuilder ss;
    ss << db() << "." << dropPendingNSPrefix;
    ss << opTime.getSecs() << "i" << opTime.getTimestamp().getInc() << "t" << opTime.getTerm();
    ss << "." << coll();
    return NamespaceString(ss.stringData().substr(0, MaxNsCollectionLen));
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
    auto status = parseNumberFromString(opTimeStr.substr(0, incrementSeparatorIndex), &seconds);
    if (!status.isOK()) {
        return status.withContext(
            str::stream() << "Invalid timestamp seconds in drop-pending namespace: " << _ns);
    }

    unsigned int increment;
    status =
        parseNumberFromString(opTimeStr.substr(incrementSeparatorIndex + 1,
                                               termSeparatorIndex - (incrementSeparatorIndex + 1)),
                              &increment);
    if (!status.isOK()) {
        return status.withContext(
            str::stream() << "Invalid timestamp increment in drop-pending namespace: " << _ns);
    }

    long long term;
    status = mongo::parseNumberFromString(opTimeStr.substr(termSeparatorIndex + 1), &term);
    if (!status.isOK()) {
        return status.withContext(str::stream() << "Invalid term in drop-pending namespace: "
                                                << _ns);
    }

    return repl::OpTime(Timestamp(Seconds(seconds), increment), term);
}

Status NamespaceString::checkLengthForRename(
    const std::string::size_type longestIndexNameLength) const {
    auto longestAllowed =
        std::min(std::string::size_type(NamespaceString::MaxNsCollectionLen),
                 std::string::size_type(NamespaceString::MaxNsLen - 2U /*strlen(".$")*/ -
                                        longestIndexNameLength));
    if (size() > longestAllowed) {
        StringBuilder sb;
        sb << "collection name length of " << size() << " exceeds maximum length of "
           << longestAllowed << ", allowing for index names";
        return Status(ErrorCodes::InvalidLength, sb.str());
    }
    return Status::OK();
}

NamespaceString NamespaceString::makeIndexNamespace(StringData indexName) const {
    StringBuilder ss;
    ss << coll() << ".$" << indexName;
    return NamespaceString(db(), ss.stringData());
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
