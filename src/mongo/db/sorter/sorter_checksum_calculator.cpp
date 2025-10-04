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

void SorterChecksumCalculator::addData(const char* data, size_t size) {
    switch (_version) {
        case SorterChecksumVersion::v1:
            _checksum = murmur3<sizeof(uint32_t)>(ConstDataRange{data, size}, _checksum);
            return;
        case SorterChecksumVersion::v2:
#ifdef MONGO_CONFIG_WIREDTIGER_ENABLED
            _checksum = wiredtiger_crc32c_with_seed_func()(_checksum, data, size);
#else
            MONGO_UNIMPLEMENTED_TASSERT(7770500);
#endif
            return;
    }
    tasserted(7784000,
              str::stream() << "Unknown sorter checksum version: "
                            << SorterChecksumVersion_serializer(_version)
                            << ". Is it possible you are reading sorter files left from a newer "
                            << "version of MongoDB?");
}

}  // namespace mongo
