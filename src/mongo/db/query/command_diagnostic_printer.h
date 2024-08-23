/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#pragma once

#include <fmt/format.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/redaction.h"

/*
 * This file contains functions which will compute a diagnostic string for use during a tassert or
 * invariant failure. These structs capture some information about a command, such as the original
 * command object, and implement a format() function which will be invoked only in the case of a
 * failure, which allows us to avoid performing unncecessary calculations on the hot path.
 */

namespace mongo::command_diagnostics {

struct Printer {
    auto format(auto& fc) const {
        // It's assumed that 'opCtx' is a valid pointer and has been decorated with a CurOp. This
        // class should not be used in scopes where that assumption doesn't hold.
        auto& curOp = *CurOp::get(opCtx);
        auto out = fc.out();
        out = format_to(
            out,
            FMT_STRING("{{'currentOp': {}, 'opDescription': {}, 'originatingCommand': {}}}"),
            redact(serializeOpDebug(curOp)).toString(),
            redact(curOp.opDescription()).toString(),
            redact(curOp.originatingCommand()).toString());
        return out;
    }

    OperationContext* opCtx;

private:
    BSONObj serializeOpDebug(CurOp& curOp) const {
        BSONObjBuilder bob;
        // 'opDescription' and 'originatingCommand' are logged separately to avoid having to
        // truncate them due to the BSON size limit.
        const bool omitCommand = true;
        curOp.debug().append(opCtx, {} /*lockStats*/, {} /*flowControlStats*/, omitCommand, bob);
        return bob.obj();
    }
};

}  // namespace mongo::command_diagnostics

namespace fmt {

template <>
struct formatter<mongo::command_diagnostics::Printer> {
    constexpr auto parse(auto& ctx) {
        return ctx.begin();
    }

    auto format(const mongo::command_diagnostics::Printer& obj, auto& ctx) {
        return obj.format(ctx);
    }
};

}  // namespace fmt
