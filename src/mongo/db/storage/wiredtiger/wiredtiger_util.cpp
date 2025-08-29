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

#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"

#include "mongo/bson/json.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/rss/persistence_provider.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/db/storage/snapshot_window_options_gen.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_event_handler.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"
#include "mongo/util/pcre.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/testing_proctor.h"

#include <algorithm>

#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWiredTiger

namespace mongo {
namespace {

// Key names for the logging subsystem setting in configuration string.
// These are used to look up keys with the WiredTigerConfigParser.
// Since these StringData values are created from c-strings, it is ok to use data() to get a
// null-terminated c-string for the WiredTiger API.
auto kLogKeyName = "log"_sd;
auto kLogEnabledKeyName = "enabled"_sd;

auto& cancelledCacheMetric = *MetricBuilder<Counter64>("storage.cancelledCacheEvictions");

// TODO SERVER-81069: Remove this.
MONGO_FAIL_POINT_DEFINE(allowEncryptionOptionsInCreationString);

const std::string kTableExtension = ".wt";
const std::string kWiredTigerBackupFile = "WiredTiger.backup";
const static StaticImmortal<pcre::Regex> encryptionOptsRegex(R"re(encryption=\([^\)]*\),?)re");

StatusWith<std::string> _getMetadata(WT_CURSOR* cursor, StringData uri) {
    std::string strUri = std::string{uri};
    cursor->set_key(cursor, strUri.c_str());
    int ret = cursor->search(cursor);
    if (ret == WT_NOTFOUND) {
        return StatusWith<std::string>(ErrorCodes::NoSuchKey,
                                       str::stream() << "Unable to find metadata for " << uri);
    } else if (ret != 0) {
        return StatusWith<std::string>(wtRCToStatus(ret, cursor->session));
    }
    const char* metadata = nullptr;
    ret = cursor->get_value(cursor, &metadata);
    if (ret != 0) {
        return StatusWith<std::string>(wtRCToStatus(ret, cursor->session));
    }
    invariant(metadata);
    return StatusWith<std::string>(metadata);
}

WT_CURSOR* _getMaybeCachedCursor(WiredTigerSession& session,
                                 const std::string& uri,
                                 uint64_t tableId) {
    WT_CURSOR* cursor = nullptr;
    try {
        cursor = session.getCachedCursor(tableId, "");
        if (!cursor) {
            cursor = session.getNewCursor(uri);
        }
    } catch (const ExceptionFor<ErrorCodes::CursorNotFound>& ex) {
        LOGV2_FATAL_NOTRACE(31293, "Cursor not found", "error"_attr = ex);
    }
    invariant(cursor);
    return cursor;
}

}  // namespace

using std::string;

std::string WiredTigerUtil::buildTableUri(StringData ident) {
    invariant(ident.find(kTableUriPrefix) == string::npos);
    invariant(ident::isValidIdent(ident), ident);
    return kTableUriPrefix + std::string{ident};
}

void WiredTigerUtil::fetchTypeAndSourceURI(WiredTigerSession& session,
                                           const std::string& tableUri,
                                           std::string* type,
                                           std::string* source) {
    std::string colgroupUri = "colgroup";
    const size_t colon = tableUri.find(':');
    invariant(colon != string::npos);
    colgroupUri += tableUri.substr(colon);
    StatusWith<std::string> colgroupResult = getMetadataCreate(session, colgroupUri);
    invariant(colgroupResult.getStatus());
    WiredTigerConfigParser parser(colgroupResult.getValue());

    WT_CONFIG_ITEM typeItem;
    invariant(parser.get("type", &typeItem) == 0);
    invariant(typeItem.type == WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID);
    *type = std::string(typeItem.str, typeItem.len);

    WT_CONFIG_ITEM sourceItem;
    invariant(parser.get("source", &sourceItem) == 0);
    invariant(sourceItem.type == WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRING);
    *source = std::string(sourceItem.str, sourceItem.len);
}

StatusWith<std::string> WiredTigerUtil::getMetadataCreate(WiredTigerSession& session,
                                                          StringData uri) {
    WT_CURSOR* cursor = _getMaybeCachedCursor(session, "metadata:create", kMetadataCreateTableId);
    ScopeGuard releaser = [&] {
        session.releaseCursor(kMetadataCreateTableId, cursor, "");
    };
    return _getMetadata(cursor, uri);
}

StatusWith<std::string> WiredTigerUtil::getMetadata(WiredTigerSession& session, StringData uri) {
    WT_CURSOR* cursor = _getMaybeCachedCursor(session, "metadata:", kMetadataTableId);
    ScopeGuard releaser = [&] {
        session.releaseCursor(kMetadataTableId, cursor, "");
    };

    return _getMetadata(cursor, uri);
}

StatusWith<std::string> WiredTigerUtil::getSourceMetadata(WiredTigerSession& session,
                                                          StringData uri) {
    if (uri.starts_with("file:")) {
        return getMetadata(session, uri);
    }
    invariant(uri.starts_with("table:"));

    WT_CURSOR* cursor = _getMaybeCachedCursor(session, "metadata:", kMetadataTableId);
    ScopeGuard releaser = [&] {
        session.releaseCursor(kMetadataTableId, cursor, "");
    };

    // Look up the config for the single colgroup for the table
    auto colgroupUri = std::string("colgroup:") + uri.substr(strlen("table:"));
    auto colgroupMetadata = _getMetadata(cursor, colgroupUri);
    if (!colgroupMetadata.isOK())
        return colgroupMetadata.getStatus();

    // The source field of the colgroup's config is the URI of the file backing the colgroup
    WiredTigerConfigParser parser(colgroupMetadata.getValue());
    WT_CONFIG_ITEM item;
    invariant(parser.get("source", &item) == 0);
    invariant(item.type == WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRING);

    // Get the metadata for the file
    return _getMetadata(cursor, StringData(item.str, item.len));
}

Status WiredTigerUtil::getApplicationMetadata(WiredTigerSession& session,
                                              StringData uri,
                                              BSONObjBuilder* bob) {
    StatusWith<std::string> metadataResult = getMetadata(session, uri);
    if (!metadataResult.isOK()) {
        return metadataResult.getStatus();
    }
    WiredTigerConfigParser topParser(metadataResult.getValue());
    WT_CONFIG_ITEM appMetadata;
    if (topParser.get("app_metadata", &appMetadata) != 0) {
        return Status::OK();
    }
    if (appMetadata.len == 0) {
        return Status::OK();
    }
    if (appMetadata.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "app_metadata must be a nested struct. Actual value: "
                                    << StringData(appMetadata.str, appMetadata.len));
    }

