/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/base/status_with.h"
#include "mongo/db/signed_logical_time.h"

namespace mongo {

class BSONElement;
class BSONObjBuilder;

namespace rpc {

/**
 * Format:
 * logicalTime: {
 *     clusterTime: <Timestamp>,
 *     signature: {
 *         hash: <SHA1 hash of clusterTime as BinData>,
 *         keyId: <long long>
 *     }
 * }
 */
class LogicalTimeMetadata {
public:
    LogicalTimeMetadata() = default;
    explicit LogicalTimeMetadata(SignedLogicalTime time);

    /**
     * Parses the metadata from BSON. Returns an empty LogicalTimeMetadata If the metadata is not
     * present.
     */
    static StatusWith<LogicalTimeMetadata> readFromMetadata(const BSONObj& metadata);
    static StatusWith<LogicalTimeMetadata> readFromMetadata(const BSONElement& metadataElem);

    void writeToMetadata(BSONObjBuilder* metadataBuilder) const;

    const SignedLogicalTime& getSignedTime() const;

    static StringData fieldName() {
        return "logicalTime";
    }

private:
    SignedLogicalTime _clusterTime;
};

}  // namespace rpc
}  // namespace mongo
