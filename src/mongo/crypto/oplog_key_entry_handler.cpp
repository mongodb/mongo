// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/crypto/oplog_key_entry_handler.h"

#include "mongo/base/status.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/service_context.h"
#include "mongo/util/serialization_context.h"

#include <memory>

namespace mongo {

namespace {
const auto oplogKeyEntryHandler =
    ServiceContext::declareDecoration<std::unique_ptr<OplogKeyEntryHandler>>();

ServiceContext::ConstructorActionRegisterer oplogKeyEntryHandlerRegisterer{
    "OplogKeyEntryHandler", [](ServiceContext* serviceContext) {
        auto oplogKeyEntryHandler = std::make_unique<OplogKeyEntryHandler>();
        OplogKeyEntryHandler::set(serviceContext, std::move(oplogKeyEntryHandler));
    }};

}  // namespace

Status OplogKeyEntryHandler::applyOplogEntry(OperationContext* opCtx,
                                             const repl::OplogEntry& oplogEntry) {
    return Status::OK();
}

void OplogKeyEntryHandler::set(ServiceContext* serviceContext,
                               std::unique_ptr<OplogKeyEntryHandler> handler) {
    auto& handle = oplogKeyEntryHandler(serviceContext);
    handle = std::move(handler);
}

OplogKeyEntryHandler* OplogKeyEntryHandler::get(ServiceContext* serviceContext) {
    return oplogKeyEntryHandler(serviceContext).get();
}

}  // namespace mongo
