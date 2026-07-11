// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/read_concern_args.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Overrides the ReadConcern in the OperationContext while in scope, mainly useful for
 * DBDirectClient.
 */
class [[MONGO_MOD_PUBLIC]] ScopedReadConcern {
public:
    ScopedReadConcern(OperationContext* opCtx, repl::ReadConcernArgs requestReadConcernArgs);
    ~ScopedReadConcern();

private:
    OperationContext* _opCtx;
    repl::ReadConcernArgs _originalRCA;
    RecoveryUnit::ReadSource _originalReadSource;
    boost::optional<Timestamp> _originalReadTimestamp;
};

}  // namespace mongo