    WiredTigerConfigParser parser(appMetadata);
    WT_CONFIG_ITEM keyItem;
    WT_CONFIG_ITEM valueItem;
    int ret;
    StringDataSet keysSeen;
    while ((ret = parser.next(&keyItem, &valueItem)) == 0) {
        const StringData key(keyItem.str, keyItem.len);
        if (keysSeen.count(key)) {
            return Status(ErrorCodes::Error(50998),
                          str::stream() << "app_metadata must not contain duplicate keys. "
                                        << "Found multiple instances of key '" << key << "'.");
        }
        keysSeen.insert(key);

        switch (valueItem.type) {
            case WT_CONFIG_ITEM::WT_CONFIG_ITEM_BOOL:
                bob->appendBool(key, valueItem.val);
                break;
            case WT_CONFIG_ITEM::WT_CONFIG_ITEM_NUM:
                bob->appendNumber(key, static_cast<long long>(valueItem.val));
                break;
            default:
                bob->append(key, StringData(valueItem.str, valueItem.len));
                break;
        }
    }
    if (ret != WT_NOTFOUND) {
        return wtRCToStatus(ret, nullptr);
    }

    return Status::OK();
}

StatusWith<BSONObj> WiredTigerUtil::getApplicationMetadata(WiredTigerSession& session,
                                                           StringData uri) {
    BSONObjBuilder bob;
    Status status = getApplicationMetadata(session, uri, &bob);
    if (!status.isOK()) {
        return StatusWith<BSONObj>(status);
    }
    return StatusWith<BSONObj>(bob.obj());
}

StatusWith<int64_t> WiredTigerUtil::checkApplicationMetadataFormatVersion(
    WiredTigerSession& session, StringData uri, int64_t minimumVersion, int64_t maximumVersion) {
    StatusWith<std::string> result = getMetadata(session, uri);
    if (result == ErrorCodes::NoSuchKey) {
        return result.getStatus();
    }
    invariant(result.getStatus());

    WiredTigerConfigParser topParser(result.getValue());
    WT_CONFIG_ITEM metadata;
    if (topParser.get("app_metadata", &metadata) != 0)
        return Status(ErrorCodes::UnsupportedFormat,
                      str::stream() << "application metadata for " << uri << " is missing ");

    if (metadata.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream()
                          << "application metadata must be enclosed in parentheses. Actual value: "
                          << StringData(metadata.str, metadata.len));
    }

    WiredTigerConfigParser parser(metadata);

    int64_t version = 0;
    WT_CONFIG_ITEM versionItem;
    if (parser.get("formatVersion", &versionItem) != 0) {
        // If 'formatVersion' is missing, this metadata was introduced by
        // one of the RC versions (where the format version is 1).
        version = 1;
    } else if (versionItem.type == WT_CONFIG_ITEM::WT_CONFIG_ITEM_NUM) {
        version = versionItem.val;
    } else {
        return Status(ErrorCodes::UnsupportedFormat,
                      str::stream() << "'formatVersion' in application metadata for " << uri
                                    << " must be a number. Current value: "
                                    << StringData(versionItem.str, versionItem.len));
    }

    if (version < minimumVersion || version > maximumVersion) {
        return Status(ErrorCodes::UnsupportedFormat,
                      str::stream() << "Application metadata for " << uri
                                    << " has unsupported format version: " << version << ".");
    }

    LOGV2_DEBUG(22428,
                2,
                "WiredTigerUtil::checkApplicationMetadataFormatVersion  uri: {uri} ok range "
                "{minimumVersion} -> {maximumVersion} current: {version}",
                "uri"_attr = uri,
                "minimumVersion"_attr = minimumVersion,
                "maximumVersion"_attr = maximumVersion,
                "version"_attr = version);

    return version;
}

// static
Status WiredTigerUtil::checkTableCreationOptions(const BSONElement& configElem) {
    invariant(configElem.fieldNameStringData() == WiredTigerUtil::kConfigStringField);

    if (configElem.type() != BSONType::string) {
        return {ErrorCodes::TypeMismatch, "'configString' must be a string."};
    }

    StringSet errors;
    ErrorAccumulator eventHandler(&errors);

    StringData config = configElem.valueStringData();
    // Do NOT allow embedded null characters
    if (config.size() != strlen(config.data())) {
        return {ErrorCodes::FailedToParse, "malformed 'configString' value."};
    }

    if (config.find("type=lsm") != std::string::npos) {
        return {ErrorCodes::Error(6627201), "Configuration 'type=lsm' is not supported."};
    }

    if (gFeatureFlagBanEncryptionOptionsInCollectionCreation.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
        encryptionOptsRegex->matchView(config) &&
        MONGO_likely(!allowEncryptionOptionsInCreationString.shouldFail())) {
        return {ErrorCodes::IllegalOperation,
                "Manual configuration of encryption options as part of 'configString' is not "
                "supported"};
    }

    Status status = wtRCToStatus(
        wiredtiger_config_validate(nullptr, &eventHandler, "WT_SESSION.create", config.data()),
        nullptr);
    if (!status.isOK()) {
        StringBuilder errorMsg;
        errorMsg << status.reason();
        for (const std::string& error : errors) {
            errorMsg << ". " << error;
        }
        errorMsg << ".";
        return status.withReason(errorMsg.stringData());
    }
    return Status::OK();
}

// static
StatusWith<int64_t> WiredTigerUtil::getStatisticsValue(WiredTigerSession& session,
                                                       const std::string& uri,
                                                       const std::string& config,
                                                       int statisticsKey) {
    return session.with(
        [&](WT_SESSION* s) { return getStatisticsValue_DoNotUse(s, uri, config, statisticsKey); });
}

// static
StatusWith<int64_t> WiredTigerUtil::getStatisticsValue_DoNotUse(WT_SESSION* session,
                                                                const std::string& uri,
                                                                const std::string& config,
                                                                int statisticsKey) {
    invariant(session);
    WT_CURSOR* cursor = nullptr;
    const char* cursorConfig = config.empty() ? nullptr : config.c_str();
    int ret = session->open_cursor(session, uri.c_str(), nullptr, cursorConfig, &cursor);
    if (ret != 0) {
        // The numerical 'statisticsKey' can be located in the WT_STATS_* preprocessor macros in
        // wiredtiger.h.
        return StatusWith<int64_t>(ErrorCodes::CursorNotFound,
                                   str::stream() << "unable to open cursor at URI " << uri
                                                 << " for statistic: " << statisticsKey
                                                 << ". reason: " << wiredtiger_strerror(ret));
    }
    invariant(cursor);
    ON_BLOCK_EXIT([&] { cursor->close(cursor); });

    cursor->set_key(cursor, statisticsKey);
    ret = cursor->search(cursor);
    if (ret != 0) {
        return StatusWith<int64_t>(ErrorCodes::NoSuchKey,
                                   str::stream()
                                       << "unable to find key " << statisticsKey << " at URI "
                                       << uri << ". reason: " << wiredtiger_strerror(ret));
    }

    int64_t value;
    ret = cursor->get_value(cursor, nullptr, nullptr, &value);
    if (ret != 0) {
        return StatusWith<int64_t>(ErrorCodes::BadValue,
                                   str::stream() << "unable to get value for key " << statisticsKey
                                                 << " at URI " << uri
                                                 << ". reason: " << wiredtiger_strerror(ret));
    }

    return StatusWith<int64_t>(value);
}

