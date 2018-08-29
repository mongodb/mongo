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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/storage_options.h"

#include "mongo/db/server_parameters.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

StorageGlobalParams::StorageGlobalParams() {
    reset();
}

void StorageGlobalParams::reset() {
    engine = "wiredTiger";
    engineSetByUser = false;
    dbpath = kDefaultDbPath;
    upgrade = false;
    repair = false;

    // The intention here is to enable the journal by default if we are running on a 64 bit system.
    dur = (sizeof(void*) == 8);

    noTableScan.store(false);
    directoryperdb = false;
    syncdelay = 60.0;
    readOnly = false;
    groupCollections = false;
}

StorageGlobalParams storageGlobalParams;

/**
 * The directory where the mongod instance stores its data.
 */
#ifdef _WIN32
const char* StorageGlobalParams::kDefaultDbPath = "\\data\\db\\";
const char* StorageGlobalParams::kDefaultConfigDbPath = "\\data\\configdb\\";
#else
const char* StorageGlobalParams::kDefaultDbPath = "/data/db";
const char* StorageGlobalParams::kDefaultConfigDbPath = "/data/configdb";
#endif

const int StorageGlobalParams::kMaxJournalCommitIntervalMs = 500;
const double StorageGlobalParams::kMaxSyncdelaySecs = 9.0 * 1000.0 * 1000.0;

namespace {
/**
 * Specify whether all queries must use indexes.
 * If 1, MongoDB will not execute queries that require a table scan and will return an error.
 * NOT recommended for production use.
 */
ExportedServerParameter<bool, ServerParameterType::kStartupAndRuntime> NoTableScanSetting(
    ServerParameterSet::getGlobal(), "notablescan", &storageGlobalParams.noTableScan);

/**
 * Specify the interval in seconds between fsync operations where mongod flushes its
 * working memory to disk. By default, mongod flushes memory to disk every 60 seconds.
 * In almost every situation you should not set this value and use the default setting.
 */
MONGO_COMPILER_VARIABLE_UNUSED auto _exportedSyncdelay =
    (new ExportedServerParameter<double, ServerParameterType::kStartupAndRuntime>(
        ServerParameterSet::getGlobal(), "syncdelay", &storageGlobalParams.syncdelay))
        -> withValidator([](const double& potentialNewValue) {
            if (potentialNewValue < 0.0 ||
                potentialNewValue > StorageGlobalParams::kMaxSyncdelaySecs) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "syncdelay must be between 0 and "
                                            << StorageGlobalParams::kMaxSyncdelaySecs
                                            << ", but attempted to set to: "
                                            << potentialNewValue);
            }
            return Status::OK();
        });

/**
 * Specify an integer between 1 and kMaxJournalCommitInterval signifying the number of milliseconds
 * (ms) between journal commits.
 */
MONGO_COMPILER_VARIABLE_UNUSED auto _exportedJournalCommitInterval =
    (new ExportedServerParameter<int, ServerParameterType::kRuntimeOnly>(
        ServerParameterSet::getGlobal(),
        "journalCommitInterval",
        &storageGlobalParams.journalCommitIntervalMs))
        -> withValidator([](const int& potentialNewValue) {
            if (potentialNewValue < 1 ||
                potentialNewValue > StorageGlobalParams::kMaxJournalCommitIntervalMs) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "journalCommitInterval must be between 1 and "
                                            << StorageGlobalParams::kMaxJournalCommitIntervalMs
                                            << ", but attempted to set to: "
                                            << potentialNewValue);
            }
            return Status::OK();
        });
}  // namespace
}  // namespace mongo
