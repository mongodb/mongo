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

#pragma once

#include <limits>
#include <wiredtiger.h>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/util/assert_util.h"

namespace mongo {

class BSONObjBuilder;
class OperationContext;
class WiredTigerConfigParser;
class WiredTigerKVEngine;
class WiredTigerSession;
class WiredTigerSessionCache;

Status wtRCToStatus_slow(int retCode, WT_SESSION* session, StringData prefix);

/**
 * converts wiredtiger return codes to mongodb statuses.
 */
inline Status wtRCToStatus(int retCode, WT_SESSION* session, const char* prefix = nullptr) {
    if (MONGO_likely(retCode == 0))
        return Status::OK();

    return wtRCToStatus_slow(retCode, session, prefix);
}

template <typename ContextExpr>
Status wtRCToStatus(int retCode, WT_SESSION* session, ContextExpr&& contextExpr) {
    if (MONGO_likely(retCode == 0))
        return Status::OK();

    return wtRCToStatus_slow(retCode, session, std::forward<ContextExpr>(contextExpr)());
}

inline void uassertWTOK(int ret, WT_SESSION* session) {
    uassertStatusOK(wtRCToStatus(ret, session));
}

#define MONGO_invariantWTOK_2(expression, session)                                               \
    do {                                                                                         \
        int _invariantWTOK_retCode = expression;                                                 \
        if (MONGO_unlikely(_invariantWTOK_retCode != 0)) {                                       \
            invariantOKFailed(                                                                   \
                #expression, wtRCToStatus(_invariantWTOK_retCode, session), __FILE__, __LINE__); \
        }                                                                                        \
    } while (false)

#define MONGO_invariantWTOK_3(expression, session, contextExpr)                     \
    do {                                                                            \
        int _invariantWTOK_retCode = expression;                                    \
        if (MONGO_unlikely(_invariantWTOK_retCode != 0)) {                          \
            invariantOKFailedWithMsg(#expression,                                   \
                                     wtRCToStatus(_invariantWTOK_retCode, session), \
                                     contextExpr,                                   \
                                     __FILE__,                                      \
                                     __LINE__);                                     \
        }                                                                           \
    } while (false)

#define invariantWTOK(...) \
    MONGO_expand(MONGO_expand(BOOST_PP_OVERLOAD(MONGO_invariantWTOK_, __VA_ARGS__))(__VA_ARGS__))

struct WiredTigerItem : public WT_ITEM {
    WiredTigerItem(const void* d, size_t s) {
        data = d;
        size = s;
    }
    WiredTigerItem(const std::string& str) {
        data = str.c_str();
        size = str.size();
    }
    // NOTE: do not call Get() on a temporary.
    // The pointer returned by Get() must not be allowed to live longer than *this.
    WT_ITEM* Get() {
        return this;
    }
    const WT_ITEM* Get() const {
        return this;
    }
};

/**
 * Returns a WT_EVENT_HANDLER with MongoDB's default handlers.
 * The default handlers just log so it is recommended that you consider calling them even if
 * you are capturing the output.
 *
 * There is no default "close" handler. You only need to provide one if you need to call a
 * destructor.
 */
class WiredTigerEventHandler : private WT_EVENT_HANDLER {
public:
    WiredTigerEventHandler();

    WT_EVENT_HANDLER* getWtEventHandler();

    bool wasStartupSuccessful() {
        return _startupSuccessful;
    }

    void setStartupSuccessful() {
        _startupSuccessful = true;
    }

    bool isWtIncompatible() {
        return _wtIncompatible;
    }

    void setWtIncompatible() {
        _wtIncompatible = true;
    }

private:
    bool _startupSuccessful = false;
    bool _wtIncompatible = false;
};

class WiredTigerUtil {
    WiredTigerUtil(const WiredTigerUtil&) = delete;
    WiredTigerUtil& operator=(const WiredTigerUtil&) = delete;

private:
    WiredTigerUtil();

public:
    /**
     * Fetch the type and source fields out of the colgroup metadata.  'tableUri' must be a
     * valid table: uri.
     */
    static void fetchTypeAndSourceURI(OperationContext* opCtx,
                                      const std::string& tableUri,
                                      std::string* type,
                                      std::string* source);

    /**
     * Reads contents of table using URI and exports all keys to BSON as string elements.
     * Additional, adds 'uri' field to output document. A filter can be specified to skip desired
     * fields.
     */
    static Status exportTableToBSON(WT_SESSION* s,
                                    const std::string& uri,
                                    const std::string& config,
                                    BSONObjBuilder* bob);
    static Status exportTableToBSON(WT_SESSION* s,
                                    const std::string& uri,
                                    const std::string& config,
                                    BSONObjBuilder* bob,
                                    const std::vector<std::string>& filter);

    /**
     * Creates an import configuration string suitable for the 'config' parameter in
     * WT_SESSION::create() given the storage engines metadata retrieved during the export.
     *
     * Returns the FailedToParse status if the storage engine metadata object is malformed.
     */
    static StatusWith<std::string> generateImportString(const StringData& ident,
                                                        const BSONObj& storageMetadata,
                                                        const ImportOptions& importOptions);

    /**
     * Creates the configuration string for the 'backup_restore_target' config option passed into
     * wiredtiger_open().
     *
     * When restoring from a backup, WiredTiger will only restore the table objects present in the
     * dbpath. WiredTiger will remove all the metadata entries for the tables that are not listed in
     * the list from the reconstructed metadata.
     */
    static std::string generateRestoreConfig();

    /**
     * Returns true if WiredTiger startup will restore from a backup.
     */
    static bool willRestoreFromBackup();

    /**
     * Appends information about the storage engine's currently available snapshots and the settings
     * that affect that window of maintained history.
     *
     * "snapshot-window-settings" : {
     *      "total number of SnapshotTooOld errors" : <num>,
     *      "minimum target snapshot window size in seconds" : <num>,
     *      "current available snapshot window size in seconds" : <num>,
     *      "latest majority snapshot timestamp available" : <num>,
     *      "oldest majority snapshot timestamp available" : <num>
     * }
     */
    static void appendSnapshotWindowSettings(WiredTigerKVEngine* engine,
                                             WiredTigerSession* session,
                                             BSONObjBuilder* bob);

    /**
     * Gets the creation metadata string for a collection or index at a given URI. Accepts an
     * OperationContext or session.
     *
     * This returns more information, but is slower than getMetadata().
     */
    static StatusWith<std::string> getMetadataCreate(OperationContext* opCtx, StringData uri);
    static StatusWith<std::string> getMetadataCreate(WT_SESSION* session, StringData uri);

    /**
     * Gets the entire metadata string for collection or index at URI. Accepts an OperationContext
     * or session.
     */
    static StatusWith<std::string> getMetadata(OperationContext* opCtx, StringData uri);
    static StatusWith<std::string> getMetadata(WT_SESSION* session, StringData uri);

    /**
     * Reads app_metadata for collection/index at URI as a BSON document.
     */
    static Status getApplicationMetadata(OperationContext* opCtx,
                                         StringData uri,
                                         BSONObjBuilder* bob);

    static StatusWith<BSONObj> getApplicationMetadata(OperationContext* opCtx, StringData uri);

    /**
     * Validates formatVersion in application metadata for 'uri'.
     * Version must be numeric and be in the range [minimumVersion, maximumVersion].
     * URI is used in error messages only. Returns actual version.
     */
    static StatusWith<int64_t> checkApplicationMetadataFormatVersion(OperationContext* opCtx,
                                                                     StringData uri,
                                                                     int64_t minimumVersion,
                                                                     int64_t maximumVersion);

    /**
     * Validates the 'configString' specified as a collection or index creation option.
     */
    static Status checkTableCreationOptions(const BSONElement& configElem);

    /**
     * Reads individual statistics using URI.
     * List of statistics keys WT_STAT_* can be found in wiredtiger.h.
     */
    static StatusWith<int64_t> getStatisticsValue(WT_SESSION* session,
                                                  const std::string& uri,
                                                  const std::string& config,
                                                  int statisticsKey);

    static int64_t getIdentSize(WT_SESSION* s, const std::string& uri);

    /**
     * Returns the bytes available for reuse for an ident. This is the amount of allocated space on
     * disk that is not storing any data.
     */
    static int64_t getIdentReuseSize(WT_SESSION* s, const std::string& uri);


    /**
     * Return amount of memory to use for the WiredTiger cache based on either the startup
     * option chosen or the amount of available memory on the host.
     */
    static size_t getCacheSizeMB(double requestedCacheSizeGB);

    class ErrorAccumulator : public WT_EVENT_HANDLER {
    public:
        ErrorAccumulator(std::vector<std::string>* errors);

    private:
        static int onError(WT_EVENT_HANDLER* handler,
                           WT_SESSION* session,
                           int error,
                           const char* message);

        using ErrorHandler = int (*)(WT_EVENT_HANDLER*, WT_SESSION*, int, const char*);

        std::vector<std::string>* const _errors;
        const ErrorHandler _defaultErrorHandler;
    };

    /**
     * Calls WT_SESSION::validate() on a side-session to ensure that your current transaction
     * isn't left in an invalid state.
     *
     * If errors is non-NULL, all error messages will be appended to the array.
     */
    static int verifyTable(OperationContext* opCtx,
                           const std::string& uri,
                           std::vector<std::string>* errors = nullptr);

    static void notifyStartupComplete();

    static bool useTableLogging(const NamespaceString& nss);

    static Status setTableLogging(OperationContext* opCtx, const std::string& uri, bool on);

    /**
     * Generates a WiredTiger connection configuration given the LOGV2 WiredTiger components
     * verbosity levels.
     */
    static std::string generateWTVerboseConfiguration();

    /**
     * Casts unsigned 64-bit statistics value to T.
     * If original value exceeds maximum value of T, return max(T).
     */
    template <typename T>
    static T castStatisticsValue(uint64_t statisticsValue);

private:
    /**
     * Casts unsigned 64-bit statistics value to T.
     * If original value exceeds 'maximumResultType', return 'maximumResultType'.
     */
    template <typename T>
    static T _castStatisticsValue(uint64_t statisticsValue, T maximumResultType);
};

class WiredTigerConfigParser {
    WiredTigerConfigParser(const WiredTigerConfigParser&) = delete;
    WiredTigerConfigParser& operator=(const WiredTigerConfigParser&) = delete;

public:
    WiredTigerConfigParser(StringData config) {
        invariantWTOK(
            wiredtiger_config_parser_open(nullptr, config.rawData(), config.size(), &_parser),
            nullptr);
    }

    WiredTigerConfigParser(const WT_CONFIG_ITEM& nested) {
        invariant(nested.type == WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT);
        invariantWTOK(wiredtiger_config_parser_open(nullptr, nested.str, nested.len, &_parser),
                      nullptr);
    }

    ~WiredTigerConfigParser() {
        invariantWTOK(_parser->close(_parser), nullptr);
    }

    int next(WT_CONFIG_ITEM* key, WT_CONFIG_ITEM* value) {
        return _parser->next(_parser, key, value);
    }

    int get(const char* key, WT_CONFIG_ITEM* value) {
        return _parser->get(_parser, key, value);
    }

private:
    WT_CONFIG_PARSER* _parser;
};

// static
template <typename ResultType>
ResultType WiredTigerUtil::castStatisticsValue(uint64_t statisticsValue) {
    return _castStatisticsValue<ResultType>(statisticsValue,
                                            std::numeric_limits<ResultType>::max());
}

// static
template <typename ResultType>
ResultType WiredTigerUtil::_castStatisticsValue(uint64_t statisticsValue,
                                                ResultType maximumResultType) {
    return statisticsValue > static_cast<uint64_t>(maximumResultType)
        ? maximumResultType
        : static_cast<ResultType>(statisticsValue);
}

}  // namespace mongo
