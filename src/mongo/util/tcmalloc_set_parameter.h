// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"

#include <functional>
#include <string_view>

namespace mongo {

#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
using TcmallocReleaseRateT = long long;
#elif defined(MONGO_CONFIG_TCMALLOC_GPERF)
using TcmallocReleaseRateT = double;
#endif  // MONGO_CONFIG_TCMALLOC_GOOGLE

constexpr inline std::string_view kMaxPerCPUCacheSizePropertyName{
    "tcmalloc.max_per_cpu_cache_size"};
constexpr inline std::string_view kMaxTotalThreadCacheBytesPropertyName{
    "tcmalloc.max_total_thread_cache_bytes"};
constexpr inline std::string_view kAggressiveMemoryDecommitPropertyName{
    "tcmalloc.aggressive_memory_decommit"};

size_t getTcmallocProperty(std::string_view propname);
void setTcmallocProperty(std::string_view propname, size_t value);
TcmallocReleaseRateT getMemoryReleaseRate();
void setMemoryReleaseRate(TcmallocReleaseRateT val);

Status onUpdateHeapProfilingSampleIntervalBytes(long long newValue);
Status validateHeapProfilingSampleIntervalBytes(long long newValue,
                                                const boost::optional<TenantId>& tenantId);
Status onUpdateHeapProfilingMaxObjects(long long newValue);

// Allows heap_profiler.cpp to register a callback for updating max sampled objects.
void setHeapProfilingMaxObjectsCallback(std::function<void(long long)> callback);
}  // namespace mongo
