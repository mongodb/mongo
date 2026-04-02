/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/scripting/mozjs/common/parse_function_helper.h"

#include "mongo/scripting/mozjs/common/valuereader.h"
#include "mongo/scripting/mozjs/common/valuewriter.h"

#include <cstring>

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
    globalThis.__parseJSFunctionOrExpression = function(fnSrc) {
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
    };
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
                                 StringData input,
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
