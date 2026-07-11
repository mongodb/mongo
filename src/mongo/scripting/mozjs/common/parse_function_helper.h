// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once


#include <string>
#include <string_view>

#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

/**
 * Initializes Reflect.parse on `global` and installs `__parseJSFunctionOrExpression`
 * as a named property on `global`. The helper captures Reflect at install time so it
 * remains usable even if Reflect is later removed from the global (e.g. in the shell).
 *
 * Must be called with JSAutoRealm entered for `global`.
 * Returns false and leaves a JS exception pending on failure.
 */
bool installParseJSFunctionHelper(JSContext* cx, JS::HandleObject global);

/**
 * Calls `__parseJSFunctionOrExpression(input)` on `global` and writes the normalized
 * function-expression source into `*out`. Mirrors mongohelpers.js::functionExpressionParser.
 *
 * Must be called with JSAutoRealm entered for `global`.
 * Returns false and leaves a JS exception pending on failure.
 */
bool parseJSFunctionOrExpression(JSContext* cx,
                                 JS::HandleObject global,
                                 std::string_view input,
                                 std::string* out);

}  // namespace mozjs
}  // namespace mongo
