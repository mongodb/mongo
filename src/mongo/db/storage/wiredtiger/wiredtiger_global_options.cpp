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

#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/storage/wiredtiger/spill_wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <wiredtiger.h>

#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace moe = mongo::optionenvironment;

namespace mongo {

WiredTigerGlobalOptions wiredTigerGlobalOptions;

Status WiredTigerGlobalOptions::store(const moe::Environment& params) {
    // WiredTiger storage engine options
    if (!wiredTigerGlobalOptions.engineConfig.empty()) {
        LOGV2(22293, "Engine custom option", "option"_attr = wiredTigerGlobalOptions.engineConfig);
    }

    if (!wiredTigerGlobalOptions.collectionConfig.empty()) {
        LOGV2(22294,
              "Collection custom option",
              "option"_attr = wiredTigerGlobalOptions.collectionConfig);
    }

    if (!wiredTigerGlobalOptions.indexConfig.empty()) {
        LOGV2(22295, "Index custom option", "option"_attr = wiredTigerGlobalOptions.indexConfig);
    }

    return Status::OK();
}

Status WiredTigerGlobalOptions::validateWiredTigerCompressor(const std::string& value) {
    if (!(str::equalCaseInsensitive(value, "none"_sd) ||
          str::equalCaseInsensitive(value, "snappy"_sd) ||
          str::equalCaseInsensitive(value, "zlib"_sd) ||
          str::equalCaseInsensitive(value, "zstd"_sd))) {
        return {ErrorCodes::BadValue,
                "Compression option must be one of: 'none', 'snappy', 'zlib', or 'zstd'"};
    }

    return Status::OK();
}

Status WiredTigerGlobalOptions::validateSpillWiredTigerCompressor(
    const std::string& value, const boost::optional<TenantId>&) {
    return validateWiredTigerCompressor(value);
}

Status WiredTigerGlobalOptions::validateWiredTigerLiveRestoreReadSizeMB(const int value) {
    if (value < 1) {
        return {ErrorCodes::BadValue,
                "Live restore read size must be greater than or equal to 1MB."};
    }

    if (value > 16) {
        return {ErrorCodes::BadValue, "Live restore read size must be less than or equal to 16MB."};
    }

    if ((value & (value - 1)) != 0) {
        return {ErrorCodes::BadValue, "Live restore read size must be a power of two."};
    }

    return Status::OK();
}

void WiredTigerEngineRuntimeConfigParameter::append(OperationContext* opCtx,
                                                    BSONObjBuilder* b,
                                                    StringData name,
                                                    const boost::optional<TenantId>&) {
    *b << name << StringData{*_data.first};
}

void SpillWiredTigerEngineRuntimeConfigParameter::append(OperationContext* opCtx,
                                                         BSONObjBuilder* b,
                                                         StringData name,
                                                         const boost::optional<TenantId>&) {
    *b << name << StringData{*_data.first};
}

Status validateExtraDiagnostics(const std::vector<std::string>& value,
                                const boost::optional<TenantId>& tenantId) {
    try {
        std::set<std::string> flagArr = {"all",
                                         "concurrent_access",
                                         "data_validation",
                                         "invalid_op",
                                         "out_of_order",
                                         "panic",
                                         "slow_operation",
                                         "visibility"};
        for (const auto& diagFlag : value) {
            if (!flagArr.contains(diagFlag)) {
                return Status(ErrorCodes::BadValue,
                              fmt::format("'{}' is not a valid flag option", diagFlag));
            }
        }
    } catch (...) {
        return exceptionToStatus();
    }

    return Status::OK();
}


Status validateNoNullCharacter(StringData str) {
    size_t pos = str.find('\0');
    if (pos != std::string::npos) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "WiredTiger configuration strings cannot contain "
                                       "embedded null characters (found at "
                                    << pos << ')');
    }
    return Status::OK();
}

template <typename T>
Status setFromStringImpl(T& data, StringData str) {
    invariant(data.second);

    int ret = data.second->reconfigure(std::string(str).c_str());
    if (ret != 0) {
        const char* errorStr = wiredtiger_strerror(ret);
        std::string result = (str::stream() << "WiredTiger reconfiguration failed with error code ("
                                            << ret << "): " << errorStr);
        LOGV2_ERROR(22378,
                    "WiredTiger reconfiguration failed",
                    "error"_attr = ret,
                    "message"_attr = errorStr);

        return Status(ErrorCodes::BadValue, result);
    }

    data.first = std::string(str);
    return Status::OK();
}

Status WiredTigerEngineRuntimeConfigParameter::setFromString(StringData str,
                                                             const boost::optional<TenantId>&) {
    if (auto s = validateNoNullCharacter(str); !s.isOK())
        return s;

    LOGV2(22376,
          "WiredTiger engine runtime configuration parameter set",
          "parameter"_attr = name(),
          "value"_attr = str);

    return setFromStringImpl(_data, str);
}

Status SpillWiredTigerEngineRuntimeConfigParameter::setFromString(
    StringData str, const boost::optional<TenantId>&) {
    if (auto s = validateNoNullCharacter(str); !s.isOK())
        return s;

    LOGV2(10320900,
          "Spill WiredTiger engine runtime configuration parameter set",
          "parameter"_attr = name(),
          "value"_attr = str);

    return setFromStringImpl(_data, str);
}

Status WiredTigerDirectoryForIndexesParameter::setFromString(StringData,
                                                             const boost::optional<TenantId>&) {
    return {ErrorCodes::IllegalOperation,
            str::stream() << name() << " cannot be set via setParameter"};
};
void WiredTigerDirectoryForIndexesParameter::append(OperationContext* opCtx,
                                                    BSONObjBuilder* builder,
                                                    StringData name,
                                                    const boost::optional<TenantId>&) {
    builder->append(name, wiredTigerGlobalOptions.directoryForIndexes);
}

}  // namespace mongo
