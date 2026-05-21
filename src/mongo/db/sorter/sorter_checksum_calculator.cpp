/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
