// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace common
{
/**
 * ExportResult is returned as result of exporting a batch of Records.
 */
enum class ExportResult
{
  // Batch was exported successfully.
  kSuccess = 0,

  // Batch exporting failed, caller must not retry exporting the same batch
  // and the batch must be dropped.
  kFailure = 1,

  // The collection does not have enough space to receive the export batch.
  kFailureFull = 2,

  // The export() function was passed an invalid argument.
  kFailureInvalidArgument = 3
};

}  // namespace common
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