int64_t WiredTigerUtil::getIdentSize(WiredTigerSession& s, const std::string& uri) {
    StatusWith<int64_t> result = WiredTigerUtil::getStatisticsValue(
        s, "statistics:" + uri, "statistics=(size)", WT_STAT_DSRC_BLOCK_SIZE);
    const Status& status = result.getStatus();
    if (!status.isOK()) {
        if (status.code() == ErrorCodes::CursorNotFound) {
            // ident gone, so its 0
            return 0;
        }
        uassertStatusOK(status);
    }
    return result.getValue();
}

int64_t WiredTigerUtil::getEphemeralIdentSize(WiredTigerSession& session, const std::string& uri) {
    // For ephemeral case, use cursor statistics
    const auto statsUri = "statistics:" + uri;

    // Helper function to retrieve stats and check for errors
    auto getStats = [&](int key) -> int64_t {
        auto result = getStatisticsValue(session, statsUri, "statistics=(fast)", key);
        if (!result.isOK()) {
            if (result.getStatus().code() == ErrorCodes::CursorNotFound)
                return 0;  // ident gone, so return 0

            uassertStatusOK(result.getStatus());
        }
        return result.getValue();
    };

    auto inserts = getStats(WT_STAT_DSRC_CURSOR_INSERT);
    auto removes = getStats(WT_STAT_DSRC_CURSOR_REMOVE);
    auto insertBytes = getStats(WT_STAT_DSRC_CURSOR_INSERT_BYTES);

    if (inserts == 0 || removes >= inserts)
        return 0;

    // Rough approximation of index size as average entry size times number of entries.
    // May be off if key sizes change significantly over the life time of the collection,
    // but is the best we can do currrently with the statistics available.
    auto bytesPerEntry = (insertBytes + inserts - 1) / inserts;  // round up
    auto numEntries = inserts - removes;
    return numEntries * bytesPerEntry;
}

int64_t WiredTigerUtil::getIdentReuseSize(WiredTigerSession& session, const std::string& uri) {
    auto result = WiredTigerUtil::getStatisticsValue(
        session, "statistics:" + uri, "statistics=(fast)", WT_STAT_DSRC_BLOCK_REUSE_BYTES);
    uassertStatusOK(result.getStatus());
    return result.getValue();
}

int64_t WiredTigerUtil::getIdentCompactRewrittenExpectedSize(WiredTigerSession& session,
                                                             const std::string& uri) {
    auto result =
        WiredTigerUtil::getStatisticsValue(session,
                                           "statistics:" + uri,
                                           "statistics=(fast)",
                                           WT_STAT_DSRC_BTREE_COMPACT_BYTES_REWRITTEN_EXPECTED);
    uassertStatusOK(result.getStatus());
    return result.getValue();
}

size_t WiredTigerUtil::getMainCacheSizeMB(double requestedCacheSizeGB,
                                          double requestedCacheSizePct) {
    invariant(!(requestedCacheSizeGB && requestedCacheSizePct));
    double cacheSizeMB;
    const double kMaxSizeCacheMB = 10 * 1000 * 1000;
    if (requestedCacheSizeGB <= 0) {
        ProcessInfo pi;
        double memSizeMB = pi.getMemSizeMB();
        double userMemSizeMB =
            std::min(WiredTigerUtil::memoryThresholdPercentage, requestedCacheSizePct) * memSizeMB;
        if (requestedCacheSizePct <= 0) {
            // Default (no size set by user).
            // Set a minimum of 256MB, otherwise use 50% of available memory over 1GB.
            cacheSizeMB = std::max((memSizeMB - 1024) * 0.5, 256.0);
        } else {
            // Percentage-based cache (cacheSizePct).
            cacheSizeMB = std::max(256.0, userMemSizeMB);
        }
    } else {
        // Size-based cache (cacheSizeGB).
        cacheSizeMB = 1024 * requestedCacheSizeGB;
    }
    if (cacheSizeMB > kMaxSizeCacheMB) {
        LOGV2(22429,
              "Requested cache size exceeds max, setting to maximum",
              "requestedMB"_attr = cacheSizeMB,
              "maximumMB"_attr = kMaxSizeCacheMB);
        cacheSizeMB = kMaxSizeCacheMB;
    }
    return static_cast<size_t>(std::floor(cacheSizeMB));
}

int32_t WiredTigerUtil::getSpillCacheSizeMB(int32_t systemMemoryMB,
                                            double pct,
                                            int32_t minMB,
                                            int32_t maxMB) {
    uassert(10698700,
            "Min spill engine cache size cannot be greater than max spill engine cache size",
            minMB <= maxMB);
    return std::clamp(static_cast<int32_t>(systemMemoryMB * pct * 0.01), minMB, maxMB);
}

logv2::LogSeverity getWTLogSeverityLevel(const BSONObj& obj) {
    const std::string field = "verbose_level_id";

    if (!obj.hasField(field)) {
        throw std::logic_error("The following field is missing: " + field);
    }

    BSONElement verbose_level_id_ele = obj[field];
    if (!verbose_level_id_ele.isNumber()) {
        throw std::logic_error("The value associated to " + field + " must be a number");
    }

    // Matching each WiredTiger verbosity level to the equivalent LOGV2 severity level.
    switch (verbose_level_id_ele.Int()) {
        case WT_VERBOSE_ERROR:
            return logv2::LogSeverity::Error();
        case WT_VERBOSE_WARNING:
            return logv2::LogSeverity::Warning();
        case WT_VERBOSE_NOTICE:
            return logv2::LogSeverity::Info();
        case WT_VERBOSE_INFO:
            return logv2::LogSeverity::Log();
        default:
            // MongoDB enables some WT debug compnonents by default. If performed a 1:1
            // translation from WT log severity levels, MongoDB would not log anything
            // below default level Log, even if a Debug message came through the message
            // handler.  To solve this, we upgrade all Debug messages to the Log level
            // to ensure they are seen.
            return logv2::LogSeverity::Log();
    }
}

