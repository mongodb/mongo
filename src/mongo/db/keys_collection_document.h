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
#include "mongo/db/logical_time.h"
#include "mongo/db/time_proof_service.h"

namespace mongo {

/**
 * Class for parsing and serializing a key document stored in admin.system.keys.
 *
 * Format:
 * {
 *     _id: <long long>,
 *     purpose: 'signLogicalTime',
 *     key: <20 byte key generated with secure PRNG in BinData>,
 *     expiresAt: <LogicalTime, currently in Timestamp format>
 * }
 */
class KeysCollectionDocument {
public:
    KeysCollectionDocument(long long keyId,
                           std::string purpose,
                           TimeProofService::Key key,
                           LogicalTime expiresAt)
        : _keyId(keyId),
          _purpose(std::move(purpose)),
          _key(std::move(key)),
          _expiresAt(std::move(expiresAt)) {}

    /**
     * Parses the key document from BSON.
     */
    static StatusWith<KeysCollectionDocument> fromBSON(const BSONObj& source);

    /**
     * Serialize the key document as BSON.
     */
    BSONObj toBSON() const;

    long long getKeyId() const;

    const std::string& getPurpose() const;

    const TimeProofService::Key& getKey() const;

    const LogicalTime& getExpiresAt() const;

private:
    long long _keyId;
    std::string _purpose;
    TimeProofService::Key _key;
    LogicalTime _expiresAt;
};

}  // namespace mongo
