/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/rpc/metadata/repl_set_metadata.h"

#include "mongo/bson/util/bson_check.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/rpc/metadata.h"

namespace mongo {
namespace rpc {

using repl::OpTime;

const char kReplSetMetadataFieldName[] = "$replData";

namespace {

const char kLastOpCommittedFieldName[] = "lastOpCommitted";
const char kLastOpVisibleFieldName[] = "lastOpVisible";
const char kConfigVersionFieldName[] = "configVersion";
const char kReplicaSetIdFieldName[] = "replicaSetId";
const char kPrimaryIndexFieldName[] = "primaryIndex";
const char kSyncSourceIndexFieldName[] = "syncSourceIndex";
const char kTermFieldName[] = "term";

}  // unnamed namespace

const int ReplSetMetadata::kNoPrimary;

ReplSetMetadata::ReplSetMetadata(long long term,
                                 OpTime committedOpTime,
                                 OpTime visibleOpTime,
                                 long long configVersion,
                                 OID id,
                                 int currentPrimaryIndex,
                                 int currentSyncSourceIndex)
    : _lastOpCommitted(std::move(committedOpTime)),
      _lastOpVisible(std::move(visibleOpTime)),
      _currentTerm(term),
      _configVersion(configVersion),
      _replicaSetId(id),
      _currentPrimaryIndex(currentPrimaryIndex),
      _currentSyncSourceIndex(currentSyncSourceIndex) {}

StatusWith<ReplSetMetadata> ReplSetMetadata::readFromMetadata(const BSONObj& metadataObj) {
    BSONElement replMetadataElement;

    Status status = bsonExtractTypedField(
        metadataObj, rpc::kReplSetMetadataFieldName, Object, &replMetadataElement);
    if (!status.isOK())
        return status;
    BSONObj replMetadataObj = replMetadataElement.Obj();

    long long configVersion;
    status = bsonExtractIntegerField(replMetadataObj, kConfigVersionFieldName, &configVersion);
    if (!status.isOK())
        return status;

    OID id;
    status = bsonExtractOIDFieldWithDefault(replMetadataObj, kReplicaSetIdFieldName, OID(), &id);
    if (!status.isOK())
        return status;

    long long primaryIndex;
    status = bsonExtractIntegerField(replMetadataObj, kPrimaryIndexFieldName, &primaryIndex);
    if (!status.isOK())
        return status;

    long long syncSourceIndex;
    status = bsonExtractIntegerField(replMetadataObj, kSyncSourceIndexFieldName, &syncSourceIndex);
    if (!status.isOK())
        return status;

    long long term;
    status = bsonExtractIntegerField(replMetadataObj, kTermFieldName, &term);
    if (!status.isOK())
        return status;

    repl::OpTime lastOpCommitted;
    status = bsonExtractOpTimeField(replMetadataObj, kLastOpCommittedFieldName, &lastOpCommitted);
    if (!status.isOK())
        return status;

    repl::OpTime lastOpVisible;
    status = bsonExtractOpTimeField(replMetadataObj, kLastOpVisibleFieldName, &lastOpVisible);
    if (!status.isOK())
        return status;

    return ReplSetMetadata(
        term, lastOpCommitted, lastOpVisible, configVersion, id, primaryIndex, syncSourceIndex);
}

Status ReplSetMetadata::writeToMetadata(BSONObjBuilder* builder) const {
    BSONObjBuilder replMetadataBuilder(builder->subobjStart(kReplSetMetadataFieldName));
    replMetadataBuilder.append(kTermFieldName, _currentTerm);
    _lastOpCommitted.append(&replMetadataBuilder, kLastOpCommittedFieldName);
    _lastOpVisible.append(&replMetadataBuilder, kLastOpVisibleFieldName);
    replMetadataBuilder.append(kConfigVersionFieldName, _configVersion);
    replMetadataBuilder.append(kReplicaSetIdFieldName, _replicaSetId);
    replMetadataBuilder.append(kPrimaryIndexFieldName, _currentPrimaryIndex);
    replMetadataBuilder.append(kSyncSourceIndexFieldName, _currentSyncSourceIndex);
    replMetadataBuilder.doneFast();

    return Status::OK();
}

}  // namespace rpc
}  // namespace mongo
