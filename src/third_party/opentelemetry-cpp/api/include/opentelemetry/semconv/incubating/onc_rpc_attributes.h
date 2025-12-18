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
namespace onc_rpc
{

/**
  ONC/Sun RPC procedure name.
 */
static constexpr const char *kOncRpcProcedureName = "onc_rpc.procedure.name";

/**
  ONC/Sun RPC procedure number.
 */
static constexpr const char *kOncRpcProcedureNumber = "onc_rpc.procedure.number";

/**
  ONC/Sun RPC program name.
 */
static constexpr const char *kOncRpcProgramName = "onc_rpc.program.name";

/**
  ONC/Sun RPC program version.
 */
static constexpr const char *kOncRpcVersion = "onc_rpc.version";

}  // namespace onc_rpc
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
