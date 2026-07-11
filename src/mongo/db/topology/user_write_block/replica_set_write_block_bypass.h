// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

namespace mongo {
// TODO (SERVER-125476): Change the class modularity to PRIVATE
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ReplicaSetWriteBlockBypass {
public:
    static ReplicaSetWriteBlockBypass& get(OperationContext* opCtx);

    bool isEnabled() const;
    void setFromMetadata(OperationContext* opCtx, boost::optional<bool> val);
    void set(bool bypassEnabled);
    void writeAsMetadata(BSONObjBuilder* builder);

private:
    bool _enabled = false;
};
}  // namespace mongo
