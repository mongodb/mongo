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
namespace nfs
{

/**
  NFSv4+ operation name.
 */
static constexpr const char *kNfsOperationName = "nfs.operation.name";

/**
  Linux: one of "hit" (NFSD_STATS_RC_HITS), "miss" (NFSD_STATS_RC_MISSES), or "nocache"
  (NFSD_STATS_RC_NOCACHE -- uncacheable)
 */
static constexpr const char *kNfsServerRepcacheStatus = "nfs.server.repcache.status";

}  // namespace nfs
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
