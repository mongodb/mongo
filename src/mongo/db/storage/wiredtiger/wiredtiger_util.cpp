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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"

#include <limits>

#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>

#include "mongo/base/simple_string_data_comparator.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/snapshot_window_options_gen.h"
#include "mongo/db/storage/storage_file_util.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(crashAfterUpdatingFirstTableLoggingSettings);

namespace {

const std::string kTableChecksFileName = "_wt_table_checks";

/**
 * Returns true if the 'kTableChecksFileName' file exists in the dbpath.
 *
 * Must be called before createTableChecksFile() or removeTableChecksFile() to get accurate results.
 */
bool hasPreviouslyIncompleteTableChecks() {
    auto path = boost::filesystem::path(storageGlobalParams.dbpath) /
        boost::filesystem::path(kTableChecksFileName);

    return boost::filesystem::exists(path);
}

/**
 * Creates the 'kTableChecksFileName' file in the dbpath.
 */
void createTableChecksFile() {
    auto path = boost::filesystem::path(storageGlobalParams.dbpath) /
        boost::filesystem::path(kTableChecksFileName);

    boost::filesystem::ofstream fileStream(path);
    fileStream << "This file indicates that a WiredTiger table check operation is in progress or "
                  "incomplete."
               << std::endl;
    if (fileStream.fail()) {
        LOGV2_FATAL_NOTRACE(4366400,
                            "Failed to write to file",
                            "file"_attr = path.generic_string(),
                            "error"_attr = errnoWithDescription());
    }
    fileStream.close();

    fassertNoTrace(4366401, fsyncFile(path));
    fassertNoTrace(4366402, fsyncParentDirectory(path));
}

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

Mutex WiredTigerUtil::_tableLoggingInfoMutex =
    MONGO_MAKE_LATCH("WiredTigerUtil::_tableLoggingInfoMutex");
WiredTigerUtil::TableLoggingInfo WiredTigerUtil::_tableLoggingInfo;

Status wtRCToStatus_slow(int retCode, const char* prefix) {
    if (retCode == 0)
        return Status::OK();

    if (retCode == WT_ROLLBACK) {
        throw WriteConflictException();
    }

    // Don't abort on WT_PANIC when repairing, as the error will be handled at a higher layer.
    fassert(28559, retCode != WT_PANIC || storageGlobalParams.repair);

    str::stream s;
    if (prefix)
        s << prefix << " ";
    s << retCode << ": " << wiredtiger_strerror(retCode);

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

void WiredTigerUtil::fetchTypeAndSourceURI(OperationContext* opCtx,
                                           const std::string& tableUri,
                                           std::string* type,
                                           std::string* source) {
    std::string colgroupUri = "colgroup";
    const size_t colon = tableUri.find(':');
    invariant(colon != string::npos);
    colgroupUri += tableUri.substr(colon);
    StatusWith<std::string> colgroupResult = getMetadataCreate(opCtx, colgroupUri);
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
        return StatusWith<std::string>(wtRCToStatus(ret));
    }
    const char* metadata = nullptr;
    ret = cursor->get_value(cursor, &metadata);
    if (ret != 0) {
        return StatusWith<std::string>(wtRCToStatus(ret));
    }
    invariant(metadata);
    return StatusWith<std::string>(metadata);
}
}  // namespace

StatusWith<std::string> WiredTigerUtil::getMetadataCreate(WT_SESSION* session, StringData uri) {
    WT_CURSOR* cursor;
    invariantWTOK(session->open_cursor(session, "metadata:create", nullptr, "", &cursor));
    invariant(cursor);
    ON_BLOCK_EXIT([cursor] { invariantWTOK(cursor->close(cursor)); });

    return _getMetadata(cursor, uri);
}

StatusWith<std::string> WiredTigerUtil::getMetadataCreate(OperationContext* opCtx, StringData uri) {
    invariant(opCtx);

    auto session = WiredTigerRecoveryUnit::get(opCtx)->getSessionNoTxn();

    WT_CURSOR* cursor = nullptr;
    try {
        const std::string metadataURI = "metadata:create";
        cursor = session->getCachedCursor(metadataURI, WiredTigerSession::kMetadataCreateTableId);
        if (!cursor) {
            cursor = session->getNewCursor(metadataURI);
        }
    } catch (const ExceptionFor<ErrorCodes::CursorNotFound>& ex) {
        LOGV2_FATAL_NOTRACE(51257, "Cursor not found", "error"_attr = ex);
    }
    invariant(cursor);
    auto releaser = makeGuard(
        [&] { session->releaseCursor(WiredTigerSession::kMetadataCreateTableId, cursor); });

    return _getMetadata(cursor, uri);
}

