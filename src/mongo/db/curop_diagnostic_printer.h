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

#include "mongo/base/string_data.h"
#include "mongo/db/operation_context.h"

#include <fmt/format.h>

/*
 * This file contains functions which will compute a diagnostic string for use during a tassert or
 * invariant failure. These structs capture some information about a command, such as the original
 * command object, and implement a format() function which will be invoked only in the case of a
 * failure, which allows us to avoid performing unnecessary calculations on the hot path.
 */

namespace mongo::diagnostic_printers {

constexpr inline auto kOmitUnsupportedCurOpMsg =
    "omitted: this CurOp does not support diagnostic printing"_sd;
constexpr inline auto kOmitUnrecognizedCommandMsg = "omitted: unrecognized command"_sd;
constexpr inline auto kOmitUnsupportedCommandMsg =
    "omitted: command does not support diagnostic printing"_sd;
constexpr inline auto kOpCtxIsNullMsg = "opCtx is null"_sd;
constexpr inline auto kCurOpIsNullMsg = "the opCtx's curOp is null"_sd;

/**
 * Indicates if the operation associated with 'opCtx' is ineligible for diagnostic logging. If the
 * operation isn't eligible, returns a message indicating why. Otherwise, returns boost::none. For
 * example, this function ensures that the CurOp stack and Command permit the logging.
 */
boost::optional<StringData> isIneligibleForDiagnosticPrinting(OperationContext* opCtx);

class CurOpPrinter {
public:
    explicit CurOpPrinter(OperationContext* opCtx) : _opCtx{opCtx} {}

    auto format(auto& fc) const {
        auto out = fc.out();
        if (auto msg = isIneligibleForDiagnosticPrinting(_opCtx))
            return fmt::format_to(out, "{}", *msg);

        Info info = _gatherInfo();
        out = fmt::format_to(out, "{{");
        auto field = [&, sep = ""_sd](StringData name, const auto& value) mutable {
            out = fmt::format_to(out, "{}'{}': {}", std::exchange(sep, ", "_sd), name, value);
        };
        field("currentOp", info.opDebug);
        field("opDescription", info.opDesc);
        if (info.origCommand)
            field("originatingCommand", *info.origCommand);
        out = fmt::format_to(out, "}}");
        return out;
    }

private:
    struct Info {
        std::string opDesc;
        std::string opDebug;
        boost::optional<std::string> origCommand;
    };

    Info _gatherInfo() const;

    // This pointer must outlive this class.
    OperationContext* _opCtx;
};

}  // namespace mongo::diagnostic_printers

namespace fmt {

template <>
struct formatter<mongo::diagnostic_printers::CurOpPrinter> {
    constexpr auto parse(auto& ctx) {
        return ctx.begin();
    }

    auto format(const mongo::diagnostic_printers::CurOpPrinter& obj, auto& ctx) const {
        return obj.format(ctx);
    }
};

}  // namespace fmt
