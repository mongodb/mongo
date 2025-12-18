/*
 * Copyright The OpenTelemetry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * DO NOT EDIT, this is an Auto-generated file from:
 * buildscripts/semantic-convention/templates/registry/semantic_attributes-h.j2
 */

#pragma once

#include "opentelemetry/common/macros.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace semconv
{
namespace code
{

/**
  Deprecated, use @code code.column.number @endcode

  @deprecated
  {"note": "Replaced by @code code.column.number @endcode.", "reason": "renamed", "renamed_to":
  "code.column.number"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kCodeColumn = "code.column";

/**
  The column number in @code code.file.path @endcode best representing the operation. It SHOULD
  point within the code unit named in @code code.function.name @endcode. This attribute MUST NOT be
  used on the Profile signal since the data is already captured in 'message Line'. This constraint
  is imposed to prevent redundancy and maintain data integrity.
 */
static constexpr const char *kCodeColumnNumber = "code.column.number";

/**
  The source code file name that identifies the code unit as uniquely as possible (preferably an
  absolute file path). This attribute MUST NOT be used on the Profile signal since the data is
  already captured in 'message Function'. This constraint is imposed to prevent redundancy and
  maintain data integrity.
 */
static constexpr const char *kCodeFilePath = "code.file.path";

/**
  Deprecated, use @code code.file.path @endcode instead

  @deprecated
  {"note": "Replaced by @code code.file.path @endcode.", "reason": "renamed", "renamed_to":
  "code.file.path"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kCodeFilepath = "code.filepath";

/**
  Deprecated, use @code code.function.name @endcode instead

  @deprecated
  {"note": "Value should be included in @code code.function.name @endcode which is expected to be a
  fully-qualified name.\n", "reason": "uncategorized"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kCodeFunction = "code.function";

/**
  The method or function fully-qualified name without arguments. The value should fit the natural
  representation of the language runtime, which is also likely the same used within @code
  code.stacktrace @endcode attribute value. This attribute MUST NOT be used on the Profile signal
  since the data is already captured in 'message Function'. This constraint is imposed to prevent
  redundancy and maintain data integrity. <p> Values and format depends on each language runtime,
  thus it is impossible to provide an exhaustive list of examples. The values are usually the same
  (or prefixes of) the ones found in native stack trace representation stored in
  @code code.stacktrace @endcode without information on arguments.
  <p>
  Examples:
  <ul>
    <li>Java method: @code com.example.MyHttpService.serveRequest @endcode</li>
    <li>Java anonymous class method: @code com.mycompany.Main$1.myMethod @endcode</li>
    <li>Java lambda method: @code com.mycompany.Main$$Lambda/0x0000748ae4149c00.myMethod
  @endcode</li> <li>PHP function: @code GuzzleHttp\Client::transfer @endcode</li> <li>Go function:
  @code github.com/my/repo/pkg.foo.func5 @endcode</li> <li>Elixir: @code OpenTelemetry.Ctx.new
  @endcode</li> <li>Erlang: @code opentelemetry_ctx:new @endcode</li> <li>Rust: @code
  playground::my_module::my_cool_func @endcode</li> <li>C function: @code fopen @endcode</li>
  </ul>
 */
static constexpr const char *kCodeFunctionName = "code.function.name";

/**
  The line number in @code code.file.path @endcode best representing the operation. It SHOULD point
  within the code unit named in @code code.function.name @endcode. This attribute MUST NOT be used
  on the Profile signal since the data is already captured in 'message Line'. This constraint is
  imposed to prevent redundancy and maintain data integrity.
 */
static constexpr const char *kCodeLineNumber = "code.line.number";

/**
  Deprecated, use @code code.line.number @endcode instead

  @deprecated
  {"note": "Replaced by @code code.line.number @endcode.", "reason": "renamed", "renamed_to":
  "code.line.number"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kCodeLineno = "code.lineno";

/**
  Deprecated, namespace is now included into @code code.function.name @endcode

  @deprecated
  {"note": "Value should be included in @code code.function.name @endcode which is expected to be a
  fully-qualified name.\n", "reason": "uncategorized"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kCodeNamespace = "code.namespace";

/**
  A stacktrace as a string in the natural representation for the language runtime. The
  representation is identical to <a
  href="/docs/exceptions/exceptions-spans.md#stacktrace-representation">@code exception.stacktrace
  @endcode</a>. This attribute MUST NOT be used on the Profile signal since the data is already
  captured in 'message Location'. This constraint is imposed to prevent redundancy and maintain data
  integrity.
 */
static constexpr const char *kCodeStacktrace = "code.stacktrace";

}  // namespace code
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
