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

#include "mongo/db/storage/storage_options.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/str.h"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

StorageGlobalParams::StorageGlobalParams() {
    _reset();
}

void StorageGlobalParams::reset_forTest() {
    _reset();
}

void StorageGlobalParams::_reset() {
    engine = "wiredTiger";
    engineSetByUser = false;
    dbpath = kDefaultDbPath;
    upgrade = false;
    repair = false;
    validate = false;
    restore = false;
    magicRestore = false;

    noTableScan.store(false);
    directoryperdb = false;
    syncdelay.store(-1.0);
    queryableBackupMode = false;
    groupCollections = false;
    oplogMinRetentionHours.store(0.0);
    allowOplogTruncation = true;
    forceDisableTableLogging = false;
}

std::string StorageGlobalParams::getSpillDbPath() const {
    return gSpillPath.empty() ? (boost::filesystem::path(dbpath) / "_tmp" / "spilldb").string()
                              : gSpillPath;
}

StorageGlobalParams storageGlobalParams;

Status StorageDirectoryPerDbParameter::setFromString(StringData, const boost::optional<TenantId>&) {
    return {ErrorCodes::IllegalOperation,
            str::stream() << name() << " cannot be set via setParameter"};
};

void StorageDirectoryPerDbParameter::append(OperationContext* opCtx,
                                            BSONObjBuilder* builder,
                                            StringData name,
                                            const boost::optional<TenantId>&) {
    builder->append(name, storageGlobalParams.directoryperdb);
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
