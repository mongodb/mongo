// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/sorter/sorter_gen.h"
#include "mongo/util/modules.h"

namespace mongo {

namespace [[MONGO_MOD_PUBLIC]] sorter {
static constexpr SorterChecksumVersion kLatestChecksumVersion = SorterChecksumVersion::v2;
}

/**
 * Calculates the checksum of a given version, by gradually consuming provided data.
 */
class SorterChecksumCalculator {
public:
    SorterChecksumCalculator(SorterChecksumVersion version, size_t seed = 0)
        : _version(version), _checksum(seed), _uncommittedChecksum(seed) {}

    void addData(const char* data, size_t size);

    /**
     * Advances the uncommitted checksum. checksum() does not reflect these bytes until commit().
     */
    void addUncommittedData(const char* data, size_t size);

    /**
     * Promotes the uncommitted checksum to the committed checksum. No-op if nothing is pending.
     */
    void commit();

    /**
     * Discards the uncommitted checksum, reverting to the committed checksum. No-op if nothing is
     * pending.
     */
    void abort();

    size_t checksum() const {
        return _checksum;
    }

    SorterChecksumVersion version() const {
        return _version;
    }

private:
    size_t _advanceChecksum(size_t seed, const char* data, size_t size) const;

    const SorterChecksumVersion _version;
    size_t _checksum = 0;
    size_t _uncommittedChecksum = 0;
};

}  // namespace mongo
