// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <fmt/format.h>

/*
 * This file contains functions which will compute a diagnostic string for use during a tassert or
 * invariant failure. These structs capture some information about a command, such as the original
 * command object, and implement a format() function which will be invoked only in the case of a
 * failure, which allows us to avoid performing unnecessary calculations on the hot path.
 */

namespace mongo::diagnostic_printers {
using namespace std::literals::string_view_literals;

constexpr inline auto kOmitUnsupportedCurOpMsg =
    "omitted: this CurOp does not support diagnostic printing"sv;
constexpr inline auto kOmitUnrecognizedCommandMsg = "omitted: unrecognized command"sv;
constexpr inline auto kOmitUnsupportedCommandMsg =
    "omitted: command does not support diagnostic printing"sv;
constexpr inline auto kOpCtxIsNullMsg = "opCtx is null"sv;
constexpr inline auto kCurOpIsNullMsg = "the opCtx's curOp is null"sv;

/**
 * Indicates if the operation associated with 'opCtx' is ineligible for diagnostic logging. If the
 * operation isn't eligible, returns a message indicating why. Otherwise, returns boost::none. For
 * example, this function ensures that the CurOp stack and Command permit the logging.
 */
boost::optional<std::string_view> isIneligibleForDiagnosticPrinting(OperationContext* opCtx);

class [[MONGO_MOD_PUBLIC]] CurOpPrinter {
public:
    explicit CurOpPrinter(OperationContext* opCtx) : _opCtx{opCtx} {}

    auto format(auto& fc) const {
        auto out = fc.out();
        if (auto msg = isIneligibleForDiagnosticPrinting(_opCtx))
            return fmt::format_to(out, "{}", *msg);

        Info info = _gatherInfo();
        out = fmt::format_to(out, "{{");
        auto field = [&, sep = ""sv](std::string_view name, const auto& value) mutable {
            out = fmt::format_to(out, "{}'{}': {}", std::exchange(sep, ", "sv), name, value);
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
