/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/time_proof_service.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/logical_time.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"

#include <array>
#include <cstdint>
#include <mutex>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * This value defines the range of times that match the cache. It is assumed that the cluster times
 * are staying within the range so the range size is defined by the mask. This assumes that the
 * implementation has a form or high 32 bit: secs low 32 bit: increment.
 */
const uint64_t kRangeMask = 0x0000'0000'0000'FFFF;

TimeProofService::Key TimeProofService::generateRandomKey() {
    std::array<std::uint8_t, SHA1Block::kHashLength> keyBuffer;
    SecureRandom().fill(keyBuffer.data(), keyBuffer.size());
    return fassert(40384, SHA1Block::fromBuffer(keyBuffer.data(), keyBuffer.size()));
}

TimeProofService::TimeProof TimeProofService::getProof(LogicalTime time, const Key& key) {
    stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
    auto timeCeil = LogicalTime(Timestamp(time.asTimestamp().asULL() | kRangeMask));
    if (_cache && _cache->hasProof(timeCeil, key)) {
        return _cache->_proof;
    }

    auto unsignedTimeArray = timeCeil.toUnsignedArray();
    // update cache
    _cache =
        CacheEntry(SHA1Block::computeHmac(
                       key.data(), key.size(), unsignedTimeArray.data(), unsignedTimeArray.size()),
                   timeCeil,
                   key);
    return _cache->_proof;
}

Status TimeProofService::checkProof(LogicalTime time, const TimeProof& proof, const Key& key) {
    auto myProof = getProof(time, key);
    if (myProof != proof) {
        return Status(ErrorCodes::TimeProofMismatch, "Proof does not match the cluster time");
    }
    return Status::OK();
}

void TimeProofService::resetCache() {
    stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
    if (_cache) {
        _cache = boost::none;
    }
}

}  // namespace mongo
