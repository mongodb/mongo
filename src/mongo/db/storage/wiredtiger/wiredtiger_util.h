// wiredtiger_util.h

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/assert_util.h"

namespace mongo {

class BSONObjBuilder;
class OperationContext;
class WiredTigerConfigParser;

inline bool wt_keeptxnopen() {
    return false;
}

Status wtRCToStatus_slow(int retCode, const char* prefix);

/**
 * converts wiredtiger return codes to mongodb statuses.
 */
inline Status wtRCToStatus(int retCode, const char* prefix = NULL) {
    if (MONGO_likely(retCode == 0))
        return Status::OK();

    return wtRCToStatus_slow(retCode, prefix);
}

#define invariantWTOK(expression)                                                       \
    do {                                                                                \
        int _invariantWTOK_retCode = expression;                                        \
        if (MONGO_unlikely(_invariantWTOK_retCode != 0)) {                              \
            invariantOKFailed(                                                          \
                #expression, wtRCToStatus(_invariantWTOK_retCode), __FILE__, __LINE__); \
        }                                                                               \
    } while (false)

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

class WiredTigerUtil {
    MONGO_DISALLOW_COPYING(WiredTigerUtil);

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
     * Additional, adds 'uri' field to output document.
     */
    static Status exportTableToBSON(WT_SESSION* s,
                                    const std::string& uri,
                                    const std::string& config,
                                    BSONObjBuilder* bob);

    /**
     * Gets entire metadata string for collection/index at URI.
     */
    static StatusWith<std::string> getMetadata(OperationContext* opCtx, StringData uri);

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
    static StatusWith<uint64_t> getStatisticsValue(WT_SESSION* session,
                                                   const std::string& uri,
                                                   const std::string& config,
                                                   int statisticsKey);

    /**
     * Reads individual statistics using URI and casts to type ResultType.
     * Caps statistics value at max(ResultType) in case of overflow.
     */
    template <typename ResultType>
    static StatusWith<ResultType> getStatisticsValueAs(WT_SESSION* session,
                                                       const std::string& uri,
                                                       const std::string& config,
                                                       int statisticsKey);

    /**
     * Reads individual statistics using URI and casts to type ResultType.
     * Caps statistics value at 'maximumResultType'.
     */
    template <typename ResultType>
    static StatusWith<ResultType> getStatisticsValueAs(WT_SESSION* session,
                                                       const std::string& uri,
                                                       const std::string& config,
                                                       int statisticsKey,
                                                       ResultType maximumResultType);

    static int64_t getIdentSize(WT_SESSION* s, const std::string& uri);


    /**
     * Return amount of memory to use for the WiredTiger cache based on either the startup
     * option chosen or the amount of available memory on the host.
     */
    static size_t getCacheSizeMB(double requestedCacheSizeGB);

    /**
     * Returns a WT_EVENT_HANDER with MongoDB's default handlers.
     * The default handlers just log so it is recommended that you consider calling them even if
     * you are capturing the output.
     *
     * There is no default "close" handler. You only need to provide one if you need to call a
     * destructor.
     */
    static WT_EVENT_HANDLER defaultEventHandlers();

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
    static int verifyTable(OperationContext* txn,
                           const std::string& uri,
                           std::vector<std::string>* errors = NULL);

private:
    /**
     * Casts unsigned 64-bit statistics value to T.
     * If original value exceeds maximum value of T, return max(T).
     */
    template <typename T>
    static T _castStatisticsValue(uint64_t statisticsValue);

    /**
     * Casts unsigned 64-bit statistics value to T.
     * If original value exceeds 'maximumResultType', return 'maximumResultType'.
     */
    template <typename T>
    static T _castStatisticsValue(uint64_t statisticsValue, T maximumResultType);
};

class WiredTigerConfigParser {
    MONGO_DISALLOW_COPYING(WiredTigerConfigParser);

public:
    WiredTigerConfigParser(StringData config) {
        invariantWTOK(
            wiredtiger_config_parser_open(NULL, config.rawData(), config.size(), &_parser));
    }

    WiredTigerConfigParser(const WT_CONFIG_ITEM& nested) {
        invariant(nested.type == WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT);
        invariantWTOK(wiredtiger_config_parser_open(NULL, nested.str, nested.len, &_parser));
    }

    ~WiredTigerConfigParser() {
        invariantWTOK(_parser->close(_parser));
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
StatusWith<ResultType> WiredTigerUtil::getStatisticsValueAs(WT_SESSION* session,
                                                            const std::string& uri,
                                                            const std::string& config,
                                                            int statisticsKey) {
    return getStatisticsValueAs<ResultType>(
        session, uri, config, statisticsKey, std::numeric_limits<ResultType>::max());
}

// static
template <typename ResultType>
StatusWith<ResultType> WiredTigerUtil::getStatisticsValueAs(WT_SESSION* session,
                                                            const std::string& uri,
                                                            const std::string& config,
                                                            int statisticsKey,
                                                            ResultType maximumResultType) {
    StatusWith<uint64_t> result = getStatisticsValue(session, uri, config, statisticsKey);
    if (!result.isOK()) {
        return StatusWith<ResultType>(result.getStatus());
    }
    return StatusWith<ResultType>(
        _castStatisticsValue<ResultType>(result.getValue(), maximumResultType));
}

// static
template <typename ResultType>
ResultType WiredTigerUtil::_castStatisticsValue(uint64_t statisticsValue) {
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
