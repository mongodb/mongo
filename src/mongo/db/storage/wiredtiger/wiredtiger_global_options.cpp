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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"

#include "mongo/util/log.h"

namespace moe = mongo::optionenvironment;

namespace mongo {

WiredTigerGlobalOptions wiredTigerGlobalOptions;

Status WiredTigerGlobalOptions::store(const moe::Environment& params) {
    // WiredTiger storage engine options
    if (params.count("storage.syncPeriodSecs")) {
        wiredTigerGlobalOptions.checkpointDelaySecs =
            static_cast<size_t>(params["storage.syncPeriodSecs"].as<double>());
    }

    if (!wiredTigerGlobalOptions.engineConfig.empty()) {
        log() << "Engine custom option: " << wiredTigerGlobalOptions.engineConfig;
    }

    if (!wiredTigerGlobalOptions.collectionConfig.empty()) {
        log() << "Collection custom option: " << wiredTigerGlobalOptions.collectionConfig;
    }

    if (!wiredTigerGlobalOptions.indexConfig.empty()) {
        log() << "Index custom option: " << wiredTigerGlobalOptions.indexConfig;
    }

    return Status::OK();
}

Status WiredTigerGlobalOptions::validateWiredTigerCompressor(const std::string& value) {
    constexpr auto kNone = "none"_sd;
    constexpr auto kSnappy = "snappy"_sd;
    constexpr auto kZlib = "zlib"_sd;
    constexpr auto kZstd = "zstd"_sd;

    if (!kNone.equalCaseInsensitive(value) && !kSnappy.equalCaseInsensitive(value) &&
        !kZlib.equalCaseInsensitive(value) && !kZstd.equalCaseInsensitive(value)) {
        return {ErrorCodes::BadValue,
                "Compression option must be one of: 'none', 'snappy', 'zlib', or 'zstd'"};
    }

    return Status::OK();
}

Status WiredTigerGlobalOptions::validateMaxCacheOverflowFileSizeGB(double value) {
    if (value != 0.0 && value < 0.1) {
        return {ErrorCodes::BadValue,
                "MaxCacheOverflowFileSizeGB must be either 0 (unbounded) or greater than 0.1."};
    }

    return Status::OK();
}

}  // namespace mongo