logv2::LogComponent getWTLogComponent(const BSONObj& obj) {
    const std::string field = "category_id";

    if (!obj.hasField(field)) {
        throw std::logic_error("The following field is missing: " + field);
    }

    BSONElement category_id_ele = obj[field];
    if (!category_id_ele.isNumber()) {
        throw std::logic_error("The value associated to " + field + " must be a number");
    }

    switch (category_id_ele.Int()) {
        case WT_VERB_BACKUP:
            return logv2::LogComponent::kWiredTigerBackup;
        case WT_VERB_CHECKPOINT:
        case WT_VERB_CHECKPOINT_CLEANUP:
        case WT_VERB_CHECKPOINT_PROGRESS:
            return logv2::LogComponent::kWiredTigerCheckpoint;
        case WT_VERB_COMPACT:
        case WT_VERB_COMPACT_PROGRESS:
            return logv2::LogComponent::kWiredTigerCompact;
        case WT_VERB_EVICTION:
            return logv2::LogComponent::kWiredTigerEviction;
        case WT_VERB_FILEOPS:
            return logv2::LogComponent::kWiredTigerFileOps;
        case WT_VERB_HS:
        case WT_VERB_HS_ACTIVITY:
            return logv2::LogComponent::kWiredTigerHS;
        case WT_VERB_LIVE_RESTORE:
        case WT_VERB_LIVE_RESTORE_PROGRESS:
            return logv2::LogComponent::kWiredTigerLiveRestore;
        case WT_VERB_RECOVERY:
        case WT_VERB_RECOVERY_PROGRESS:
            return logv2::LogComponent::kWiredTigerRecovery;
        case WT_VERB_RTS:
            return logv2::LogComponent::kWiredTigerRTS;
        case WT_VERB_SALVAGE:
            return logv2::LogComponent::kWiredTigerSalvage;
        case WT_VERB_TIERED:
            return logv2::LogComponent::kWiredTigerTiered;
        case WT_VERB_TIMESTAMP:
            return logv2::LogComponent::kWiredTigerTimestamp;
        case WT_VERB_TRANSACTION:
            return logv2::LogComponent::kWiredTigerTransaction;
        case WT_VERB_VERIFY:
            return logv2::LogComponent::kWiredTigerVerify;
        case WT_VERB_LOG:
            return logv2::LogComponent::kWiredTigerWriteLog;
        default:
            return logv2::LogComponent::kWiredTiger;
    }
}

namespace {

void logWTErrorMessage(int id, int errorCode, const std::string& message) {
    logv2::LogComponent component = logv2::LogComponent::kWiredTiger;
    logv2::DynamicAttributes attr;
    attr.add("error", errorCode);

    try {
        // Parse the WT JSON message string.
        BSONObj obj = fromjson(message);
        attr.add("message", obj);
        component = getWTLogComponent(obj);
    } catch (...) {
        // Fall back to default behaviour.
        attr.add("message", message);
    }
    LOGV2_ERROR_OPTIONS(id, logv2::LogOptions{component}, "WiredTiger error message", attr);
}

int mdb_handle_error_with_startup_suppression(WT_EVENT_HANDLER* handler,
                                              WT_SESSION* session,
                                              int errorCode,
                                              const char* message) {
    WiredTigerEventHandler* wtHandler = reinterpret_cast<WiredTigerEventHandler*>(handler);

    try {
        StringData sd(message);
        if (!wtHandler->wasStartupSuccessful()) {
            // During startup, storage tries different WiredTiger compatibility modes to determine
            // the state of the data files before FCV can be read. Suppress the error messages
            // regarding expected version compatibility requirements.
            if (sd.find("Version incompatibility detected:") != std::string::npos) {
                return 0;
            }

            // WT shipped with MongoDB 4.4 can read data left behind by 4.0, but cannot write 4.0
            // compatible data. Instead of forcing an upgrade on the user, it refuses to start up
            // with this error string.
            if (sd.find("WiredTiger version incompatible with current binary") !=
                std::string::npos) {
                wtHandler->setWtIncompatible();
                return 0;
            }
        }

        logWTErrorMessage(22435, errorCode, message);

        // Don't abort on WT_PANIC when repairing, as the error will be handled at a higher layer.
        if (storageGlobalParams.repair) {
            return 0;
        }
        fassert(50853, errorCode != WT_PANIC);
    } catch (...) {
        std::terminate();
    }
    return 0;
}

int mdb_handle_error(WT_EVENT_HANDLER* handler,
                     WT_SESSION* session,
                     int errorCode,
                     const char* message) {
    try {
        logWTErrorMessage(22436, errorCode, std::string(redact(message)));

        // Don't abort on WT_PANIC when repairing, as the error will be handled at a higher layer.
        if (storageGlobalParams.repair) {
            return 0;
        }
        fassert(28558, errorCode != WT_PANIC);
    } catch (...) {
        std::terminate();
    }
    return 0;
}

int mdb_handle_message(WT_EVENT_HANDLER* handler, WT_SESSION* session, const char* message) {
    logv2::DynamicAttributes attr;
    logv2::LogSeverity severity = ::mongo::logv2::LogSeverity::Log();
    logv2::LogOptions options = ::mongo::logv2::LogOptions{MONGO_LOGV2_DEFAULT_COMPONENT};

    try {
        try {
            // Parse the WT JSON message string.
            const BSONObj obj = fromjson(message);
            severity = getWTLogSeverityLevel(obj);
            options = logv2::LogOptions{getWTLogComponent(obj)};
            attr.add("message", redact(obj));
        } catch (...) {
            // Fall back to default behaviour.
            attr.add("message", redact(message));
        }

        LOGV2_IMPL(22430, severity, options, "WiredTiger message", attr);
    } catch (...) {
        std::terminate();
    }
    return 0;
}

int mdb_handle_progress(WT_EVENT_HANDLER* handler,
                        WT_SESSION* session,
                        const char* operation,
                        uint64_t progress) {
    try {
        LOGV2(22431,
              "WiredTiger progress",
              "operation"_attr = redact(operation),
              "progress"_attr = progress);
    } catch (...) {
        std::terminate();
    }

    return 0;
}

/**
 * Defines a callback function that can be passed via a WT_EVENT_HANDLER*
 * (WT_EVENT_HANDLER::handle_general) into WT::wiredtiger_open() call.
 *
 * The void* WT_SESSION::app_private is leveraged to inject MDB state into the WT code layer.
 * Long running WT::compact operations will periodically use this callback function to check whether
 * or not to quit early and fail the WT::compact operation.
 */
int mdb_handle_general(WT_EVENT_HANDLER* handler,
                       WT_CONNECTION* wt_conn,
                       WT_SESSION* session,
                       WT_EVENT_TYPE type,
                       void* arg) {

    WiredTigerEventHandler* wtHandler = reinterpret_cast<WiredTigerEventHandler*>(handler);

    switch (type) {
        case WT_EVENT_CONN_READY: {
            wtHandler->setWtConnReady(wt_conn);
            return 0;
        }
        case WT_EVENT_CONN_CLOSE: {
            wtHandler->setWtConnNotReady();
            return 0;
        }
        case WT_EVENT_COMPACT_CHECK: {
            if (session->app_private) {
                return reinterpret_cast<OperationContext*>(session->app_private)
                           ->checkForInterruptNoAssert()
                           .isOK()
                    ? 0
                    : -1;  // Returning non-zero indicates an error to WT. The precise value is
                           // irrelevant.
            }
            return 0;
        }
        case WT_EVENT_EVICTION: {
            return WiredTigerUtil::handleWtEvictionEvent(session);
        }
        default: {
            return 0;
        }
    }
    MONGO_UNREACHABLE;
}

WT_EVENT_HANDLER defaultEventHandlers() {
    WT_EVENT_HANDLER handlers = {};
    handlers.handle_error = mdb_handle_error;
    handlers.handle_message = mdb_handle_message;
    handlers.handle_progress = mdb_handle_progress;
    handlers.handle_general = mdb_handle_general;
    return handlers;
}
}  // namespace

