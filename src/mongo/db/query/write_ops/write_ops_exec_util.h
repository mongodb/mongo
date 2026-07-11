// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/util/decorable.h"
#include "mongo/util/modules.h"

namespace mongo::write_ops_exec {

/**
 * Sets the Client's LastOp to the system OpTime if needed. This is especially helpful for
 * adjusting the client opTime for cases when batched write performed multiple writes, but
 * when the last write was a no-op (which will not advance the client opTime).
 * TODO SERVER-115820 remove external dependencies on this class.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] LastOpFixer {
public:
    LastOpFixer(OperationContext* opCtx);

    ~LastOpFixer();

    // Called when we are starting an operation on the given namespace. The namespace is
    // needed so we can check if it is local, since we do not need to fix lastOp for local
    // writes.
    void startingOp(const NamespaceString& ns);

    // Called when we finish the operation that we last called startingOp() for.
    void finishedOpSuccessfully();

private:
    repl::ReplClientInfo& replClientInfo() {
        return repl::ReplClientInfo::forClient(_opCtx->getClient());
    }

    OperationContext* const _opCtx;
    bool _needToFixLastOp = true;
    repl::OpTime _opTimeAtLastOpStart;
};

}  // namespace mongo::write_ops_exec
