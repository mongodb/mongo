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

#pragma once

#include <iosfwd>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/config.h"

namespace mongo::logv2 {

// clang-format off
/**
 * id: The enum identifier for the LogComponent.
 * val: (empty except for kDefault) an expression used to assign value to the enum.
 * shortName: its short name, used in component related server options.
 * logName: The key that appears in log the "c" field. Should fit into 8 columns, as
 *          we pad the `c` field in json logs to 8 columns.
 * parent: Components are arranged in a hierarchy for the purposes of log filtering.  The
 *        dottedName that is used to configure log filtering is a parent-recursive "."-join
 *        of shortName strings.
 */
#define MONGO_EXPAND_LOGV2_COMPONENT(X) \
/*   (id, val                  , shortName               , logName   , parent) */ \
    X(kDefault, = 0            , "default"               , "-"       , kNumLogComponents) \
    X(kAccessControl,          , "accessControl"         , "ACCESS"  , kDefault) \
    X(kAssert,                 , "assert"                , "ASSERT"  , kDefault) \
    X(kCommand,                , "command"               , "COMMAND" , kDefault) \
    X(kControl,                , "control"               , "CONTROL" , kDefault) \
    X(kExecutor,               , "executor"              , "EXECUTOR", kDefault) \
    X(kGeo,                    , "geo"                   , "GEO"     , kDefault) \
    X(kGlobalIndex,            , "globalIndex"           , "GBL_IDX" , kDefault) \
    X(kIndex,                  , "index"                 , "INDEX"   , kDefault) \
    X(kNetwork,                , "network"               , "NETWORK" , kDefault) \
    X(kProcessHealth,          , "processHealth"         , "HEALTH"  , kDefault) \
    X(kQuery,                  , "query"                 , "QUERY"   , kDefault) \
    X(kReplication,            , "replication"           , "REPL"    , kDefault) \
    X(kReplicationElection,    , "election"              , "ELECTION", kReplication) \
    X(kReplicationHeartbeats,  , "heartbeats"            , "REPL_HB" , kReplication) \
    X(kReplicationInitialSync, , "initialSync"           , "INITSYNC", kReplication) \
    X(kReplicationRollback,    , "rollback"              , "ROLLBACK", kReplication) \
    X(kSharding,               , "sharding"              , "SHARDING", kDefault) \
    X(kShardingRangeDeleter,   , "rangeDeleter"          , "RDELETER", kSharding) \
    X(kShardingCatalogRefresh, , "shardingCatalogRefresh", "SH_REFR" , kSharding) \
    X(kShardingMigration,      , "migration"             , "MIGRATE" , kSharding) \
    X(kResharding,             , "reshard"               , "RESHARD" , kSharding) \
    X(kShardMigrationPerf,     , "migrationPerf"         , "MIG_PERF", kSharding) \
    X(kStorage,                , "storage"               , "STORAGE" , kDefault) \
    X(kStorageRecovery,        , "recovery"              , "RECOVERY", kStorage) \
    X(kJournal,                , "journal"               , "JOURNAL" , kStorage) \
    X(kWiredTiger,             , "wt"                    , "WT"      , kStorage) \
    X(kWiredTigerBackup,       , "wtBackup"              , "WTBACKUP", kWiredTiger) \
    X(kWiredTigerCheckpoint,   , "wtCheckpoint"          , "WTCHKPT" , kWiredTiger) \
    X(kWiredTigerCompact,      , "wtCompact"             , "WTCMPCT" , kWiredTiger) \
    X(kWiredTigerEviction,     , "wtEviction"            , "WTEVICT" , kWiredTiger) \
    X(kWiredTigerHS,           , "wtHS"                  , "WTHS"    , kWiredTiger) \
    X(kWiredTigerRecovery,     , "wtRecovery"            , "WTRECOV" , kWiredTiger) \
    X(kWiredTigerRTS,          , "wtRTS"                 , "WTRTS"   , kWiredTiger) \
    X(kWiredTigerSalvage,      , "wtSalvage"             , "WTSLVG"  , kWiredTiger) \
    X(kWiredTigerTiered,       , "wtTiered"              , "WTTIER"  , kWiredTiger) \
    X(kWiredTigerTimestamp,    , "wtTimestamp"           , "WTTS"    , kWiredTiger) \
    X(kWiredTigerTransaction,  , "wtTransaction"         , "WTTXN"   , kWiredTiger) \
    X(kWiredTigerVerify,       , "wtVerify"              , "WTVRFY"  , kWiredTiger) \
    X(kWiredTigerWriteLog,     , "wtWriteLog"            , "WTWRTLOG", kWiredTiger) \
    X(kWrite,                  , "write"                 , "WRITE"   , kDefault) \
    X(kFTDC,                   , "ftdc"                  , "FTDC"    , kDefault) \
    X(kASIO,                   , "asio"                  , "ASIO"    , kNetwork) \
    X(kBridge,                 , "bridge"                , "BRIDGE"  , kNetwork) \
    X(kTracking,               , "tracking"              , "TRACKING", kDefault) \
    X(kTransaction,            , "transaction"           , "TXN"     , kDefault) \
    X(kTenantMigration,        , "tenantMigration"       , "TENANT_M", kDefault) \
    X(kConnectionPool,         , "connectionPool"        , "CONNPOOL", kNetwork) \
    X(kTest,                   , "test"                  , "TEST"    , kDefault) \
    X(kResourceConsumption,    , "resourceConsumption"   , "RES_CONS", kDefault) \
    X(kNumLogComponents,       , "total"                 , "TOTAL"   , kNumLogComponents) \
    /**/
// clang-format on

/**
 * Log components.
 * Debug messages logged using the LOG() or MONGO_LOG_COMPONENT().
 * Macros may be associated with one or more log components.
 */
class LogComponent {
public:
    enum Value {
        // clang-format off
        /** Placeholder for using the component set by the MONGO_LOGV2_DEFAULT_COMPONENT macro */
        kAutomaticDetermination = -1,
#define X_(id, val, shortName, logName, parent) id val,
MONGO_EXPAND_LOGV2_COMPONENT(X_)
#undef X_
        // clang-format on
    };

    /* implicit */
    constexpr LogComponent(Value value) : _value(value) {}

    constexpr operator Value() const {
        return _value;
    }

    /**
     * Returns parent component.
     * Returns kNumComponents if parent component is not defined (for kDefault or
     * kNumLogComponents).
     */
    LogComponent parent() const;

    /**
     * Returns short name as a StringData.
     */
    StringData toStringData() const;

    /**
     * Returns short name of log component.
     * Used to generate server parameter names in the format "logLevel_<component short name>".
     */
    std::string getShortName() const;

    /**
     * Returns dotted name of log component - short name prefixed by dot-separated names of
     * ancestors.
     * Used to generate command line and config file option names.
     */
    std::string getDottedName() const;

    /**
     * Returns name suitable for inclusion in formatted log message.
     * This is derived from upper-casing the short name with some padding to
     * fit into a fixed length field.
     */
    StringData getNameForLog() const;

private:
    Value _value;
};

std::ostream& operator<<(std::ostream& os, LogComponent component);

}  // namespace mongo::logv2