WiredTigerEventHandler::WiredTigerEventHandler() {
    WT_EVENT_HANDLER* handler = static_cast<WT_EVENT_HANDLER*>(this);
    invariant((void*)this == (void*)handler);

    handler->handle_error = mdb_handle_error_with_startup_suppression;
    handler->handle_message = mdb_handle_message;
    handler->handle_progress = mdb_handle_progress;
    handler->handle_close = nullptr;
    handler->handle_general = mdb_handle_general;
}

WT_EVENT_HANDLER* WiredTigerEventHandler::getWtEventHandler() {
    WT_EVENT_HANDLER* ret = static_cast<WT_EVENT_HANDLER*>(this);
    invariant((void*)this == (void*)ret);

    return ret;
}

WiredTigerUtil::ErrorAccumulator::ErrorAccumulator(StringSet* errors)
    : WT_EVENT_HANDLER(defaultEventHandlers()),
      _errors(errors),
      _defaultErrorHandler(handle_error) {
    if (errors) {
        handle_error = onError;
    }
}

// static
int WiredTigerUtil::ErrorAccumulator::onError(WT_EVENT_HANDLER* handler,
                                              WT_SESSION* session,
                                              int error,
                                              const char* message) {
    try {
        ErrorAccumulator* self = static_cast<ErrorAccumulator*>(handler);
        self->_errors->insert(message);
        return self->_defaultErrorHandler(handler, session, error, message);
    } catch (...) {
        std::terminate();
    }
}

int WiredTigerUtil::verifyTable(WiredTigerSession& session,
                                const std::string& uri,
                                const boost::optional<std::string>& configurationOverride,
                                StringSet* errors) {
    ErrorAccumulator eventHandler(errors);

    // Try to close as much as possible to avoid EBUSY errors.
    session.closeAllCursors(uri);
    WiredTigerConnection& connection = session.getConnection();

    // Open a new session with custom error handlers.
    const char* sessionConfig = nullptr;
    if (gFeatureFlagPrefetch.isEnabled() && !connection.isEphemeral()) {
        sessionConfig = "prefetch=(enabled=true)";
    }
    WiredTigerSession verifySession(&connection, &eventHandler, sessionConfig);

    const char* verifyConfig =
        configurationOverride.has_value() ? configurationOverride->c_str() : nullptr;
    // Do the verify. Weird parens prevent treating "verify" as a macro.
    return verifySession.verify(uri.c_str(), verifyConfig);
}

void WiredTigerUtil::validateTableLogging(WiredTigerSession& session,
                                          StringData uri,
                                          bool isLogged,
                                          boost::optional<StringData> indexName,
                                          ValidateResultsIf& validationResult) {
    if (gWiredTigerSkipTableLoggingChecksDuringValidation) {
        LOGV2(9264500,
              "Skipping validation of table log settings due to usage of "
              "'wiredTigerSkipTableLoggingChecksDuringValidation'",
              "ident"_attr = uri);
        return;
    }

    logv2::DynamicAttributes attrs;
    if (indexName) {
        attrs.add("index", indexName);
    }
    attrs.add("uri", uri);

    auto metadata = WiredTigerUtil::getSourceMetadata(session, uri);
    if (!metadata.isOK()) {
        attrs.add("error", metadata.getStatus());
        LOGV2_WARNING(6898100, "Failed to check WT table logging setting", attrs);

        validationResult.addWarning(fmt::format(
            "Failed to check WT table logging setting for {}",
            indexName ? fmt::format("index '{}'", std::string{*indexName}) : "collection"));

        return;
    }

    WiredTigerConfigParser metadataParser(metadata.getValue());
    auto currentSetting = metadataParser.isTableLoggingEnabled();
    if (!currentSetting || *currentSetting != isLogged) {
        attrs.add("expected", isLogged);
        LOGV2_ERROR(6898101, "Detected incorrect WT table logging setting", attrs);

        validationResult.addError(fmt::format(
            "Detected incorrect table logging setting for {}",
            indexName ? fmt::format("index '{}'", std::string{*indexName}) : "collection"));
    }
}

bool WiredTigerUtil::useTableLogging(const rss::PersistenceProvider& provider,
                                     const NamespaceString& nss,
                                     bool isReplSet,
                                     bool shouldRecoverFromOplogAsStandalone) {
    if (storageGlobalParams.forceDisableTableLogging) {
        invariant(TestingProctor::instance().isEnabled());
        LOGV2(6825405, "Table logging disabled", logAttrs(nss));
        return false;
    }

    // We only turn off logging in the case of:
    // 1) Replication is enabled (the typical deployment), or
    // 2) This is a disaggregated storage mongod, or
    // 3) We're running as a standalone with recoverFromOplogAsStandalone=true
    if (!provider.supportsTableLogging()) {
        return false;
    }

    const bool journalWritesBecauseStandalone = !isReplSet && !shouldRecoverFromOplogAsStandalone;
    if (journalWritesBecauseStandalone) {
        return true;
    }

    // Don't make assumptions if there is no namespace string.
    invariant(nss.size() > 0);

    // Of the replica set configurations:
    if (!nss.isLocalDB()) {
        // All replicated collections are not logged.
        return false;
    }

    if (nss.coll() == "replset.minvalid") {
        // Of local collections, this is derived from the state of the data and therefore
        // not logged.
        return false;
    }

    // The remainder of local gets logged. In particular, the oplog and user created
    // collections.
    return true;
}

