// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
    std::lock_guard<std::mutex> lk(_cacheMutex);
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
    std::lock_guard<std::mutex> lk(_cacheMutex);
    if (_cache) {
        _cache = boost::none;
    }
}

}  // namespace mongo
