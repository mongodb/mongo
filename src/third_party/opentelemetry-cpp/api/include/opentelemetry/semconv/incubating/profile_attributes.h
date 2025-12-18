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
namespace profile
{

/**
  Describes the interpreter or compiler of a single frame.
 */
static constexpr const char *kProfileFrameType = "profile.frame.type";

namespace ProfileFrameTypeValues
{
/**
  <a href="https://wikipedia.org/wiki/.NET">.NET</a>
 */
static constexpr const char *kDotnet = "dotnet";

/**
  <a href="https://wikipedia.org/wiki/Java_virtual_machine">JVM</a>
 */
static constexpr const char *kJvm = "jvm";

/**
  <a href="https://wikipedia.org/wiki/Kernel_(operating_system)">Kernel</a>
 */
static constexpr const char *kKernel = "kernel";

/**
  Can be one of but not limited to <a
  href="https://wikipedia.org/wiki/C_(programming_language)">C</a>, <a
  href="https://wikipedia.org/wiki/C%2B%2B">C++</a>, <a
  href="https://wikipedia.org/wiki/Go_(programming_language)">Go</a> or <a
  href="https://wikipedia.org/wiki/Rust_(programming_language)">Rust</a>. If possible, a more
  precise value MUST be used.
 */
static constexpr const char *kNative = "native";

/**
  <a href="https://wikipedia.org/wiki/Perl">Perl</a>
 */
static constexpr const char *kPerl = "perl";

/**
  <a href="https://wikipedia.org/wiki/PHP">PHP</a>
 */
static constexpr const char *kPhp = "php";

/**
  <a href="https://wikipedia.org/wiki/Python_(programming_language)">Python</a>
 */
static constexpr const char *kCpython = "cpython";

/**
  <a href="https://wikipedia.org/wiki/Ruby_(programming_language)">Ruby</a>
 */
static constexpr const char *kRuby = "ruby";

/**
  <a href="https://wikipedia.org/wiki/V8_(JavaScript_engine)">V8JS</a>
 */
static constexpr const char *kV8js = "v8js";

/**
  <a href="https://en.wikipedia.org/wiki/BEAM_(Erlang_virtual_machine)">Erlang</a>
 */
static constexpr const char *kBeam = "beam";

/**
  <a href="https://wikipedia.org/wiki/Go_(programming_language)">Go</a>,
 */
static constexpr const char *kGo = "go";

/**
  <a href="https://wikipedia.org/wiki/Rust_(programming_language)">Rust</a>
 */
static constexpr const char *kRust = "rust";

}  // namespace ProfileFrameTypeValues

}  // namespace profile
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