Status WiredTigerUtil::setTableLogging(WiredTigerSession& session,
                                       const std::string& uri,
                                       bool on) {
    if (gWiredTigerSkipTableLoggingChecksOnStartup) {
        LOGV2_DEBUG(5548302, 1, "Skipping table logging check", "uri"_attr = uri);
        return Status::OK();
    }

    const std::string setting = on ? "log=(enabled=true)" : "log=(enabled=false)";

    // Try to close as much as possible to avoid EBUSY errors.
    session.closeAllCursors(uri);
    WiredTigerConnection& connection = session.getConnection();

    // This method uses the WiredTiger config parser to see if the table is in the expected logging
    // state. Only attempt to alter the table when a change is needed. This avoids grabbing heavy
    // locks in WT when creating new tables for collections and indexes. Those tables are created
    // with the proper settings and consequently should not be getting changed here.
    //
    // If the settings need to be changed (only expected at startup), the alter table call must
    // succeed.
    std::string existingMetadata;
    {
        auto managedSession = connection.getUninterruptibleSession();
        auto metadata = getSourceMetadata(*managedSession, uri);
        if (!metadata.isOK()) {
            return metadata.getStatus();
        }
        existingMetadata = std::move(metadata.getValue());
    }

    {
        WiredTigerConfigParser existingMetadataParser(existingMetadata);
        auto currentSetting = existingMetadataParser.isTableLoggingEnabled();
        if (currentSetting && *currentSetting == on) {
            // The table is running with the expected logging settings.
            return Status::OK();
        }
    }

    LOGV2_DEBUG(
        22432, 1, "Changing table logging settings", "uri"_attr = uri, "loggingEnabled"_attr = on);
    // Only alter the metadata once we're sure that we need to change the table settings, since
    // WT_SESSION::alter may return EBUSY and require taking a checkpoint to make progress.
    auto status = connection.getKVEngine()->alterMetadata(uri, setting);
    if (!status.isOK()) {
        // Dump the storage engine's internal state to assist in diagnosis.
        connection.getKVEngine()->dump();

        LOGV2_FATAL(50756,
                    "Failed to update log setting",
                    "uri"_attr = uri,
                    "loggingEnabled"_attr = on,
                    "error"_attr = status.code(),
                    "metadata"_attr = redact(existingMetadata),
                    "message"_attr = status.reason());
    }

    return Status::OK();
}

static int mdb_handle_error_for_statistics(WT_EVENT_HANDLER* handler,
                                           WT_SESSION* session,
                                           int errorCode,
                                           const char* message) {
    if (errorCode == EINVAL) {
        // suppressing error "cannot open a non-statistics cursor before connection is opened" error
        try {
            // Parse the WT JSON message string.
            if (BSONObj obj = fromjson(message); obj.getStringField("msg").ends_with(
                    "cannot open a non-statistics cursor before connection is opened")) {
                return 0;
            }
        } catch (...) {
            // Fall back to default behaviour.
        }
    }
    // If it is not the error above we delegate to the normal error handling.
    return mdb_handle_error(handler, session, errorCode, message);
}

std::unique_ptr<WiredTigerSession> WiredTigerUtil::getStatisticsSession(
    WiredTigerKVEngineBase& engine,
    StatsCollectionPermit& permit,
    WiredTigerEventHandler& eventHandler) {
    // Silence some errors when trying to get statistics during startup
    auto handler = eventHandler.getWtEventHandler();
    handler->handle_error = mdb_handle_error_for_statistics;

    // Obtain a session that can be used during shut down, potentially before the storage engine
    // itself shuts down.
    return std::make_unique<WiredTigerSession>(&engine.getConnection(), handler, permit);
}

bool WiredTigerUtil::collectConnectionStatistics(WiredTigerKVEngineBase& engine,
                                                 BSONObjBuilder& bob,
                                                 const std::vector<std::string>& fieldsToInclude) {
    boost::optional<StatsCollectionPermit> permit = engine.tryGetStatsCollectionPermit();
    if (!permit) {
        return false;
    }

    WiredTigerEventHandler eventHandler;
    auto session = getStatisticsSession(engine, *permit, eventHandler);

    // Filter out irrelevant statistic fields.
    std::vector<std::string> categoriesToIgnore = {"LSM"};

    Status status = WiredTigerUtil::exportTableToBSON(
        *session,
        "statistics:",
        "statistics=(fast)",
        bob,
        fieldsToInclude.empty() ? categoriesToIgnore : fieldsToInclude,
        fieldsToInclude.empty() ? FilterBehavior::kExcludeCategories
                                : FilterBehavior::kIncludeStats);
    if (!status.isOK()) {
        bob.append("error", "unable to retrieve statistics");
        bob.append("code", static_cast<int>(status.code()));
        bob.append("reason", status.reason());
    }
    return true;
}

bool WiredTigerUtil::historyStoreStatistics(WiredTigerKVEngine& engine, BSONObjBuilder& bob) {
    // History Storage does not exists on the in Memory storage.
    if (engine.isEphemeral()) {
        return false;
    }

    boost::optional<StatsCollectionPermit> permit = engine.tryGetStatsCollectionPermit();
    if (!permit) {
        return false;
    }

    WiredTigerEventHandler eventHandler;
    auto session = getStatisticsSession(engine, *permit, eventHandler);

    const auto historyStorageStatUri = "statistics:file:WiredTigerHS.wt";

    Status status = WiredTigerUtil::exportTableToBSON(
        *session, historyStorageStatUri, "statistics=(fast)", bob);
    if (!status.isOK()) {
        bob.append("error", "unable to retrieve statistics");
        bob.append("code", static_cast<int>(status.code()));
        bob.append("reason", status.reason());
    }
    return true;
}

Status WiredTigerUtil::exportTableToBSON(WiredTigerSession& session,
                                         const std::string& uri,
                                         const std::string& config,
                                         BSONObjBuilder& bob) {
    return exportTableToBSON(session, uri, config, bob, {}, FilterBehavior::kExcludeCategories);
}

Status WiredTigerUtil::exportTableToBSON(WiredTigerSession& session,
                                         const std::string& uri,
                                         const std::string& config,
                                         BSONObjBuilder& bob,
                                         const std::vector<std::string>& filter,
                                         FilterBehavior filterBehavior) {
    WT_CURSOR* cursor = nullptr;
    const char* cursorConfig = config.empty() ? nullptr : config.c_str();

    // Attempt to open a statistics cursor on the provided URI.
    int ret = session.open_cursor(uri.c_str(), nullptr, cursorConfig, &cursor);
    if (ret != 0) {
        return Status(ErrorCodes::CursorNotFound,
                      str::stream() << "unable to open cursor at URI " << uri
                                    << ". reason: " << wiredtiger_strerror(ret));
    }
    bob.append("uri", uri);
    bob.append("version", wiredtiger_version(NULL, NULL, NULL));
    invariant(cursor);
    ON_BLOCK_EXIT([&] { cursor->close(cursor); });

    // measurementsMappedByCategory is used to organize the table's nested BSON structure as we
    // iterate through the WT table. We keep track of each category's measurements in this format:
    // { "cache":
    //       {"bytes read into cache": 30.000, "bytes written from cache": 20.000, ...},
    //   "category":
    //       {"measurement1": value, "measurement2": value, ...},
    //   ...
    // }
    std::map<string, BSONObjBuilder> measurementsMappedByCategory;
    const char* statisticDescription;
    uint64_t statisticValue;
    while (cursor->next(cursor) == 0 &&
           cursor->get_value(cursor, &statisticDescription, nullptr, &statisticValue) == 0) {
        // The description returned by the statistics cursor of the format: "Category: Measurement",
        // like "cache: bytes read into cache" or "cache: bytes written from cache".
        StringData key(statisticDescription);
        StringData category;
        StringData measurement;

        // Attempt to split the description into the category and measurement if a category is
        // provided. Otherwise, attempt to use the first word of the description as the category.
        size_t idx = key.find_first_of(": ");
        if (idx != string::npos) {
            category = key.substr(0, idx);
            measurement = key.substr(idx + 1);
        } else {
            category = key;
            measurement = "num";
        }

        long long value = castStatisticsValue<long long>(statisticValue);

        switch (filterBehavior) {
            case FilterBehavior::kExcludeCategories: {
                if (category.size() == 0) {
                    bob.appendNumber(statisticDescription, value);
                    break;
                }

                bool shouldSkipField =
                    std::find(filter.begin(), filter.end(), category) != filter.end();
                if (shouldSkipField) {
                    continue;
                }

                measurementsMappedByCategory[std::string{category}].appendNumber(
                    str::ltrim(std::string{measurement}), value);
                break;
            }
            case FilterBehavior::kIncludeStats: {
                bool shouldIncludeField =
                    std::find(filter.begin(), filter.end(), statisticDescription) != filter.end();
                if (shouldIncludeField) {
                    measurementsMappedByCategory[std::string{category}].appendNumber(
                        str::ltrim(std::string{measurement}), value);
                }
                break;
            }
        }
    }

    // Attach the table statistics to the BSONObjBuilder provided in the function arguments.
    for (std::map<string, BSONObjBuilder>::iterator it = measurementsMappedByCategory.begin();
         it != measurementsMappedByCategory.end();
         ++it) {
        const std::string& category = it->first;
        bob.append(category, it->second.obj());
    }
    return Status::OK();
}

