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

#include <atomic>
#include <string>

#include "mongo/platform/atomic_proxy.h"
#include "mongo/platform/atomic_word.h"

/*
 * This file defines the storage for options that come from the command line related to data file
 * persistence.  Many executables that can access data files directly such as mongod and certain
 * tools use these variables, but each executable may have a different set of command line flags
 * that allow the user to change a different subset of these options.
 */

namespace mongo {

struct StorageGlobalParams {
    StorageGlobalParams();
    void reset();

    // Default data directory for mongod when running in non-config server mode.
    static const char* kDefaultDbPath;

    // Default data directory for mongod when running as the config database of
    // a sharded cluster.
    static const char* kDefaultConfigDbPath;

    // --storageEngine
    // storage engine for this instance of mongod.
    std::string engine;

    // True if --storageEngine was passed on the command line, and false otherwise.
    bool engineSetByUser;

    // The directory where the mongod instance stores its data.
    std::string dbpath;

    // --upgrade
    // Upgrades the on-disk data format of the files specified by the --dbpath to the
    // latest version, if needed.
    bool upgrade;

    // --repair
    // Runs a repair routine on all databases.
    bool repair;

    bool dur;  // --dur durability (now --journal)

    // --journalCommitInterval
    static constexpr int kMaxJournalCommitIntervalMs = 500;
    AtomicWord<int> journalCommitIntervalMs;

    // --notablescan
    // no table scans allowed
    AtomicWord<bool> noTableScan;

    // --directoryperdb
    // Stores each database’s files in its own folder in the data directory.
    // When applied to an existing system, the directoryPerDB option alters
    // the storage pattern of the data directory.
    bool directoryperdb;

    // --syncdelay
    // Controls how much time can pass before MongoDB flushes data to the data files
    // via an fsync operation.
    // Do not set this value on production systems.
    // In almost every situation, you should use the default setting.
    static constexpr double kMaxSyncdelaySecs = 9.0 * 1000.0 * 1000.0;
    AtomicDouble syncdelay;  // seconds between fsyncs

    // --queryableBackupMode
    // Puts MongoD into "read-only" mode. MongoD will not write any data to the underlying
    // filesystem. Note that read operations may require writes. For example, a sort on a large
    // dataset may fail if it requires spilling to disk.
    bool readOnly;

    // --groupCollections
    // Dictate to the storage engine that it should attempt to create new MongoDB collections from
    // an existing underlying MongoDB database level resource if possible. This can improve
    // workloads that rely heavily on creating many collections within a database.
    bool groupCollections;

    // Controls whether we allow the OplogStones mechanism to delete oplog history on WT.
    bool allowOplogTruncation = true;
};

extern StorageGlobalParams storageGlobalParams;

}  // namespace mongo
