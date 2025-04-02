/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/curop_diagnostic_printer.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/redaction.h"

namespace mongo::diagnostic_printers {

namespace {

BSONObj serializeOpDebug(OperationContext* opCtx, CurOp& curOp) {
    BSONObjBuilder bob;
    // 'opDescription' and 'originatingCommand' are logged separately to avoid having to
    // truncate them due to the BSON size limit.
    const bool omitCommand = true;
    curOp.debug().append(opCtx,
                         {} /*lockStats*/,
                         {} /*flowControlStats*/,
                         {} /*storageMetrics*/,
                         0 /*prepareReadConflicts*/,
                         omitCommand,
                         bob);
    return bob.obj();
}
}  // namespace

boost::optional<StringData> isIneligibleForDiagnosticPrinting(OperationContext* opCtx) {
    // All operations have an OperationContext, and all OpContexts are decorated with a
    // CurOpStack. This access should always be valid while 'opCtx' is a valid pointer.
    if (!opCtx) {
        return kOpCtxIsNullMsg;
    }
    CurOp* curOp = CurOp::get(opCtx);
    if (!curOp) {
        return kCurOpIsNullMsg;
    }

    // Do not log any information if asked to omit it.
    if (CurOp::shouldCurOpStackOmitDiagnosticInformation(curOp)) {
        return kOmitUnsupportedCurOpMsg;
    }
    const Command* curCommand = curOp->getCommand();
    if (!curCommand) {
        return kOmitUnrecognizedCommandMsg;
    } else if (!curCommand->enableDiagnosticPrintingOnFailure()) {
        return kOmitUnsupportedCommandMsg;
    }
    return boost::none;
}

auto CurOpPrinter::_gatherInfo() const -> Info {
    CurOp* curOp = CurOp::get(_opCtx);
    const Command* curCommand = curOp->getCommand();

    // Remove sensitive fields from the command object before logging.
    // TODO SERVER-74604: When the implementations of OpDebug::append() and OpDebug::report()
    // are merged, we should be able to remove the duplicated logic here that handles
    // 'snipForLogging()' and 'redact()'.
    mutablebson::Document cmdToLog(curOp->opDescription(), mutablebson::Document::kInPlaceDisabled);
    curCommand->snipForLogging(&cmdToLog);
    BSONObj cmd = cmdToLog.getObject();

    return {redact(cmd).toString(),
            redact(serializeOpDebug(_opCtx, *curOp)).toString(),
            curOp->originatingCommand().isEmpty() ? boost::optional<std::string>{}
                                                  : redact(curOp->originatingCommand()).toString()};
}

}  // namespace mongo::diagnostic_printers
