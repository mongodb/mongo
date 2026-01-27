/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/replicated_size_and_count_metadata_manager/uncommitted_changes.h"

#include "mongo/db/replicated_size_and_count_metadata_manager/replicated_size_and_count_metadata_manager.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace {
const OperationContext::Decoration<std::shared_ptr<UncommittedMetaChange>>
    getUncommittedMetaChange =
        OperationContext::declareDecoration<std::shared_ptr<UncommittedMetaChange>>();
const ServiceContext::Decoration<ReplicatedSizeAndCountMetadataManager> getMetadataManager =
    ServiceContext::declareDecoration<ReplicatedSizeAndCountMetadataManager>();
}  // namespace

const UncommittedMetaChange& UncommittedMetaChange::read(OperationContext* opCtx) {
    std::shared_ptr<UncommittedMetaChange>& ptr = getUncommittedMetaChange(opCtx);
    if (ptr) {
        return *ptr;
    }

    static UncommittedMetaChange empty;
    return empty;
}


UncommittedMetaChange& UncommittedMetaChange::write(OperationContext* opCtx) {
    std::shared_ptr<UncommittedMetaChange>& ptr = getUncommittedMetaChange(opCtx);
    if (ptr) {
        return *ptr;
    }

    auto metaChange = std::make_shared<UncommittedMetaChange>();
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [metaChange](OperationContext* opCtx, boost::optional<Timestamp> commitTime) {
            getMetadataManager(opCtx->getServiceContext())
                .commit(metaChange->_trackedChanges, commitTime);
        });
    ptr = std::move(metaChange);
    return *ptr;
}

CollectionSizeCount UncommittedMetaChange::find(UUID uuid) const {
    auto it = _trackedChanges.find(uuid);
    if (it != _trackedChanges.end()) {
        return it->second;
    }
    return {};
}

void UncommittedMetaChange::record(UUID uuid, int64_t numDelta, int64_t sizeDelta) {
    auto& collChanges = _trackedChanges[uuid];
    collChanges.count += numDelta;
    collChanges.size += sizeDelta;
}

}  // namespace mongo

