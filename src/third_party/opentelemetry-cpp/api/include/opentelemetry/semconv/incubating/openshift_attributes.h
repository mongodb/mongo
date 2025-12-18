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
namespace openshift
{

/**
  The name of the cluster quota.
 */
static constexpr const char *kOpenshiftClusterquotaName = "openshift.clusterquota.name";

/**
  The UID of the cluster quota.
 */
static constexpr const char *kOpenshiftClusterquotaUid = "openshift.clusterquota.uid";

}  // namespace openshift
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
