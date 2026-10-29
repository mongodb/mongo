// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/forwardable_operation_metadata_gen.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/*
 * Stores metadata of an operation context that needs to be forwarded to other nodes in the cluster.
 *
 * The metadata are captured from the original operation context through the constructor and can be
 * attached to a new operation context by mean of the `setOn` member function.
 *
 * example:
 * 		auto opMetadata = ForwardableOperationMetadata(opCtx);
 * 		opMetadata.setOn(newOpCtx);
 */
class ForwardableOperationMetadata : public ForwardableOperationMetadataBase {
public:
    ForwardableOperationMetadata() = default;
    explicit ForwardableOperationMetadata(const BSONObj& obj);
    explicit ForwardableOperationMetadata(OperationContext* opCtx);

    void setOn(OperationContext* opCtx) const;

    /**
     * Enables propagation of the VersionContext (OFCV) to sub-operations for network calls.
     * This may only be called from durable operations (e.g. ShardingCoordinator, resharding state
     * machines), which setFCV can reliably track and drain.
     *
     * Misusing this API can lead to issues with inconsistent data persistence. Please include a
     * Catalog and Routing member on the code review before enabling this flag.
     */
    [[nodiscard]] ForwardableOperationMetadata withVersionContextPropagation_UNSAFE() const {
        auto copy = *this;

        // TODO SERVER-99655: update once gSnapshotFCVInDDLCoordinators is enabled on lastLTS
        if (const auto& vCtx = copy.getVersionContext()) {
            copy.setVersionContext(vCtx->withPropagationAcrossShards_UNSAFE());
        }
        return copy;
    }
};

}  // namespace mongo
