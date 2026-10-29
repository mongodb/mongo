// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/wiredtiger/wiredtiger_index_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <cerrno>
#include <string_view>

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
    std::string_view creationStringName("creationString");
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
    std::stringstream ss;
    ss << "statistics=(" << wiredTigerGlobalOptions.statisticsSetting << ")";
    Status status =
        WiredTigerUtil::exportTableToBSON(*session, "statistics:" + uri, ss.str(), *output);
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
                      str::stream() << "Compaction interrupted on " << uri.c_str());
    }

    if (ret == ENOENT) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "Can't compact missing URI " << uri);
    }

    invariantWTOK(ret, *s, uri);

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
