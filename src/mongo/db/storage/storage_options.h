/*
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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

/*
 * This file defines the storage for options that come from the command line related to data file
 * persistence.  Many executables that can access data files directly such as mongod and certain
 * tools use these variables, but each executable may have a different set of command line flags
 * that allow the user to change a different subset of these options.
 */

namespace mongo {

struct StorageGlobalParams {
    // Default data directory for mongod when running in non-config server mode.
    static const char* kDefaultDbPath;

    // Default data directory for mongod when running as the config database of
    // a sharded cluster.
    static const char* kDefaultConfigDbPath;

    // --storageEngine
    // storage engine for this instance of mongod.
    std::string engine = "wiredTiger";

    // True if --storageEngine was passed on the command line, and false otherwise.
    bool engineSetByUser = false;

    // The directory where the mongod instance stores its data.
    std::string dbpath = kDefaultDbPath;

    // --upgrade
    // Upgrades the on-disk data format of the files specified by the --dbpath to the
    // latest version, if needed.
    bool upgrade = false;

    // --repair
    // Runs a repair routine on all databases. This is equivalent to shutting down and
    // running the repairDatabase database command on all databases.
    bool repair = false;

    // --repairpath
    // Specifies the root directory containing MongoDB data files to use for the --repair
    // operation.
    // Default: A _tmp directory within the path specified by the dbPath option.
    std::string repairpath;

    // The intention here is to enable the journal by default if we are running on a 64 bit system.
    bool dur = (sizeof(void*) == 8);  // --dur durability (now --journal)

    // --journalCommitInterval
    static const int kMaxJournalCommitIntervalMs;
    std::atomic<int> journalCommitIntervalMs;  // NOLINT

    // --notablescan
    // no table scans allowed
    std::atomic<bool> noTableScan{false};  // NOLINT

    // --directoryperdb
    // Stores each database’s files in its own folder in the data directory.
    // When applied to an existing system, the directoryPerDB option alters
    // the storage pattern of the data directory.
    bool directoryperdb = false;

    // --syncdelay
    // Controls how much time can pass before MongoDB flushes data to the data files
    // via an fsync operation.
    // Do not set this value on production systems.
    // In almost every situation, you should use the default setting.
    AtomicDouble syncdelay{60.0};  // seconds between fsyncs

    bool readOnly = false;
};

extern StorageGlobalParams storageGlobalParams;

}  // namespace mongo
