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
namespace az
{

/**
  Deprecated, use @code azure.resource_provider.namespace @endcode instead.

  @deprecated
  {"note": "Replaced by @code azure.resource_provider.namespace @endcode.", "reason": "renamed",
  "renamed_to": "azure.resource_provider.namespace"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kAzNamespace = "az.namespace";

/**
  Deprecated, use @code azure.service.request.id @endcode instead.

  @deprecated
  {"note": "Replaced by @code azure.service.request.id @endcode.", "reason": "renamed",
  "renamed_to": "azure.service.request.id"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kAzServiceRequestId = "az.service_request_id";

}  // namespace az
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
