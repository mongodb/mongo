/**
 *    Copyright 2015 MongoDB Inc.
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

#include "mongo/db/repl/replication_metadata.h"

#include "mongo/bson/util/bson_check.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"

namespace mongo {
namespace repl {

namespace {

const std::string kLastOpCommittedTimestampFieldName = "lastOpCommittedTimestamp";
const std::string kLastOpCommittedTermFieldName = "lastOpCommittedTerm";
const std::string kConfigVersionFieldName = "configVersion";
const std::string kPrimaryIdFieldName = "primaryId";
const std::string kTermFieldName = "term";

const std::string kLegalReplicationMetadataFieldNames[] = {
    kLastOpCommittedTimestampFieldName,
    kLastOpCommittedTermFieldName,
    kConfigVersionFieldName,
    kPrimaryIdFieldName,
    kTermFieldName,
};

}  // namespace

Status ReplicationMetadata::initialize(const BSONObj& metadataObj) {
    Status status = bsonCheckOnlyHasFields(
        "ReplicationMetadata", metadataObj, kLegalReplicationMetadataFieldNames);
    if (!status.isOK())
        return status;

    status = bsonExtractIntegerField(metadataObj, kConfigVersionFieldName, &_configVersion);
    if (!status.isOK())
        return status;

    status = bsonExtractIntegerField(metadataObj, kPrimaryIdFieldName, &_primaryId);
    if (!status.isOK())
        return status;

    status = bsonExtractIntegerField(metadataObj, kTermFieldName, &_term);
    if (!status.isOK())
        return status;

    // extracting the lastOpCommitted is a bit of a process
    Timestamp ts;
    status = bsonExtractTimestampField(metadataObj, kLastOpCommittedTimestampFieldName, &ts);
    if (!status.isOK())
        return status;
    long long term;
    status = bsonExtractIntegerField(metadataObj, kLastOpCommittedTermFieldName, &term);
    if (!status.isOK())
        return status;
    _lastOpCommitted = OpTime(ts, term);

    return Status::OK();
}

}  // namespace repl
}  // namespace mongo
