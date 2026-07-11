// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"

namespace mongo::join_ordering {

/**
 * Statistics for a single collection.
 */
struct CollectionStats {
    CollectionStats(double logicalDataSizeBytes,
                    double onDiskSizeBytes,
                    double pageSizeBytes = 32 * 1024)
        : logicalDataSizeBytes(logicalDataSizeBytes),
          _onDiskSizeBytes(onDiskSizeBytes),
          _pageSizeBytes(pageSizeBytes) {}

    /**
     * Returns the estimated number of on-disk pages for this collection, rounded to the nearest
     * power of 2^(1/4). The purpose is to smooth-out small variations that can occur in
     * onDiskSizeBytes across different runs of plan stability tests which invoke `mongorestore`
     * each time. Returns 0 if the collection has no on-disk data. Callers should always prefer this
     * over accessing the raw on-disk size directly, to avoid sensitivity to non-deterministic
     * storage engine values.
     */
    double numPages() const;

    // Estimate of the data size of this collection when in-memory (uncompressed and unencrypted).
    double logicalDataSizeBytes;

private:
    // Estimate of the data size of this collection on-disk post compression. Not exposed directly —
    // callers must use numOnDiskPages() which applies quantization to absorb small
    // platform-dependent differences in the raw value (e.g. between mongorestore runs).
    double _onDiskSizeBytes;

    // Approximate size, in bytes, of a single WT data page on-disk. The optimizer uses this as the
    // I/O granularity when estimating the number of disk I/Os performed by an operator for cost
    // estimates. Default to 32KiB if not specified.
    double _pageSizeBytes;
};

/**
 * Statistics extracted from the catalog useful for cost estimation.
 */
struct CatalogStats {
    /**
     * Estimate the number of pages from the given collection that would fit in the WT cache. This
     * function is parameterized by collection as we've found that different collections can have
     * very different average page sizes in memory.
     */
    double numPagesInStorageEngineCache(const NamespaceString& nss) const;

    stdx::unordered_map<NamespaceString, CollectionStats> collStats;

    // Default to 2GiB.
    double bytesInStorageEngineCache{2.0 * 1024 * 1024 * 1024};
};

// For a single collection, the maximum number of distinct fields that are part of unique indexes
// which we will use to determine join field uniqueness. If there are more than this many fields,
// only the first 'kMaxUniqueFieldsPerCollection' fields will be used, and the rest will be ignored.
constexpr size_t kMaxUniqueFieldsPerCollection = 64;

// A combination of fields which, based on index metadata, are known to represent unique data.
using UniqueFieldSet = std::bitset<kMaxUniqueFieldsPerCollection>;
using UniqueFieldSets = absl::flat_hash_set<UniqueFieldSet>;

// Maps from field to the bit assigned to that field.
using FieldToBit = absl::flat_hash_map<FieldPath, int>;

struct UniqueFieldInformation {
    // Maps from field to bit assigned to that field.
    FieldToBit fieldToBit;
    // A combination of fields which, based on index metadata, are known to represent unique data.
    UniqueFieldSets uniqueFieldSet;
};

/**
 * Given a keyPattern from an index assumed to be unique, constructs its unique field information.
 * Note that this function modifies 'fieldToBit' if new fields requiring new bits are seen.
 */
boost::optional<UniqueFieldSet> buildUniqueFieldSetForIndex(const BSONObj& keyPattern,
                                                            FieldToBit& fieldToBit);

/**
 * Returns whether the 'ndvFields' represent unique values given the index-derived metadata
 * 'uniqueFields', which indicates what combinations of fields are guaranteed to be unique.
 *
 * For example, given unique fields {{"a"}, {"b", "c"}}, then given the following NDV fields...
 * - {"a"}           --> return true
 * - {"b", "c"}      --> return true
 * - {"b", "c", "d"} --> return true
 * - {"e", "c"}      --> return false
 */
bool fieldsAreUnique(const std::set<FieldPath>& ndvFields,
                     const UniqueFieldInformation& uniqueFields);

}  // namespace mongo::join_ordering
