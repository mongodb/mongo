// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/curop_diagnostic_printer.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/redaction.h"

#include <string_view>

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

boost::optional<std::string_view> isIneligibleForDiagnosticPrinting(OperationContext* opCtx) {
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
