// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "src/common/platform/fork.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace common
{
namespace platform
{
int AtFork(void (*prepare)(), void (*parent)(), void (*child)()) noexcept
{
  (void)prepare;
  (void)parent;
  (void)child;
  return 0;
}
}  // namespace platform
}  // namespace common
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