StatusWith<std::string> WiredTigerUtil::getMetadata(WT_SESSION* session, StringData uri) {
    WT_CURSOR* cursor;
    invariantWTOK(session->open_cursor(session, "metadata:", nullptr, "", &cursor));
    invariant(cursor);
    ON_BLOCK_EXIT([cursor] { invariantWTOK(cursor->close(cursor)); });

    return _getMetadata(cursor, uri);
}

StatusWith<std::string> WiredTigerUtil::getMetadata(OperationContext* opCtx, StringData uri) {
    invariant(opCtx);

    auto session = WiredTigerRecoveryUnit::get(opCtx)->getSessionNoTxn();
    WT_CURSOR* cursor = nullptr;
    try {
        const std::string metadataURI = "metadata:";
        cursor = session->getCachedCursor(metadataURI, WiredTigerSession::kMetadataTableId);
        if (!cursor) {
            cursor = session->getNewCursor(metadataURI);
        }
    } catch (const ExceptionFor<ErrorCodes::CursorNotFound>& ex) {
        LOGV2_FATAL_NOTRACE(31293, "Cursor not found", "error"_attr = ex);
    }
    invariant(cursor);
    auto releaser =
        makeGuard([&] { session->releaseCursor(WiredTigerSession::kMetadataTableId, cursor); });

    return _getMetadata(cursor, uri);
}

Status WiredTigerUtil::getApplicationMetadata(OperationContext* opCtx,
                                              StringData uri,
                                              BSONObjBuilder* bob) {
    StatusWith<std::string> metadataResult = getMetadata(opCtx, uri);
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
    auto keysSeen = SimpleStringDataComparator::kInstance.makeStringDataUnorderedSet();
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
                bob->appendIntOrLL(key, valueItem.val);
                break;
            default:
                bob->append(key, StringData(valueItem.str, valueItem.len));
                break;
        }
    }
    if (ret != WT_NOTFOUND) {
        return wtRCToStatus(ret);
    }

    return Status::OK();
}

StatusWith<BSONObj> WiredTigerUtil::getApplicationMetadata(OperationContext* opCtx,
                                                           StringData uri) {
    BSONObjBuilder bob;
    Status status = getApplicationMetadata(opCtx, uri, &bob);
    if (!status.isOK()) {
        return StatusWith<BSONObj>(status);
    }
    return StatusWith<BSONObj>(bob.obj());
}

