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
namespace elasticsearch
{

/**
  Represents the human-readable identifier of the node/instance to which a request was routed.
 */
static constexpr const char *kElasticsearchNodeName = "elasticsearch.node.name";

}  // namespace elasticsearch
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
