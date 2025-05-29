/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/storage/wiredtiger/wiredtiger_index_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <cerrno>

#include <wiredtiger.h>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

MONGO_FAIL_POINT_DEFINE(WTCompactIndexEBUSY);
MONGO_FAIL_POINT_DEFINE(WTValidateIndexStructuralDamage);

bool WiredTigerIndexUtil::appendCustomStats(WiredTigerRecoveryUnit& ru,
                                            BSONObjBuilder* output,
                                            double scale,
                                            const std::string& uri) {
    {
        BSONObjBuilder metadata(output->subobjStart("metadata"));
        Status status =
            WiredTigerUtil::getApplicationMetadata(*ru.getSessionNoTxn(), uri, &metadata);
        if (!status.isOK()) {
            metadata.append("error", "unable to retrieve metadata");
            metadata.append("code", static_cast<int>(status.code()));
            metadata.append("reason", status.reason());
        }
    }
    std::string type, sourceURI;
    WiredTigerUtil::fetchTypeAndSourceURI(*ru.getSessionNoTxn(), uri, &type, &sourceURI);
    StatusWith<std::string> metadataResult =
        WiredTigerUtil::getMetadataCreate(*ru.getSessionNoTxn(), sourceURI);
    StringData creationStringName("creationString");
    if (!metadataResult.isOK()) {
        BSONObjBuilder creationString(output->subobjStart(creationStringName));
        creationString.append("error", "unable to retrieve creation config");
        creationString.append("code", static_cast<int>(metadataResult.getStatus().code()));
        creationString.append("reason", metadataResult.getStatus().reason());
    } else {
        output->append(creationStringName, metadataResult.getValue());
        // Type can be "lsm" or "file"
        output->append("type", type);
    }

    WiredTigerSession* session = ru.getSession();
    Status status = WiredTigerUtil::exportTableToBSON(
        *session, "statistics:" + uri, "statistics=(fast)", *output);
    if (!status.isOK()) {
        output->append("error", "unable to retrieve statistics");
        output->append("code", static_cast<int>(status.code()));
        output->append("reason", status.reason());
    }
    return true;
}

StatusWith<int64_t> WiredTigerIndexUtil::compact(OperationContext* opCtx,
                                                 WiredTigerRecoveryUnit& wtRu,
                                                 const std::string& uri,
                                                 const CompactOptions& options) {
    WiredTigerConnection* connection = wtRu.getConnection();
    if (connection->isEphemeral()) {
        return 0;
    }

    WiredTigerSession* s = wtRu.getSession();
    wtRu.abandonSnapshot();

    StringBuilder config;
    config << "timeout=0";
    if (options.dryRun) {
        config << ",dryrun=true";
    }
    if (options.freeSpaceTargetMB) {
        config << ",free_space_target=" + std::to_string(*options.freeSpaceTargetMB) + "MB";
    }

    int ret = s->compact(uri.c_str(), config.str().c_str());

    if (ret == WT_ERROR && !opCtx->checkForInterruptNoAssert().isOK()) {
        return Status(ErrorCodes::Interrupted,
                      str::stream() << "Storage compaction interrupted on " << uri.c_str());
    }

    if (MONGO_unlikely(WTCompactIndexEBUSY.shouldFail())) {
        ret = EBUSY;
    }

    if (ret == EBUSY) {
        return Status(ErrorCodes::Interrupted,
                      str::stream() << "Compaction interrupted on " << uri.c_str()
                                    << " due to cache eviction pressure");
    }

    invariantWTOK(ret, *s);

    return options.dryRun ? WiredTigerUtil::getIdentCompactRewrittenExpectedSize(*s, uri) : 0;
}

bool WiredTigerIndexUtil::isEmpty(OperationContext* opCtx,
                                  WiredTigerRecoveryUnit& wtRu,
                                  const std::string& uri,
                                  uint64_t tableId) {
    auto cursorParams = getWiredTigerCursorParams(wtRu, tableId);
    WiredTigerCursor curwrap(std::move(cursorParams), uri, *wtRu.getSession());
    WT_CURSOR* c = curwrap.get();
    if (!c)
        return true;
    int ret = wiredTigerPrepareConflictRetry(
        *opCtx, StorageExecutionContext::get(opCtx)->getPrepareConflictTracker(), wtRu, [&] {
            return c->next(c);
        });
    if (ret == WT_NOTFOUND)
        return true;
    invariantWTOK(ret, c->session);
    return false;
}

void WiredTigerIndexUtil::validateStructure(
    WiredTigerRecoveryUnit& ru,
    const std::string& uri,
    const boost::optional<std::string>& configurationOverride,
    IndexValidateResults& results) {
    if (ru.getConnection()->isEphemeral()) {
        return;
    }

    if (WTValidateIndexStructuralDamage.shouldFail()) {
        std::string msg = str::stream() << "verify() returned an error. "
                                        << "This indicates structural damage. "
                                        << "Not examining individual index entries.";
        results.setHasStructuralDamage(true);
        results.addError(msg);
        return;
    }

    auto err = WiredTigerUtil::verifyTable(
        *ru.getSession(), uri, configurationOverride, results.getErrorsUnsafe());
    if (err == EBUSY) {
        std::string msg = str::stream()
            << "Could not complete validation of " << uri << ". "
            << "This is a transient issue as the collection was actively "
               "in use by other operations.";

        LOGV2_PROD_ONLY(51781,
                        "Could not complete validation. This is a transient issue as "
                        "the collection was actively in use by other operations",
                        "uri"_attr = uri);

        results.addWarning(msg);
    } else if (err) {
        std::string msg = str::stream() << "verify() returned " << wiredtiger_strerror(err) << ". "
                                        << "This indicates structural damage. "
                                        << "Not examining individual index entries.";
        LOGV2_ERROR(51782,
                    "verify() returned an error. This indicates structural damage. Not "
                    "examining individual index entries.",
                    "error"_attr = wiredtiger_strerror(err));

        results.setHasStructuralDamage(true);
        results.addError(msg);
    }
}

}  // namespace mongo