StatusWith<std::string> WiredTigerUtil::generateImportString(StringData ident,
                                                             const BSONObj& storageMetadata,
                                                             bool panicOnCorruptWtMetadata,
                                                             bool repair) {

    if (!storageMetadata.hasField(ident)) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Missing the storage metadata for ident " << ident << " in "
                                    << redact(storageMetadata));
    }

    if (storageMetadata.getField(ident).type() != BSONType::object) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "The storage metadata for ident " << ident
                                    << " is not of type object but is of type "
                                    << storageMetadata.getField(ident).type() << " in "
                                    << redact(storageMetadata));
    }

    const BSONObj& identMd = storageMetadata.getField(ident).Obj();
    if (!identMd.hasField("tableMetadata") || !identMd.hasField("fileMetadata")) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream()
                          << "The storage metadata for ident " << ident
                          << " is missing either the 'tableMetadata' or 'fileMetadata' field in "
                          << redact(storageMetadata));
    }

    const BSONElement tableMetadata = identMd.getField("tableMetadata");
    const BSONElement fileMetadata = identMd.getField("fileMetadata");

    if (tableMetadata.type() != BSONType::string || fileMetadata.type() != BSONType::string) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "The storage metadata for ident " << ident
                                    << " is not of type string for either the 'tableMetadata' or "
                                       "'fileMetadata' field in "
                                    << redact(storageMetadata));
    }

    std::stringstream ss;
    ss << tableMetadata.String();
    ss << ",import=(enabled=true,repair=false,";

    if (!panicOnCorruptWtMetadata) {
        invariant(!repair);
        ss << "panic_corrupt=false,";
    }
    if (repair) {
        ss << "repair=true,";
    }
    ss << "file_metadata=(" << fileMetadata.String() << "))";

    return StatusWith<std::string>(ss.str());
}

std::string WiredTigerUtil::generateRestoreConfig() {
    std::stringstream ss;
    ss << "backup_restore_target=[";

    const auto dbpath = boost::filesystem::path(storageGlobalParams.dbpath);
    for (const auto& entry : boost::filesystem::recursive_directory_iterator(dbpath)) {
        if (boost::filesystem::is_directory(entry)) {
            continue;
        }

        if (entry.path().extension() != kTableExtension) {
            continue;
        }

        // Skip WiredTiger metadata files with the ".wt" extension.
        const std::string filename = entry.path().filename().string();
        if (filename == "WiredTiger.wt" || filename == "WiredTigerHS.wt") {
            continue;
        }

        boost::filesystem::path relativePath =
            boost::filesystem::relative(entry.path(), dbpath).parent_path();
        relativePath /= entry.path().stem();

        ss << "\"table:" << relativePath.string() << "\",";
    }

    ss << "]";

    return ss.str();
}

bool WiredTigerUtil::willRestoreFromBackup() {
    if (!(storageGlobalParams.restore || storageGlobalParams.magicRestore)) {
        return false;
    }

    const auto dbpath = boost::filesystem::path(storageGlobalParams.dbpath);
    return boost::filesystem::exists(dbpath / kWiredTigerBackupFile);
}

void WiredTigerUtil::appendSnapshotWindowSettings(WiredTigerKVEngine* engine, BSONObjBuilder* bob) {
    invariant(engine);
    invariant(bob);

    const Timestamp& stableTimestamp = engine->getStableTimestamp();
    const Timestamp& oldestTimestamp = engine->getOldestTimestamp();

    const unsigned currentAvailableSnapshotWindow =
        stableTimestamp.getSecs() - oldestTimestamp.getSecs();

    auto totalNumberOfSnapshotTooOldErrors = snapshotTooOldErrorCount.load();

    BSONObjBuilder settings(bob->subobjStart("snapshot-window-settings"));
    settings.append("total number of SnapshotTooOld errors", totalNumberOfSnapshotTooOldErrors);
    settings.append("minimum target snapshot window size in seconds",
                    minSnapshotHistoryWindowInSeconds.load());
    settings.append("current available snapshot window size in seconds",
                    static_cast<int>(currentAvailableSnapshotWindow));
    settings.append("latest majority snapshot timestamp available",
                    stableTimestamp.toStringPretty());
    settings.append("oldest majority snapshot timestamp available",
                    oldestTimestamp.toStringPretty());

    std::map<std::string, Timestamp> pinnedTimestamps = engine->getPinnedTimestampRequests();
    settings.append("pinned timestamp requests", static_cast<int>(pinnedTimestamps.size()));

    Timestamp minPinned = Timestamp::max();
    for (const auto& it : pinnedTimestamps) {
        minPinned = std::min(minPinned, it.second);
    }
    settings.append("min pinned timestamp", minPinned);
}

