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

#include <absl/container/node_hash_set.h>
#include <boost/cstdint.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/move/utility_core.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "boost/system/detail/error_code.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <new>
#include <ostream>
#include <stdexcept>
#include <wiredtiger.h>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/exception_util_gen.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/snapshot_window_options_gen.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_parameters_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/pcre.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/testing_proctor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWiredTiger

// From src/third_party/wiredtiger/src/include/txn.h
#define WT_TXN_ROLLBACK_REASON_OLDEST_FOR_EVICTION \
    "oldest pinned transaction ID rolled back for eviction"

#define WT_TXN_ROLLBACK_REASON_TOO_LARGE_FOR_CACHE \
    "transaction is too large and will not fit in the storage engine cache"
namespace mongo {

void WiredTigerEventHandler::setWtConnReadyStatus(bool status) {
    stdx::unique_lock<mongo::Mutex> lock(_mutex);
    _wtConnReady = status;
    if (_activeSections == 0 || _wtConnReady) {
        return;
    }
    LOGV2(7003100,
          "WiredTiger connection close is waiting for active statistics readers to finish",
          "activeReaders"_attr = _activeSections);
    _idleCondition.wait(lock, [this]() { return _activeSections != 0; });
}

bool WiredTigerEventHandler::getSectionActivityPermit() {
    stdx::lock_guard<mongo::Mutex> lock(_mutex);
    if (_wtConnReady) {
        _activeSections++;
        return true;
    }
    return false;
}

void WiredTigerEventHandler::releaseSectionActivityPermit() {
    stdx::unique_lock<mongo::Mutex> lock(_mutex);
    _activeSections--;
    if (_activeSections == 0 && !_wtConnReady) {
        _idleCondition.notify_all();
        return;
    }
}

namespace {

// TODO SERVER-81069: Remove this.
MONGO_FAIL_POINT_DEFINE(allowEncryptionOptionsInCreationString);

const std::string kTableChecksFileName = "_wt_table_checks";
const std::string kTableExtension = ".wt";
const std::string kWiredTigerBackupFile = "WiredTiger.backup";
const static StaticImmortal<pcre::Regex> encryptionOptsRegex(R"re(encryption=\([^\)]*\),?)re");

/**
 * Removes the 'kTableChecksFileName' file in the dbpath, if it exists.
 */
void removeTableChecksFile() {
    auto path = boost::filesystem::path(storageGlobalParams.dbpath) /
        boost::filesystem::path(kTableChecksFileName);

    if (!boost::filesystem::exists(path)) {
        return;
    }

    boost::system::error_code errorCode;
    boost::filesystem::remove(path, errorCode);

    if (errorCode) {
        LOGV2_FATAL_NOTRACE(4366403,
                            "Failed to remove file",
                            "file"_attr = path.generic_string(),
                            "error"_attr = errorCode.message());
    }
}

}  // namespace

using std::string;

bool wasRollbackReasonCachePressure(WT_SESSION* session) {
    if (session) {
        const auto reason = session->get_rollback_reason(session);
        if (reason) {
            return strncmp(WT_TXN_ROLLBACK_REASON_OLDEST_FOR_EVICTION,
                           reason,
                           sizeof(WT_TXN_ROLLBACK_REASON_OLDEST_FOR_EVICTION)) == 0;
        }
    }
    return false;
}

/**
 * Configured WT cache is deemed insufficient for a transaction when its dirty bytes in cache
 * exceed a certain threshold on the proportion of total cache which is used by transaction.
 *
 * For instance, if the transaction uses 80% of WT cache and the threshold is set to 75%, the
 * transaction is considered too large.
 */
bool isCacheInsufficientForTransaction(WT_SESSION* session, double threshold) {
    StatusWith<int64_t> txnDirtyBytes = WiredTigerUtil::getStatisticsValue(
        session, "statistics:session", "", WT_STAT_SESSION_TXN_BYTES_DIRTY);
    if (!txnDirtyBytes.isOK()) {
        tasserted(6190900,
                  str::stream() << "unable to gather the WT session's txn dirty bytes: "
                                << txnDirtyBytes.getStatus());
    }

    StatusWith<int64_t> cacheDirtyBytes = WiredTigerUtil::getStatisticsValue(
        session, "statistics:", "", WT_STAT_CONN_CACHE_BYTES_DIRTY);
    if (!cacheDirtyBytes.isOK()) {
        tasserted(6190901,
                  str::stream() << "unable to gather the WT connection's cache dirty bytes: "
                                << txnDirtyBytes.getStatus());
    }


    double txnBytesDirtyOverCacheBytesDirty =
        static_cast<double>(txnDirtyBytes.getValue()) / cacheDirtyBytes.getValue();

    LOGV2_DEBUG(6190902,
                2,
                "Checking if transaction can eventually succeed",
                "txnDirtyBytes"_attr = txnDirtyBytes.getValue(),
                "cacheDirtyBytes"_attr = cacheDirtyBytes.getValue(),
                "txnBytesDirtyOverCacheBytesDirty"_attr = txnBytesDirtyOverCacheBytesDirty,
                "threshold"_attr = threshold);

    return txnBytesDirtyOverCacheBytesDirty > threshold;
}

Status wtRCToStatus_slow(int retCode, WT_SESSION* session, StringData prefix) {
    if (retCode == 0)
        return Status::OK();

    const auto generateContextStrStream = [&](StringData reason) {
        str::stream contextStrStream;
        if (!prefix.empty())
            contextStrStream << prefix << " ";
        contextStrStream << retCode << ": " << reason;

        return contextStrStream;
    };

    if (retCode == WT_ROLLBACK) {
        double cacheThreshold = gTransactionTooLargeForCacheThreshold.load();
        bool txnTooLargeEnabled = cacheThreshold < 1.0;
        bool temporarilyUnavailableEnabled = gEnableTemporarilyUnavailableExceptions.load();
        bool reasonWasCachePressure = (txnTooLargeEnabled || temporarilyUnavailableEnabled) &&
            wasRollbackReasonCachePressure(session);

        if (reasonWasCachePressure) {
            if (txnTooLargeEnabled && isCacheInsufficientForTransaction(session, cacheThreshold)) {
                throwTransactionTooLargeForCache(
                    generateContextStrStream(WT_TXN_ROLLBACK_REASON_TOO_LARGE_FOR_CACHE));
            }

            if (temporarilyUnavailableEnabled) {
                throwTemporarilyUnavailableException(
                    generateContextStrStream(WT_TXN_ROLLBACK_REASON_OLDEST_FOR_EVICTION));
            }
        }

        throwWriteConflictException(prefix);
    }

    // Don't abort on WT_PANIC when repairing, as the error will be handled at a higher layer.
    fassert(28559, retCode != WT_PANIC || storageGlobalParams.repair);

    auto s = generateContextStrStream(wiredtiger_strerror(retCode));

    if (retCode == EINVAL) {
        return Status(ErrorCodes::BadValue, s);
    }
    if (retCode == EMFILE) {
        return Status(ErrorCodes::TooManyFilesOpen, s);
    }
    if (retCode == EBUSY) {
        return Status(ErrorCodes::ObjectIsBusy, s);
    }

    uassert(ErrorCodes::ExceededMemoryLimit, s, retCode != WT_CACHE_FULL);

    // TODO convert specific codes rather than just using UNKNOWN_ERROR for everything.
    return Status(ErrorCodes::UnknownError, s);
}

void WiredTigerUtil::fetchTypeAndSourceURI(WiredTigerRecoveryUnit& ru,
                                           const std::string& tableUri,
                                           std::string* type,
                                           std::string* source) {
    std::string colgroupUri = "colgroup";
    const size_t colon = tableUri.find(':');
    invariant(colon != string::npos);
    colgroupUri += tableUri.substr(colon);
    StatusWith<std::string> colgroupResult = getMetadataCreate(ru, colgroupUri);
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

namespace {
StatusWith<std::string> _getMetadata(WT_CURSOR* cursor, StringData uri) {
    std::string strUri = uri.toString();
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
}  // namespace

StatusWith<std::string> WiredTigerUtil::getMetadataCreate(WT_SESSION* session, StringData uri) {
    WT_CURSOR* cursor;
    invariantWTOK(session->open_cursor(session, "metadata:create", nullptr, "", &cursor), session);
    invariant(cursor);
    ON_BLOCK_EXIT([cursor, session] { invariantWTOK(cursor->close(cursor), session); });

    return _getMetadata(cursor, uri);
}

StatusWith<std::string> WiredTigerUtil::getMetadataCreate(WiredTigerRecoveryUnit& ru,
                                                          StringData uri) {
    auto session = ru.getSessionNoTxn();

    WT_CURSOR* cursor = nullptr;
    try {
        const std::string metadataURI = "metadata:create";
        cursor = session->getCachedCursor(WiredTigerSession::kMetadataCreateTableId, "");
        if (!cursor) {
            cursor = session->getNewCursor(metadataURI);
        }
    } catch (const ExceptionFor<ErrorCodes::CursorNotFound>& ex) {
        LOGV2_FATAL_NOTRACE(51257, "Cursor not found", "error"_attr = ex);
    }
    invariant(cursor);
    ScopeGuard releaser = [&] {
        session->releaseCursor(WiredTigerSession::kMetadataCreateTableId, cursor, "");
    };

    return _getMetadata(cursor, uri);
}

StatusWith<std::string> WiredTigerUtil::getMetadata(WT_SESSION* session, StringData uri) {
    WT_CURSOR* cursor;
    invariantWTOK(session->open_cursor(session, "metadata:", nullptr, "", &cursor), session);
    invariant(cursor);
    ON_BLOCK_EXIT([cursor, session] { invariantWTOK(cursor->close(cursor), session); });

    return _getMetadata(cursor, uri);
}

StatusWith<std::string> WiredTigerUtil::getMetadata(WiredTigerRecoveryUnit& ru, StringData uri) {
    auto session = ru.getSessionNoTxn();
    WT_CURSOR* cursor = nullptr;
    try {
        const std::string metadataURI = "metadata:";
        cursor = session->getCachedCursor(WiredTigerSession::kMetadataTableId, "");
        if (!cursor) {
            cursor = session->getNewCursor(metadataURI);
        }
    } catch (const ExceptionFor<ErrorCodes::CursorNotFound>& ex) {
        LOGV2_FATAL_NOTRACE(31293, "Cursor not found", "error"_attr = ex);
    }
    invariant(cursor);
    ScopeGuard releaser = [&] {
        session->releaseCursor(WiredTigerSession::kMetadataTableId, cursor, "");
    };

    return _getMetadata(cursor, uri);
}

Status WiredTigerUtil::getApplicationMetadata(WiredTigerRecoveryUnit& ru,
                                              StringData uri,
                                              BSONObjBuilder* bob) {
    StatusWith<std::string> metadataResult = getMetadata(ru, uri);
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

StatusWith<BSONObj> WiredTigerUtil::getApplicationMetadata(WiredTigerRecoveryUnit& ru,
                                                           StringData uri) {
    BSONObjBuilder bob;
    Status status = getApplicationMetadata(ru, uri, &bob);
    if (!status.isOK()) {
        return StatusWith<BSONObj>(status);
    }
    return StatusWith<BSONObj>(bob.obj());
}

StatusWith<int64_t> WiredTigerUtil::checkApplicationMetadataFormatVersion(
    WiredTigerRecoveryUnit& ru, StringData uri, int64_t minimumVersion, int64_t maximumVersion) {
    StatusWith<std::string> result = getMetadata(ru, uri);
    if (result.getStatus().code() == ErrorCodes::NoSuchKey) {
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

    if (configElem.type() != String) {
        return {ErrorCodes::TypeMismatch, "'configString' must be a string."};
    }

    std::vector<std::string> errors;
    ErrorAccumulator eventHandler(&errors);

    StringData config = configElem.valueStringData();
    // Do NOT allow embedded null characters
    if (config.size() != strlen(config.rawData())) {
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
        wiredtiger_config_validate(nullptr, &eventHandler, "WT_SESSION.create", config.rawData()),
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
StatusWith<int64_t> WiredTigerUtil::getStatisticsValue(WT_SESSION* session,
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

int64_t WiredTigerUtil::getIdentSize(WT_SESSION* s, const std::string& uri) {
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

int64_t WiredTigerUtil::getEphemeralIdentSize(WT_SESSION* s, const std::string& uri) {
    // For ephemeral case, use cursor statistics
    const auto statsUri = "statistics:" + uri;

    // Helper function to retrieve stats and check for errors
    auto getStats = [&](int key) -> int64_t {
        auto result = getStatisticsValue(s, statsUri, "statistics=(fast)", key);
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

int64_t WiredTigerUtil::getIdentReuseSize(WT_SESSION* s, const std::string& uri) {
    auto result = WiredTigerUtil::getStatisticsValue(
        s, "statistics:" + uri, "statistics=(fast)", WT_STAT_DSRC_BLOCK_REUSE_BYTES);
    uassertStatusOK(result.getStatus());
    return result.getValue();
}

int64_t WiredTigerUtil::getIdentCompactRewrittenExpectedSize(WT_SESSION* s,
                                                             const std::string& uri) {
    auto result =
        WiredTigerUtil::getStatisticsValue(s,
                                           "statistics:" + uri,
                                           "statistics=(fast)",
                                           WT_STAT_DSRC_BTREE_COMPACT_BYTES_REWRITTEN_EXPECTED);
    uassertStatusOK(result.getStatus());
    return result.getValue();
}

size_t WiredTigerUtil::getCacheSizeMB(double requestedCacheSizeGB) {
    double cacheSizeMB;
    const double kMaxSizeCacheMB = 10 * 1000 * 1000;
    if (requestedCacheSizeGB == 0) {
        // Choose a reasonable amount of cache when not explicitly specified by user.
        // Set a minimum of 256MB, otherwise use 50% of available memory over 1GB.
        ProcessInfo pi;
        double memSizeMB = pi.getMemSizeMB();
        cacheSizeMB = std::max((memSizeMB - 1024) * 0.5, 256.0);
    } else {
        cacheSizeMB = 1024 * requestedCacheSizeGB;
    }
    if (cacheSizeMB > kMaxSizeCacheMB) {
        LOGV2(22429,
              "Requested cache size exceeds max, setting to maximum",
              "requestedMB"_attr = cacheSizeMB,
              "maximumMB"_attr = kMaxSizeCacheMB);
        cacheSizeMB = kMaxSizeCacheMB;
    }
    return static_cast<size_t>(cacheSizeMB);
}

logv2::LogSeverity getWTLOGV2SeverityLevel(const BSONObj& obj) {
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

logv2::LogComponent getWTLOGV2Component(const BSONObj& obj) {
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
        case WT_VERB_EVICT:
            return logv2::LogComponent::kWiredTigerEviction;
        case WT_VERB_HS:
        case WT_VERB_HS_ACTIVITY:
            return logv2::LogComponent::kWiredTigerHS;
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
        component = getWTLOGV2Component(obj);
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
            severity = getWTLOGV2SeverityLevel(obj);
            options = logv2::LogOptions{getWTLOGV2Component(obj)};
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
    if (type == WT_EVENT_CONN_READY) {
        wtHandler->setWtConnReadyStatus(true);
    } else if (type == WT_EVENT_CONN_CLOSE) {
        wtHandler->setWtConnReadyStatus(false);
    }
    if (type != WT_EVENT_COMPACT_CHECK || session == nullptr || session->app_private == nullptr) {
        return 0;
    }

    return reinterpret_cast<Interruptible*>(session->app_private)
               ->checkForInterruptNoAssert()
               .isOK()
        ? 0
        : -1;  // Returning non-zero indicates an error to WT. The precise value is irrelevant.
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

WiredTigerUtil::ErrorAccumulator::ErrorAccumulator(std::vector<std::string>* errors)
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
        self->_errors->push_back(message);
        return self->_defaultErrorHandler(handler, session, error, message);
    } catch (...) {
        std::terminate();
    }
}

int WiredTigerUtil::verifyTable(WiredTigerRecoveryUnit& ru,
                                const std::string& uri,
                                std::vector<std::string>* errors) {
    ErrorAccumulator eventHandler(errors);

    // Try to close as much as possible to avoid EBUSY errors.
    ru.getSession()->closeAllCursors(uri);
    WiredTigerSessionCache* sessionCache = ru.getSessionCache();
    sessionCache->closeAllCursors(uri);

    // Open a new session with custom error handlers.
    WT_CONNECTION* conn = ru.getSessionCache()->conn();
    WT_SESSION* session;
    invariantWTOK(conn->open_session(conn, &eventHandler, nullptr, &session), nullptr);
    ON_BLOCK_EXIT([&] { session->close(session, ""); });

    // Do the verify. Weird parens prevent treating "verify" as a macro.
    return (session->verify)(session, uri.c_str(), nullptr);
}

void WiredTigerUtil::validateTableLogging(WiredTigerRecoveryUnit& ru,
                                          StringData uri,
                                          bool isLogged,
                                          boost::optional<StringData> indexName,
                                          bool& valid,
                                          std::vector<std::string>& errors,
                                          std::vector<std::string>& warnings) {
    logv2::DynamicAttributes attrs;
    if (indexName) {
        attrs.add("index", indexName);
    }
    attrs.add("uri", uri);

    auto metadata = WiredTigerUtil::getMetadataCreate(ru, uri);
    if (!metadata.isOK()) {
        attrs.add("error", metadata.getStatus());
        LOGV2_WARNING(6898100, "Failed to check WT table logging setting", attrs);

        warnings.push_back(fmt::format("Failed to check WT table logging setting for {}",
                                       indexName ? fmt::format("index '{}'", indexName->toString())
                                                 : "collection"));

        return;
    }

    if (metadata.getValue().find(fmt::format("log=(enabled={})", isLogged ? "true" : "false")) ==
        std::string::npos) {
        attrs.add("expected", isLogged);
        LOGV2_ERROR(6898101, "Detected incorrect WT table logging setting", attrs);

        errors.push_back(fmt::format("Detected incorrect table logging setting for {}",
                                     indexName ? fmt::format("index '{}'", indexName->toString())
                                               : "collection"));
        valid = false;
    }
}

void WiredTigerUtil::notifyStorageStartupRecoveryComplete() {
    removeTableChecksFile();
}

bool WiredTigerUtil::useTableLogging(const NamespaceString& nss) {
    if (storageGlobalParams.forceDisableTableLogging) {
        invariant(TestingProctor::instance().isEnabled());
        LOGV2(6825405, "Table logging disabled", logAttrs(nss));
        return false;
    }

    // We only turn off logging in the case of:
    // 1) Replication is enabled (the typical deployment), or
    // 2) We're running as a standalone with recoverFromOplogAsStandalone=true
    const bool journalWritesBecauseStandalone = !getGlobalReplSettings().isReplSet() &&
        !repl::ReplSettings::shouldRecoverFromOplogAsStandalone();
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

Status WiredTigerUtil::setTableLogging(WiredTigerRecoveryUnit& ru,
                                       const std::string& uri,
                                       bool on) {
    if (gWiredTigerSkipTableLoggingChecksOnStartup) {
        LOGV2_DEBUG(5548302, 1, "Skipping table logging check", "uri"_attr = uri);
        return Status::OK();
    }

    const std::string setting = on ? "log=(enabled=true)" : "log=(enabled=false)";

    // Try to close as much as possible to avoid EBUSY errors.
    ru.getSession()->closeAllCursors(uri);
    WiredTigerSessionCache* sessionCache = ru.getSessionCache();
    sessionCache->closeAllCursors(uri);

    // This method does some "weak" parsing to see if the table is in the expected logging
    // state. Only attempt to alter the table when a change is needed. This avoids grabbing heavy
    // locks in WT when creating new tables for collections and indexes. Those tables are created
    // with the proper settings and consequently should not be getting changed here.
    //
    // If the settings need to be changed (only expected at startup), the alter table call must
    // succeed.
    std::string existingMetadata;
    {
        auto session = sessionCache->getSession();
        existingMetadata = getMetadataCreate(session->getSession(), uri).getValue();
    }
    if (existingMetadata.find("log=(enabled=true)") != std::string::npos &&
        existingMetadata.find("log=(enabled=false)") != std::string::npos) {
        // Sanity check against a table having multiple logging specifications.
        invariant(false,
                  str::stream() << "Table has contradictory logging settings. Uri: " << uri
                                << " Conf: " << existingMetadata);
    }

    if (existingMetadata.find(setting) != std::string::npos) {
        // The table is running with the expected logging settings.
        return Status::OK();
    }

    LOGV2_DEBUG(
        22432, 1, "Changing table logging settings", "uri"_attr = uri, "loggingEnabled"_attr = on);
    // Only alter the metadata once we're sure that we need to change the table settings, since
    // WT_SESSION::alter may return EBUSY and require taking a checkpoint to make progress.
    auto status = sessionCache->getKVEngine()->alterMetadata(uri, setting);
    if (!status.isOK()) {
        // Dump the storage engine's internal state to assist in diagnosis.
        sessionCache->getKVEngine()->dump();

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

Status WiredTigerUtil::exportTableToBSON(WT_SESSION* session,
                                         const std::string& uri,
                                         const std::string& config,
                                         BSONObjBuilder* bob) {
    return exportTableToBSON(session, uri, config, bob, {});
}

Status WiredTigerUtil::exportTableToBSON(WT_SESSION* session,
                                         const std::string& uri,
                                         const std::string& config,
                                         BSONObjBuilder* bob,
                                         const std::vector<std::string>& filter) {
    invariant(session);
    invariant(bob);
    WT_CURSOR* c = nullptr;
    const char* cursorConfig = config.empty() ? nullptr : config.c_str();
    int ret = session->open_cursor(session, uri.c_str(), nullptr, cursorConfig, &c);
    if (ret != 0) {
        return Status(ErrorCodes::CursorNotFound,
                      str::stream() << "unable to open cursor at URI " << uri
                                    << ". reason: " << wiredtiger_strerror(ret));
    }
    bob->append("uri", uri);
    invariant(c);
    ON_BLOCK_EXIT([&] { c->close(c); });

    std::map<string, BSONObjBuilder*> subs;
    const char* desc;
    uint64_t value;
    while (c->next(c) == 0 && c->get_value(c, &desc, nullptr, &value) == 0) {
        StringData key(desc);

        StringData prefix;
        StringData suffix;

        size_t idx = key.find(':');
        if (idx != string::npos) {
            prefix = key.substr(0, idx);
            suffix = key.substr(idx + 1);
        } else {
            idx = key.find(' ');
        }

        if (idx != string::npos) {
            prefix = key.substr(0, idx);
            suffix = key.substr(idx + 1);
        } else {
            prefix = key;
            suffix = "num";
        }

        long long v = castStatisticsValue<long long>(value);

        if (prefix.size() == 0) {
            bob->appendNumber(desc, v);
        } else {
            bool shouldSkipField = std::find(filter.begin(), filter.end(), prefix) != filter.end();
            if (shouldSkipField) {
                continue;
            }

            BSONObjBuilder*& sub = subs[prefix.toString()];
            if (!sub)
                sub = new BSONObjBuilder();
            sub->appendNumber(str::ltrim(suffix.toString()), v);
        }
    }

    for (std::map<string, BSONObjBuilder*>::const_iterator it = subs.begin(); it != subs.end();
         ++it) {
        const std::string& s = it->first;
        bob->append(s, it->second->obj());
        delete it->second;
    }
    return Status::OK();
}

StatusWith<std::string> WiredTigerUtil::generateImportString(StringData ident,
                                                             const BSONObj& storageMetadata,
                                                             const ImportOptions& importOptions) {
    if (!storageMetadata.hasField(ident)) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Missing the storage metadata for ident " << ident << " in "
                                    << redact(storageMetadata));
    }

    if (storageMetadata.getField(ident).type() != BSONType::Object) {
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

    if (tableMetadata.type() != BSONType::String || fileMetadata.type() != BSONType::String) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "The storage metadata for ident " << ident
                                    << " is not of type string for either the 'tableMetadata' or "
                                       "'fileMetadata' field in "
                                    << redact(storageMetadata));
    }

    std::stringstream ss;
    ss << tableMetadata.String();
    ss << ",import=(enabled=true,repair=false,";
    if (importOptions.importTimestampRule == ImportOptions::ImportTimestampRule::kStable) {
        ss << "compare_timestamp=stable,";
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
    if (!storageGlobalParams.restore) {
        return false;
    }

    const auto dbpath = boost::filesystem::path(storageGlobalParams.dbpath);
    return boost::filesystem::exists(dbpath / kWiredTigerBackupFile);
}

void WiredTigerUtil::appendSnapshotWindowSettings(WiredTigerKVEngine* engine,
                                                  WiredTigerSession* session,
                                                  BSONObjBuilder* bob) {
    invariant(engine);
    invariant(session);
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
        {logv2::LogComponent::kWiredTigerEviction, "evict"},
        {logv2::LogComponent::kWiredTigerHS, "history_store"},
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
    cfg << "recovery_progress:1,checkpoint_progress:1,compact_progress:1";

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

void WiredTigerUtil::removeEncryptionFromConfigString(std::string* configString) {
    encryptionOptsRegex->substitute("", configString, pcre::SUBSTITUTE_GLOBAL);
}

// static
BSONObj WiredTigerUtil::getSanitizedStorageOptionsForSecondaryReplication(const BSONObj& options) {
    // Storage options may contain settings for non-WiredTiger storage engines (e.g. inMemory).
    // We should leave these settings intact.
    if (auto wtElem = options[kWiredTigerEngineName]) {
        BSONObj wtObj = wtElem.Obj();
        if (auto configStringElem = wtObj.getField(kConfigStringField)) {
            auto configString = configStringElem.String();
            removeEncryptionFromConfigString(&configString);
            // Return a new BSONObj with the configString field sanitized.
            return options.addFields(BSON(kWiredTigerEngineName << wtObj.addFields(
                                              BSON(kConfigStringField << configString))));
        }
    }

    return options;
}

Status WiredTigerUtil::canRunAutoCompact(OperationContext* opCtx, bool isEphemeral) {
    if (!gFeatureFlagAutoCompact.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return Status(ErrorCodes::IllegalOperation,
                      "autoCompact() requires its feature flag to be enabled");
    }
    if (isEphemeral) {
        return Status(ErrorCodes::IllegalOperation,
                      "autoCompact() cannot be executed for in-memory configurations");
    }
    if (!opCtx->getServiceContext()->userWritesAllowed()) {
        return Status(ErrorCodes::IllegalOperation,
                      "autoCompact() can only be executed when writes are allowed");
    }
    if (storageGlobalParams.syncdelay == 0) {
        return Status(ErrorCodes::IllegalOperation,
                      "autoCompact() can only be executed when checkpoints are enabled");
    }
    return Status::OK();
}


}  // namespace mongo
