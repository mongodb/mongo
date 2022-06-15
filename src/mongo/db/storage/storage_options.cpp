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

#include "mongo/db/storage/storage_options.h"

#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/str.h"

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
    restore = false;

    noTableScan.store(false);
    directoryperdb = false;
    syncdelay = 60.0;
    queryableBackupMode = false;
    groupCollections = false;
    oplogMinRetentionHours.store(0.0);
    allowOplogTruncation = true;
    disableLockFreeReads = false;
    checkpointDelaySecs = 0;
}

StorageGlobalParams storageGlobalParams;

Status StorageDirectoryPerDbParameter::setFromString(const std::string&) {
    return {ErrorCodes::IllegalOperation,
            str::stream() << name() << " cannot be set via setParameter"};
};

void StorageDirectoryPerDbParameter::append(OperationContext* opCtx,
                                            BSONObjBuilder& builder,
                                            const std::string& name) {
    builder.append(name, storageGlobalParams.directoryperdb);
}


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

}  // namespace mongo
