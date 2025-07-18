// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/exporters/otlp/otlp_grpc_utils.h"

OPENTELEMETRY_BEGIN_NAMESPACE

namespace exporter
{
namespace otlp
{
namespace grpc_utils
{

const char *grpc_status_code_to_string(::grpc::StatusCode status_code)
{
  switch (status_code)
  {
    case ::grpc::StatusCode::OK:
      return "OK";
    case ::grpc::StatusCode::CANCELLED:
      return "CANCELLED";
    case ::grpc::StatusCode::UNKNOWN:
      return "UNKNOWN";
    case ::grpc::StatusCode::INVALID_ARGUMENT:
      return "INVALID_ARGUMENT";
    case ::grpc::StatusCode::DEADLINE_EXCEEDED:
      return "DEADLINE_EXCEEDED";
    case ::grpc::StatusCode::NOT_FOUND:
      return "NOT_FOUND";
    case ::grpc::StatusCode::ALREADY_EXISTS:
      return "ALREADY_EXISTS";
    case ::grpc::StatusCode::PERMISSION_DENIED:
      return "PERMISSION_DENIED";
    case ::grpc::StatusCode::UNAUTHENTICATED:
      return "UNAUTHENTICATED";
    case ::grpc::StatusCode::RESOURCE_EXHAUSTED:
      return "RESOURCE_EXHAUSTED";
    case ::grpc::StatusCode::FAILED_PRECONDITION:
      return "FAILED_PRECONDITION";
    case ::grpc::StatusCode::ABORTED:
      return "ABORTED";
    case ::grpc::StatusCode::OUT_OF_RANGE:
      return "OUT_OF_RANGE";
    case ::grpc::StatusCode::UNIMPLEMENTED:
      return "UNIMPLEMENTED";
    case ::grpc::StatusCode::INTERNAL:
      return "INTERNAL";
    case ::grpc::StatusCode::UNAVAILABLE:
      return "UNAVAILABLE";
    case ::grpc::StatusCode::DATA_LOSS:
      return "DATA_LOSS";
    default:
      return "UNKNOWN";
  }
}

}  // namespace grpc_utils
}  // namespace otlp
}  // namespace exporter

OPENTELEMETRY_END_NAMESPACE
