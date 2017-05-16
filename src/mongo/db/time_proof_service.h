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

#pragma once

#include "mongo/base/status.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/db/logical_time.h"

namespace mongo {

/**
 * TODO: SERVER-28127 Add key rotation to the TimeProofService
 *
 * The TimeProofService holds the key used by mongod and mongos processes to verify logical times
 * and contains the logic to generate this key, but not to store or retrieve it.
 */
class TimeProofService {
public:
    // This type must be synchronized with the library that generates SHA1 or other proof.
    using TimeProof = SHA1Block;
    using Key = SHA1Block;

    TimeProofService() = default;

    /**
     * Generates a pseudorandom key to be used for HMAC authentication.
     */
    static Key generateRandomKey();

    /**
     * Returns the proof matching the time argument.
     */
    TimeProof getProof(const LogicalTime& time, const Key& key) const;

    /**
     * Verifies that the proof matches the time argument.
     */
    Status checkProof(const LogicalTime& time, const TimeProof& proof, const Key& key) const;
};

}  // namespace mongo
