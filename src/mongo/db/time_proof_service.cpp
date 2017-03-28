/**
 *    Copyright (C) 2017 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/time_proof_service.h"

#include "mongo/base/status.h"
#include "mongo/db/logical_time.h"
#include "mongo/platform/random.h"

namespace mongo {

TimeProofService::Key TimeProofService::generateRandomKey() {
    // SecureRandom only produces 64-bit numbers, so 3 is the minimum for 20 random bytes.
    const size_t kRandomNumbers = 3;
    std::array<std::int64_t, kRandomNumbers> keyBuffer;
    std::unique_ptr<SecureRandom> rng(SecureRandom::create());
    std::generate(keyBuffer.begin(), keyBuffer.end(), [&] { return rng->nextInt64(); });

    return fassertStatusOK(40384,
                           SHA1Block::fromBuffer(reinterpret_cast<std::uint8_t*>(keyBuffer.data()),
                                                 SHA1Block::kHashLength));
}

TimeProofService::TimeProof TimeProofService::getProof(const LogicalTime& time,
                                                       const Key& key) const {
    auto unsignedTimeArray = time.toUnsignedArray();
    return SHA1Block::computeHmac(
        key.data(), key.size(), unsignedTimeArray.data(), unsignedTimeArray.size());
}

Status TimeProofService::checkProof(const LogicalTime& time,
                                    const TimeProof& proof,
                                    const Key& key) const {
    auto myProof = getProof(time, key);
    if (myProof != proof) {
        return Status(ErrorCodes::TimeProofMismatch, "Proof does not match the logical time");
    }
    return Status::OK();
}

}  // namespace mongo
