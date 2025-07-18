// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <grpcpp/grpcpp.h>

#include "opentelemetry/sdk/version/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE

namespace exporter
{
namespace otlp
{
namespace grpc_utils
{

const char *grpc_status_code_to_string(::grpc::StatusCode status_code);

}  // namespace grpc_utils
}  // namespace otlp
}  // namespace exporter

OPENTELEMETRY_END_NAMESPACE
