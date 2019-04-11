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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/embedded/embedded_options.h"

#include "mongo/db/server_options_base.h"
#include "mongo/db/server_options_helpers.h"
#include "mongo/db/storage/mobile/mobile_options_gen.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/embedded/embedded_options_gen.h"

#include <boost/filesystem.hpp>
#include <string>

namespace mongo {
namespace embedded {

using std::string;

Status addOptions(optionenvironment::OptionSection* options) {
    Status ret = addBaseServerOptions(options);
    if (!ret.isOK()) {
        return ret;
    }

    ret = addMobileStorageOptionDefinitions(options);
    if (!ret.isOK()) {
        return ret;
    }

    return embedded::addStorageOptions(options);
}

Status canonicalizeOptions(optionenvironment::Environment* params) {

    Status ret = canonicalizeBaseOptions(params);
    if (!ret.isOK()) {
        return ret;
    }

    return Status::OK();
}

Status storeOptions(const moe::Environment& params) {
    Status ret = storeBaseOptions(params);
    if (!ret.isOK()) {
        return ret;
    }

    if (params.count("storage.engine")) {
        storageGlobalParams.engine = params["storage.engine"].as<std::string>();
        storageGlobalParams.engineSetByUser = true;
    }

    if (params.count("storage.dbPath")) {
        storageGlobalParams.dbpath = params["storage.dbPath"].as<string>();
    }

    ret = storeMobileStorageOptionDefinitions(params);
    if (!ret.isOK()) {
        return ret;
    }

#ifdef _WIN32
    if (storageGlobalParams.dbpath.size() > 1 &&
        storageGlobalParams.dbpath[storageGlobalParams.dbpath.size() - 1] == '/') {
        // size() check is for the unlikely possibility of --dbpath "/"
        storageGlobalParams.dbpath =
            storageGlobalParams.dbpath.erase(storageGlobalParams.dbpath.size() - 1);
    }
#endif

#ifdef _WIN32
    // If dbPath is a default value, prepend with drive name so log entries are explicit
    // We must resolve the dbpath before it stored in repairPath in the default case.
    if (storageGlobalParams.dbpath == storageGlobalParams.kDefaultDbPath ||
        storageGlobalParams.dbpath == storageGlobalParams.kDefaultConfigDbPath) {
        boost::filesystem::path currentPath = boost::filesystem::current_path();
        storageGlobalParams.dbpath = currentPath.root_name().string() + storageGlobalParams.dbpath;
    }
#endif

    return Status::OK();
}

void resetOptions() {
    storageGlobalParams.reset();
    mobileGlobalOptions = MobileOptions();
}

std::string storageDBPathDescription() {
    StringBuilder sb;

    sb << "Directory for datafiles - defaults to " << storageGlobalParams.kDefaultDbPath;

#ifdef _WIN32
    boost::filesystem::path currentPath = boost::filesystem::current_path();

    sb << " which is " << currentPath.root_name().string() << storageGlobalParams.kDefaultDbPath
       << " based on the current working drive";
#endif

    return sb.str();
}

}  // namespace embedded
}  // namespace mongo
