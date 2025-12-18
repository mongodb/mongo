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
namespace graphql
{

/**
  The GraphQL document being executed.
  <p>
  The value may be sanitized to exclude sensitive information.
 */
static constexpr const char *kGraphqlDocument = "graphql.document";

/**
  The name of the operation being executed.
 */
static constexpr const char *kGraphqlOperationName = "graphql.operation.name";

/**
  The type of the operation being executed.
 */
static constexpr const char *kGraphqlOperationType = "graphql.operation.type";

namespace GraphqlOperationTypeValues
{
/**
  GraphQL query
 */
static constexpr const char *kQuery = "query";

/**
  GraphQL mutation
 */
static constexpr const char *kMutation = "mutation";

/**
  GraphQL subscription
 */
static constexpr const char *kSubscription = "subscription";

}  // namespace GraphqlOperationTypeValues

}  // namespace graphql
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
