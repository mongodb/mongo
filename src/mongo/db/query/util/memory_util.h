// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string>
#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo::memory_util {

/**
 * Defines units of memory.
 */
enum class MemoryUnits {
    kPercent,
    kBytes,
    kKB,
    kMB,
    kGB,
};

struct MemorySize;
uint64_t convertToSizeInBytes(const MemorySize& memSize);

/**
 * Represents parsed memory size parameter.
 */
struct MemorySize {
    static StatusWith<MemorySize> parse(const std::string& str);

    static MemorySize parseFromBSON(const BSONElement& element);

    void serializeToBSON(std::string_view fieldName, BSONObjBuilder* builder) const;
    void serializeToBSON(BSONArrayBuilder* builder) const;

    operator uint64_t() const {
        return convertToSizeInBytes(*this);
    }

    double size;
    MemoryUnits units = MemoryUnits::kBytes;
};

StatusWith<MemoryUnits> parseUnitString(const std::string& strUnit);
size_t capMemorySize(size_t requestedSizeBytes,
                     size_t maximumSizeGB,
                     double percentTotalSystemMemory);
size_t getRequestedMemSizeInBytes(const MemorySize& memSize);

/**
 * Callback called on validation of the 'planCacheSize' parameter.
 */
Status validatePlanCacheSize(const std::string& str, const boost::optional<TenantId>&);


}  // namespace mongo::memory_util