StatusWith<int64_t> WiredTigerUtil::checkApplicationMetadataFormatVersion(OperationContext* opCtx,
                                                                          StringData uri,
                                                                          int64_t minimumVersion,
                                                                          int64_t maximumVersion) {
    StatusWith<std::string> result = getMetadata(opCtx, uri);
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
    invariant(configElem.fieldNameStringData() == "configString");

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

    Status status = wtRCToStatus(
        wiredtiger_config_validate(nullptr, &eventHandler, "WT_SESSION.create", config.rawData()));
    if (!status.isOK()) {
        StringBuilder errorMsg;
        errorMsg << status.reason();
        for (std::string error : errors) {
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
        return StatusWith<int64_t>(ErrorCodes::CursorNotFound,
                                   str::stream() << "unable to open cursor at URI " << uri
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

int64_t WiredTigerUtil::getIdentReuseSize(WT_SESSION* s, const std::string& uri) {
    auto result = WiredTigerUtil::getStatisticsValue(
        s, "statistics:" + uri, "statistics=(fast)", WT_STAT_DSRC_BLOCK_REUSE_BYTES);
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
              "Requested cache size: {requestedMB}MB exceeds max; setting to {maximumMB}MB",
              "Requested cache size exceeds max, setting to maximum",
              "requestedMB"_attr = cacheSizeMB,
              "maximumMB"_attr = kMaxSizeCacheMB);
        cacheSizeMB = kMaxSizeCacheMB;
    }
    return static_cast<size_t>(cacheSizeMB);
}

namespace {
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
        LOGV2_ERROR(22435,
                    "WiredTiger error ({error}) {message}",
                    "WiredTiger error",
                    "error"_attr = errorCode,
                    "message"_attr = message);

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
        LOGV2_ERROR(22436,
                    "WiredTiger error ({errorCode}) {message}",
                    "WiredTiger error",
                    "error"_attr = errorCode,
                    "message"_attr = redact(message));

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
    try {
        LOGV2(22430, "WiredTiger message", "message"_attr = redact(message));
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

WT_EVENT_HANDLER defaultEventHandlers() {
    WT_EVENT_HANDLER handlers = {};
    handlers.handle_error = mdb_handle_error;
    handlers.handle_message = mdb_handle_message;
    handlers.handle_progress = mdb_handle_progress;
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

int WiredTigerUtil::verifyTable(OperationContext* opCtx,
                                const std::string& uri,
                                std::vector<std::string>* errors) {
    ErrorAccumulator eventHandler(errors);

    // Try to close as much as possible to avoid EBUSY errors.
    WiredTigerRecoveryUnit::get(opCtx)->getSession()->closeAllCursors(uri);
    WiredTigerSessionCache* sessionCache = WiredTigerRecoveryUnit::get(opCtx)->getSessionCache();
    sessionCache->closeAllCursors(uri);

    // Open a new session with custom error handlers.
    WT_CONNECTION* conn = WiredTigerRecoveryUnit::get(opCtx)->getSessionCache()->conn();
    WT_SESSION* session;
    invariantWTOK(conn->open_session(conn, &eventHandler, nullptr, &session));
    ON_BLOCK_EXIT([&] { session->close(session, ""); });

    // Do the verify. Weird parens prevent treating "verify" as a macro.
    return (session->verify)(session, uri.c_str(), nullptr);
}

void WiredTigerUtil::notifyStartupComplete() {
    {
        stdx::lock_guard<Latch> lk(_tableLoggingInfoMutex);
        invariant(_tableLoggingInfo.isInitializing);
        _tableLoggingInfo.isInitializing = false;
    }

    if (!storageGlobalParams.readOnly) {
        removeTableChecksFile();
    }
}

void WiredTigerUtil::resetTableLoggingInfo() {
    stdx::lock_guard<Latch> lk(_tableLoggingInfoMutex);
    _tableLoggingInfo = TableLoggingInfo();
}

bool WiredTigerUtil::useTableLogging(NamespaceString ns, bool replEnabled) {
    if (!replEnabled) {
        // All tables on standalones are logged.
        return true;
    }

    // Of the replica set configurations:
    if (ns.db() != "local") {
        // All replicated collections are not logged.
        return false;
    }

    if (ns.coll() == "replset.minvalid") {
        // Of local collections, this is derived from the state of the data and therefore
        // not logged.
        return false;
    }

    // The remainder of local gets logged. In particular, the oplog and user created collections.
    return true;
}

Status WiredTigerUtil::setTableLogging(OperationContext* opCtx, const std::string& uri, bool on) {
    // Try to close as much as possible to avoid EBUSY errors.
    WiredTigerRecoveryUnit::get(opCtx)->getSession()->closeAllCursors(uri);
    WiredTigerSessionCache* sessionCache = WiredTigerRecoveryUnit::get(opCtx)->getSessionCache();
    sessionCache->closeAllCursors(uri);

    // Use a dedicated session for alter operations to avoid transaction issues.
    WiredTigerSession session(sessionCache->conn());
    return setTableLogging(session.getSession(), uri, on);
}

Status WiredTigerUtil::setTableLogging(WT_SESSION* session, const std::string& uri, bool on) {
    invariant(!storageGlobalParams.readOnly);
    stdx::lock_guard<Latch> lk(_tableLoggingInfoMutex);

    // Update the table logging settings regardless if we're no longer starting up the process.
    if (!_tableLoggingInfo.isInitializing) {
        return _setTableLogging(session, uri, on);
    }

    // During the start up process, the table logging settings are checked for each table to verify
    // that they are set appropriately. We can speed this process up by assuming that the logging
    // setting is identical for each table.
    // We cross reference the logging settings for the first table and if it isn't correctly set, we
    // change the logging settings for all tables during start up.
    // In the event that the server wasn't shutdown cleanly, the logging settings will be modified
    // for all tables as a safety precaution, or if repair mode is running.
    if (_tableLoggingInfo.isFirstTable && hasPreviouslyIncompleteTableChecks()) {
        _tableLoggingInfo.hasPreviouslyIncompleteTableChecks = true;
    }

    if (storageGlobalParams.repair || _tableLoggingInfo.hasPreviouslyIncompleteTableChecks) {
        if (_tableLoggingInfo.isFirstTable) {
            _tableLoggingInfo.isFirstTable = false;
            if (!_tableLoggingInfo.hasPreviouslyIncompleteTableChecks) {
                createTableChecksFile();
            }

            LOGV2(4366405,
                  "Modifying the table logging settings for all existing WiredTiger tables",
                  "loggingEnabled"_attr = on,
                  "repair"_attr = storageGlobalParams.repair,
                  "hasPreviouslyIncompleteTableChecks"_attr =
                      _tableLoggingInfo.hasPreviouslyIncompleteTableChecks);
        }

        return _setTableLogging(session, uri, on);
    }

    if (!_tableLoggingInfo.isFirstTable) {
        if (_tableLoggingInfo.changeTableLogging) {
            return _setTableLogging(session, uri, on);
        }

        // The table logging settings do not need to be modified.
        return Status::OK();
    }

    invariant(_tableLoggingInfo.isFirstTable);
    invariant(!_tableLoggingInfo.hasPreviouslyIncompleteTableChecks);
    _tableLoggingInfo.isFirstTable = false;

    // Check if the first tables logging settings need to be modified.
    const std::string setting = on ? "log=(enabled=true)" : "log=(enabled=false)";
    const std::string existingMetadata = getMetadataCreate(session, uri).getValue();
    if (existingMetadata.find(setting) != std::string::npos) {
        // The table is running with the expected logging settings.
        LOGV2(4366408,
              "No table logging settings modifications are required for existing WiredTiger tables",
              "loggingEnabled"_attr = on);
        return Status::OK();
    }

    // The first table is running with the incorrect logging settings. All tables will need to have
    // their logging settings modified.
    _tableLoggingInfo.changeTableLogging = true;
    createTableChecksFile();

    LOGV2(4366406,
          "Modifying the table logging settings for all existing WiredTiger tables",
          "loggingEnabled"_attr = on);

    Status status = _setTableLogging(session, uri, on);

    if (MONGO_unlikely(crashAfterUpdatingFirstTableLoggingSettings.shouldFail())) {
        LOGV2_FATAL_NOTRACE(
            4366407, "Crashing due to 'crashAfterUpdatingFirstTableLoggingSettings' fail point");
    }
    return status;
}

Status WiredTigerUtil::_setTableLogging(WT_SESSION* session, const std::string& uri, bool on) {
    const std::string setting = on ? "log=(enabled=true)" : "log=(enabled=false)";

    // This method does some "weak" parsing to see if the table is in the expected logging
    // state. Only attempt to alter the table when a change is needed. This avoids grabbing heavy
    // locks in WT when creating new tables for collections and indexes. Those tables are created
    // with the proper settings and consequently should not be getting changed here.
    //
    // If the settings need to be changed (only expected at startup), the alter table call must
    // succeed.
    std::string existingMetadata = getMetadataCreate(session, uri).getValue();
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
    int ret = session->alter(session, uri.c_str(), setting.c_str());
    if (ret) {
        LOGV2_FATAL(50756,
                    "Failed to update log setting",
                    "uri"_attr = uri,
                    "loggingEnabled"_attr = on,
                    "error"_attr = ret,
                    "metadata"_attr = redact(existingMetadata),
                    "message"_attr = session->strerror(session, ret));
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

StatusWith<std::string> WiredTigerUtil::generateImportString(const StringData& ident,
                                                             const BSONObj& storageMetadata) {
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
    ss << ",import=(enabled=true,repair=false,file_metadata=(";
    ss << fileMetadata.String();
    ss << "))";

    return StatusWith<std::string>(ss.str());
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
}

}  // namespace mongo
