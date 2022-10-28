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
#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

MONGO_FAIL_POINT_DEFINE(WTCompactIndexEBUSY);

bool WiredTigerIndexUtil::appendCustomStats(OperationContext* opCtx,
                                            BSONObjBuilder* output,
                                            double scale,
                                            const std::string& uri) {
    dassert(opCtx->lockState()->isReadLocked());
    {
        BSONObjBuilder metadata(output->subobjStart("metadata"));
        Status status = WiredTigerUtil::getApplicationMetadata(opCtx, uri, &metadata);
        if (!status.isOK()) {
            metadata.append("error", "unable to retrieve metadata");
            metadata.append("code", static_cast<int>(status.code()));
            metadata.append("reason", status.reason());
        }
    }
    std::string type, sourceURI;
    WiredTigerUtil::fetchTypeAndSourceURI(opCtx, uri, &type, &sourceURI);
    StatusWith<std::string> metadataResult = WiredTigerUtil::getMetadataCreate(opCtx, sourceURI);
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

    WiredTigerSession* session = WiredTigerRecoveryUnit::get(opCtx)->getSession();
    WT_SESSION* s = session->getSession();
    Status status =
        WiredTigerUtil::exportTableToBSON(s, "statistics:" + uri, "statistics=(fast)", output);
    if (!status.isOK()) {
        output->append("error", "unable to retrieve statistics");
        output->append("code", static_cast<int>(status.code()));
        output->append("reason", status.reason());
    }
    return true;
}

Status WiredTigerIndexUtil::compact(OperationContext* opCtx, const std::string& uri) {
    dassert(opCtx->lockState()->isWriteLocked());
    WiredTigerSessionCache* cache = WiredTigerRecoveryUnit::get(opCtx)->getSessionCache();
    if (!cache->isEphemeral()) {
        WT_SESSION* s = WiredTigerRecoveryUnit::get(opCtx)->getSession()->getSession();
        opCtx->recoveryUnit()->abandonSnapshot();
        // WT compact prompts WT to take checkpoints, so we need to take the checkpoint lock around
        // WT compact calls.
        auto checkpointLock = opCtx->getServiceContext()->getStorageEngine()->getCheckpointLock(
            opCtx, StorageEngine::CheckpointLock::Mode::kExclusive);
        int ret = s->compact(s, uri.c_str(), "timeout=0");
        if (MONGO_unlikely(WTCompactIndexEBUSY.shouldFail())) {
            ret = EBUSY;
        }

        if (ret == EBUSY) {
            return Status(ErrorCodes::Interrupted,
                          str::stream() << "Compaction interrupted on " << uri.c_str()
                                        << " due to cache eviction pressure");
        }
        invariantWTOK(ret, s);
    }
    return Status::OK();
}

bool WiredTigerIndexUtil::isEmpty(OperationContext* opCtx,
                                  const std::string& uri,
                                  uint64_t tableId) {
    WiredTigerCursor curwrap(uri, tableId, false, opCtx);
    WT_CURSOR* c = curwrap.get();
    if (!c)
        return true;
    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->next(c); });
    if (ret == WT_NOTFOUND)
        return true;
    invariantWTOK(ret, c->session);
    return false;
}

bool WiredTigerIndexUtil::validateStructure(OperationContext* opCtx,
                                            const std::string& uri,
                                            IndexValidateResults* fullResults) {
    if (fullResults && !WiredTigerRecoveryUnit::get(opCtx)->getSessionCache()->isEphemeral()) {
        int err = WiredTigerUtil::verifyTable(opCtx, uri, &(fullResults->errors));
        if (err == EBUSY) {
            std::string msg = str::stream()
                << "Could not complete validation of " << uri << ". "
                << "This is a transient issue as the collection was actively "
                   "in use by other operations.";

            LOGV2_WARNING(51781,
                          "Could not complete validation. This is a transient issue as "
                          "the collection was actively in use by other operations",
                          "uri"_attr = uri);
            fullResults->warnings.push_back(msg);
        } else if (err) {
            std::string msg = str::stream()
                << "verify() returned " << wiredtiger_strerror(err) << ". "
                << "This indicates structural damage. "
                << "Not examining individual index entries.";
            LOGV2_ERROR(51782,
                        "verify() returned an error. This indicates structural damage. Not "
                        "examining individual index entries.",
                        "error"_attr = wiredtiger_strerror(err));
            fullResults->errors.push_back(msg);
            fullResults->valid = false;
            return false;
        }
    }
    return true;
}

}  // namespace mongo
