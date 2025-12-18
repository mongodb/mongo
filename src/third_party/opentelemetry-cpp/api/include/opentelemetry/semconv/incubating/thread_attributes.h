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
namespace thread
{

/**
  Current "managed" thread ID (as opposed to OS thread ID).
  <p>
  Examples of where the value can be extracted from:
  <p>
  | Language or platform  | Source |
  | --- | --- |
  | JVM | @code Thread.currentThread().threadId() @endcode |
  | .NET | @code Thread.CurrentThread.ManagedThreadId @endcode |
  | Python | @code threading.current_thread().ident @endcode |
  | Ruby | @code Thread.current.object_id @endcode |
  | C++ | @code std::this_thread::get_id() @endcode |
  | Erlang | @code erlang:self() @endcode |
 */
static constexpr const char *kThreadId = "thread.id";

/**
  Current thread name.
  <p>
  Examples of where the value can be extracted from:
  <p>
  | Language or platform  | Source |
  | --- | --- |
  | JVM | @code Thread.currentThread().getName() @endcode |
  | .NET | @code Thread.CurrentThread.Name @endcode |
  | Python | @code threading.current_thread().name @endcode |
  | Ruby | @code Thread.current.name @endcode |
  | Erlang | @code erlang:process_info(self(), registered_name) @endcode |
 */
static constexpr const char *kThreadName = "thread.name";

}  // namespace thread
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
