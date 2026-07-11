// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/util/decorable.h"

namespace mongo {
namespace repl {
namespace {

const OperationContext::Decoration<ReadConcernArgs> handle =
    OperationContext::declareDecoration<ReadConcernArgs>();

}  // namespace

ReadConcernArgs& ReadConcernArgs::get(OperationContext* opCtx) {
    return handle(opCtx);
}

const ReadConcernArgs& ReadConcernArgs::get(const OperationContext* opCtx) {
    return handle(opCtx);
}
}  // namespace repl
}  // namespace mongo
