// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/storage/wiredtiger/spill_wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>

#include <wiredtiger.h>

#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace moe = mongo::optionenvironment;

namespace mongo {
using namespace std::literals::string_view_literals;

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
    if (!(str::equalCaseInsensitive(value, "none"sv) ||
          str::equalCaseInsensitive(value, "snappy"sv) ||
          str::equalCaseInsensitive(value, "zlib"sv) ||
          str::equalCaseInsensitive(value, "zstd"sv))) {
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
                                                    std::string_view name,
                                                    const boost::optional<TenantId>&) {
    *b << name << **_data.first;
}

void SpillWiredTigerEngineRuntimeConfigParameter::append(OperationContext* opCtx,
                                                         BSONObjBuilder* b,
                                                         std::string_view name,
                                                         const boost::optional<TenantId>&) {
    *b << name << **_data.first;
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


Status validateNoNullCharacter(std::string_view str) {
    size_t pos = str.find('\0');
    if (pos != std::string::npos) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "WiredTiger configuration strings cannot contain "
                                       "embedded null characters (found at "
                                    << pos << ')');
    }
    return Status::OK();
}

Status WiredTigerGlobalOptions::validateStatisticsSetting(const std::string& setting) {
    if (!(str::equalCaseInsensitive(setting, "all"sv) ||
          str::equalCaseInsensitive(setting, "cache_walk"sv) ||
          str::equalCaseInsensitive(setting, "fast"sv) ||
          str::equalCaseInsensitive(setting, "none"sv) ||
          str::equalCaseInsensitive(setting, "clear"sv) ||
          str::equalCaseInsensitive(setting, "tree_walk"sv))) {
        return {ErrorCodes::BadValue,
                "storage.wiredTiger.engineConfig.statistics expects one of 'all', 'cache_walk', "
                "'fast', 'none', 'clear', or 'tree_walk'"};
    }

    return Status::OK();
}

template <typename T>
Status setFromStringImpl(T& data, std::string_view str) {
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

Status WiredTigerEngineRuntimeConfigParameter::setFromString(std::string_view str,
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
    std::string_view str, const boost::optional<TenantId>&) {
    if (auto s = validateNoNullCharacter(str); !s.isOK())
        return s;

    LOGV2(10320900,
          "Spill WiredTiger engine runtime configuration parameter set",
          "parameter"_attr = name(),
          "value"_attr = str);

    return setFromStringImpl(_data, str);
}

Status WiredTigerDirectoryForIndexesParameter::setFromString(std::string_view,
                                                             const boost::optional<TenantId>&) {
    return {ErrorCodes::IllegalOperation,
            str::stream() << name() << " cannot be set via setParameter"};
};
void WiredTigerDirectoryForIndexesParameter::append(OperationContext* opCtx,
                                                    BSONObjBuilder* builder,
                                                    std::string_view name,
                                                    const boost::optional<TenantId>&) {
    builder->append(name, wiredTigerGlobalOptions.directoryForIndexes);
}

}  // namespace mongo
