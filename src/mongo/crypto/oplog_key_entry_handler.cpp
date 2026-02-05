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
