// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sorter/sorter_checksum_calculator.h"

#include "mongo/util/murmur3.h"

#ifdef MONGO_CONFIG_WIREDTIGER_ENABLED
#include <wiredtiger.h>
#endif

namespace mongo {

size_t SorterChecksumCalculator::_advanceChecksum(size_t seed,
                                                  const char* data,
                                                  size_t size) const {
    switch (_version) {
        case SorterChecksumVersion::v1:
            return murmur3<sizeof(uint32_t)>(ConstDataRange{data, size}, seed);
        case SorterChecksumVersion::v2:
#ifdef MONGO_CONFIG_WIREDTIGER_ENABLED
            return wiredtiger_crc32c_with_seed_func()(seed, data, size);
#else
            MONGO_UNIMPLEMENTED_TASSERT(7770500);
#endif
    }
    tasserted(7784000,
              str::stream() << "Unknown sorter checksum version: " << idl::serialize(_version)
                            << ". Is it possible you are reading sorter files left from a newer "
                            << "version of MongoDB?");
}

void SorterChecksumCalculator::addData(const char* data, size_t size) {
    _checksum = _advanceChecksum(_checksum, data, size);
    _uncommittedChecksum = _checksum;
}

void SorterChecksumCalculator::addUncommittedData(const char* data, size_t size) {
    _uncommittedChecksum = _advanceChecksum(_uncommittedChecksum, data, size);
}

void SorterChecksumCalculator::commit() {
    _checksum = _uncommittedChecksum;
}

void SorterChecksumCalculator::abort() {
    _uncommittedChecksum = _checksum;
}

}  // namespace mongo
