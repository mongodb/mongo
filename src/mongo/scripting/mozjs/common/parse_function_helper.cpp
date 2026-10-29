// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/common/parse_function_helper.h"

#include "mongo/scripting/mozjs/common/valuereader.h"
#include "mongo/scripting/mozjs/common/valuewriter.h"

#include <cstring>
#include <string_view>

#include <jsapi.h>

#include <js/CompilationAndEvaluation.h>
#include <js/SourceText.h>
#include <js/Value.h>
#include <js/ValueArray.h>
#include <mozilla/Utf8.h>

namespace mongo {
namespace mozjs {

namespace {

// Parses a JS function or expression using Reflect.parse(). Wrapped in an IIFE that captures
// Reflect in a closure so it remains available even if Reflect is later removed from the global.
static const char kParseHelperSrc[] = R"js(
(function() {
    const _Reflect = Reflect;
    // Non-enumerable + non-writable + configurable: hidden from Object.keys() and cannot be
    // replaced by user code, but configurable so postInstall() can move it off the global.
    Object.defineProperty(globalThis, '__parseJSFunctionOrExpression', {
        configurable: true,
        value: function(fnSrc) {
        // Ensure that a provided expression or function body is not terminated with a ';'.
        // This ensures we interpret the input as a single expression, rather than a sequence
        // of expressions, and can wrap it in parentheses.
        while (fnSrc.endsWith(";") || fnSrc != fnSrc.trimRight()) {
            fnSrc = fnSrc.slice(0, -1).trimRight();
        }

        let parseTree;
        try {
            parseTree = _Reflect.parse(fnSrc);
        } catch (e) {
            if (e == "SyntaxError: function statement requires a name") {
                return fnSrc;
            } else if (e == "SyntaxError: return not in function") {
                return "function() { " + fnSrc + " }";
            } else {
                throw e;
            }
        }
        // Input source is a series of expressions. we should prepend the last one with return
        let lastStatement = parseTree.body.length - 1;
        let lastStatementType = parseTree.body[lastStatement].type;
        if (lastStatementType == "ExpressionStatement") {
            let prevExprEnd = 0;
            let loc = parseTree.body[lastStatement].loc.start;

            // When we're actually doing the pre-pending of return later on we need to know
            // whether we've reached the beginning of the line, or the end of the 2nd-to-last
            // expression.
            if (lastStatement > 0) {
                prevExprEnd = parseTree.body[lastStatement - 1].loc.end;
                if (prevExprEnd.line != loc.line) {
                    prevExprEnd = 0;
                } else {
                    // Starting in MozJS ESR128, the column numbers returned by the engine are
                    // 1-indexed. To ensure we reference the correct substring when we perform
                    // string manipulation below, we need to subtract 1.
                    prevExprEnd = prevExprEnd.column - 1;
                }
            }

            let lines = fnSrc.split("\n");
            // Adjust for 1-indexed column number by substracting 1.
            let col = loc.column - 1;
            var fnSrc;
            let tmpTree;
            let origLine = lines[loc.line - 1];

            // The parser has a weird behavior where sometimes if you have an expression like
            // ((x == 5)), it says that the expression string is "x == 5))", so we may need to
            // adjust where we prepend "return".
            while (col >= prevExprEnd) {
                let modLine = origLine.substr(0, col) + "return " + origLine.substr(col);
                lines[loc.line - 1] = modLine;
                fnSrc = "{ " + lines.join("\n") + " }";
                try {
                    tmpTree = _Reflect.parse("function x() " + fnSrc);
                } catch (e) {
                    col -= 1;
                    continue;
                }
                break;
            }

            return "function() " + fnSrc;
        } else if (lastStatementType == "FunctionDeclaration") {
            return fnSrc;
        } else {
            return "function() { " + fnSrc + " }";
        }
        },
        enumerable: false,
        writable: false,
    });
})();
)js";

}  // namespace

bool installParseJSFunctionHelper(JSContext* cx, JS::HandleObject global) {
    if (!JS_InitReflectParse(cx, global))
        return false;

    JS::CompileOptions opts(cx);
    opts.setFileAndLine("common:parseHelper", 1);

    JS::SourceText<mozilla::Utf8Unit> src;
    if (!src.init(cx, kParseHelperSrc, std::strlen(kParseHelperSrc), JS::SourceOwnership::Borrowed))
        return false;

    JS::RootedValue result(cx);
    return JS::Evaluate(cx, opts, src, &result);
}

bool parseJSFunctionOrExpression(JSContext* cx,
                                 JS::HandleObject global,
                                 std::string_view input,
                                 std::string* out) {
    JS::RootedValue helperVal(cx);
    if (!JS_GetProperty(cx, global, "__parseJSFunctionOrExpression", &helperVal))
        return false;

    JS::RootedValue inputVal(cx);
    ValueReader(cx, &inputVal).fromStringData(input);

    JS::RootedValue result(cx);
    if (!JS::Call(cx, global, helperVal, JS::HandleValueArray(inputVal), &result))
        return false;

    *out = ValueWriter(cx, result).toString();
    return true;
}

}  // namespace mozjs
}  // namespace mongo