std::string WiredTigerUtil::generateWTVerboseConfiguration() {
    // Mapping between LOGV2 WiredTiger components and their WiredTiger verbose setting counterpart.
    static const StaticImmortal wtVerboseComponents = std::map<logv2::LogComponent, std::string>{
        {logv2::LogComponent::kWiredTigerBackup, "backup"},
        {logv2::LogComponent::kWiredTigerCheckpoint, "checkpoint"},
        {logv2::LogComponent::kWiredTigerCompact, "compact"},
        {logv2::LogComponent::kWiredTigerEviction, "eviction"},
        {logv2::LogComponent::kWiredTigerFileOps, "fileops"},
        {logv2::LogComponent::kWiredTigerHS, "history_store"},
        {logv2::LogComponent::kWiredTigerLiveRestore, "live_restore"},
        {logv2::LogComponent::kWiredTigerRecovery, "recovery"},
        {logv2::LogComponent::kWiredTigerRTS, "rts"},
        {logv2::LogComponent::kWiredTigerSalvage, "salvage"},
        {logv2::LogComponent::kWiredTigerTiered, "tiered"},
        {logv2::LogComponent::kWiredTigerTimestamp, "timestamp"},
        {logv2::LogComponent::kWiredTigerTransaction, "transaction"},
        {logv2::LogComponent::kWiredTigerVerify, "verify"},
        {logv2::LogComponent::kWiredTigerWriteLog, "log"},
    };

    str::stream cfg;

    // Define the verbose level for each component.
    cfg << "verbose=[";

    // Enable WiredTiger progress messages.
    cfg << "recovery_progress:1,checkpoint_progress:1,compact_progress:1,live_restore_progress:1";

    // Process each LOGV2 WiredTiger component and set the desired verbosity level.
    for (const auto& [component, componentStr] : *wtVerboseComponents) {
        auto severity =
            logv2::LogManager::global().getGlobalSettings().getMinimumLogSeverity(component);

        cfg << ",";

        int level;
        switch (severity.toInt()) {
            case logv2::LogSeverity::Debug(1).toInt():
                level = WT_VERBOSE_DEBUG_1;
                break;
            case logv2::LogSeverity::Debug(2).toInt():
                level = WT_VERBOSE_DEBUG_2;
                break;
            case logv2::LogSeverity::Debug(3).toInt():
                level = WT_VERBOSE_DEBUG_3;
                break;
            case logv2::LogSeverity::Debug(4).toInt():
                level = WT_VERBOSE_DEBUG_4;
                break;
            case logv2::LogSeverity::Debug(5).toInt():
                level = WT_VERBOSE_DEBUG_5;
                break;
            default:
                level = WT_VERBOSE_INFO;
                break;
        }

        cfg << componentStr << ":" << level;
    }

    cfg << "]";

    return cfg;
}

// static
boost::optional<std::string> WiredTigerUtil::getConfigStringFromStorageOptions(
    const BSONObj& options) {
    if (auto wtElem = options[kWiredTigerEngineName]) {
        BSONObj wtObj = wtElem.Obj();
        if (auto configStringElem = wtObj.getField(kConfigStringField)) {
            return configStringElem.String();
        }
    }

    return boost::none;
}

// static
BSONObj WiredTigerUtil::setConfigStringToStorageOptions(const BSONObj& options,
                                                        const std::string& configString) {
    // Storage options may contain settings for non-WiredTiger storage engines (e.g. inMemory).
    // We should leave these settings intact.
    auto wtElem = options[kWiredTigerEngineName];
    auto wtObj = wtElem ? wtElem.Obj() : BSONObj();
    return options.addFields(
        BSON(kWiredTigerEngineName << wtObj.addFields(BSON(kConfigStringField << configString))));
}

void WiredTigerUtil::removeEncryptionFromConfigString(std::string* configString) {
    encryptionOptsRegex->substitute("", configString, pcre::SUBSTITUTE_GLOBAL);
}

// static
BSONObj WiredTigerUtil::getSanitizedStorageOptionsForSecondaryReplication(const BSONObj& options) {
    auto configString = getConfigStringFromStorageOptions(options);
    if (!configString) {
        return options;
    }

    removeEncryptionFromConfigString(configString.get_ptr());
    return setConfigStringToStorageOptions(options, *configString);
}

Status WiredTigerUtil::canRunAutoCompact(bool isEphemeral) {
    if (!gFeatureFlagAutoCompact.isEnabled()) {
        return Status(ErrorCodes::IllegalOperation,
                      "autoCompact() requires its feature flag to be enabled");
    }
    if (isEphemeral) {
        return Status(ErrorCodes::IllegalOperation,
                      "autoCompact() cannot be executed for in-memory configurations");
    }
    if (storageGlobalParams.syncdelay.load() == 0) {
        return Status(ErrorCodes::IllegalOperation,
                      "autoCompact() can only be executed when checkpoints are enabled");
    }
    return Status::OK();
}

// static
uint64_t WiredTigerUtil::genTableId() {
    static AtomicWord<unsigned long long> nextTableId(WiredTigerUtil::kLastTableId);
    return nextTableId.fetchAndAdd(1);
}

boost::optional<bool> WiredTigerConfigParser::isTableLoggingEnabled() const {
    WT_CONFIG_ITEM value;
    if (auto retCode = get(kLogKeyName.data(), &value); retCode != 0) {
        invariant(
            retCode == WT_NOTFOUND,
            fmt::format("expected WT_NOTFOUND from WT_CONFIG_PARSER::get() but got {} instead",
                        retCode));
        return boost::none;
    }

    if (value.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT) {
        return boost::none;
    }

    WiredTigerConfigParser logSettingParser(value);
    WT_CONFIG_ITEM enabledValue;
    // Not taking into consideration multiple "enabled" sub-keys within the "log" struct.
    if (auto retCode = logSettingParser.get(kLogEnabledKeyName.data(), &enabledValue);
        retCode != 0) {
        invariant(retCode == WT_NOTFOUND,
                  fmt::format("expected WT_NOTFOUND from WT_CONFIG_PARSER::get() but got {} "
                              "instead. Log key value: {}",
                              retCode,
                              StringData{value.str, value.len}));
        return boost::none;
    }

    if (enabledValue.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_BOOL) {
        return boost::none;
    }

    return enabledValue.val;
}

int WiredTigerUtil::handleWtEvictionEvent(WT_SESSION* session) {
    if (!session->app_private) {
        return 0;
    }
    auto opCtx = reinterpret_cast<OperationContext*>(session->app_private);

    if (feature_flags::gStorageEngineInterruptibility.isEnabled() &&
        !opCtx->checkForInterruptNoAssert().isOK()) {
        auto& metrics = StorageExecutionContext::get(opCtx)->getStorageMetrics();
        // Check killTime as it can be 0 for a short window when interrupted during shutdown.
        auto killTime = opCtx->getKillTime();
        if (!metrics.interruptResponseNs.load() && killTime) {
            auto ts = opCtx->getServiceContext()->getTickSource();
            auto duration =
                durationCount<Nanoseconds>(ts->ticksTo<Nanoseconds>(ts->getTicks() - killTime));
            metrics.incrementInterruptResponseNs(duration);
        }
        cancelledCacheMetric.increment();
        return -1;
    }

    return 0;
}

long long WiredTigerUtil::getCancelledCacheMetric_forTest() {
    return cancelledCacheMetric.get();
}

void WiredTigerUtil::truncate(WiredTigerRecoveryUnit& ru, StringData uri) {
    invariantWTOK(WT_OP_CHECK(ru.getSession()->truncate(uri.data(), nullptr, nullptr, nullptr)),
                  *ru.getSession());
}

}  // namespace mongo
