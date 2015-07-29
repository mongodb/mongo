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

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/metadata.h"

namespace mongo {
namespace rpc {

using repl::OpTime;

namespace {

const char kTermField[] = "term";
const char kLastCommittedTSField[] = "lastOpCommittedTimestamp";
const char kLastCommittedTermField[] = "lastOpCommittedTerm";
const char kLastCommittedConfigVersionField[] = "configVersion";
const char kLastCommittedPrimaryIndexField[] = "primaryIndex";

}  // unnamed namespace

ReplSetMetadata::ReplSetMetadata() = default;

ReplSetMetadata::ReplSetMetadata(long long term,
                                 OpTime committedOpTime,
                                 long long configVersion,
                                 int currentPrimaryIndex)
    : _currentTerm(term),
      _committedOpTime(std::move(committedOpTime)),
      _configVersion(configVersion),
      _currentPrimaryIndex(currentPrimaryIndex) {}

StatusWith<ReplSetMetadata> ReplSetMetadata::readFromMetadata(const BSONObj& doc) {
    long long term = 0;
    auto termStatus = bsonExtractIntegerField(doc, kTermField, &term);

    if (!termStatus.isOK()) {
        return termStatus;
    }

    Timestamp timestamp;
    auto timestampStatus = bsonExtractTimestampField(doc, kLastCommittedTSField, &timestamp);

    if (!timestampStatus.isOK()) {
        return timestampStatus;
    }

    long long termNumber = 0;
    auto commtedTermStatus = bsonExtractIntegerField(doc, kLastCommittedTermField, &termNumber);

    if (!commtedTermStatus.isOK()) {
        return commtedTermStatus;
    }

    long long configVersion = 0;
    auto configVersionStatus =
        bsonExtractIntegerField(doc, kLastCommittedConfigVersionField, &configVersion);

    if (!configVersionStatus.isOK()) {
        return configVersionStatus;
    }

    long long primaryIndex = 0;
    auto primaryIndexStatus =
        bsonExtractIntegerField(doc, kLastCommittedPrimaryIndexField, &primaryIndex);

    if (!primaryIndexStatus.isOK()) {
        return primaryIndexStatus;
    }

    return ReplSetMetadata(term, OpTime(timestamp, termNumber), configVersion, primaryIndex);
}

Status ReplSetMetadata::writeToMetadata(BSONObjBuilder* builder) const {
    builder->append(kTermField, _currentTerm);
    builder->append(kLastCommittedTSField, _committedOpTime.getTimestamp());
    builder->append(kLastCommittedTermField, _committedOpTime.getTerm());
    builder->append(kLastCommittedConfigVersionField, _configVersion);
    builder->append(kLastCommittedPrimaryIndexField, _currentPrimaryIndex);

    return Status::OK();
}

const OpTime& ReplSetMetadata::getLastCommittedOptime() const {
    return _committedOpTime;
}

}  // namespace rpc
}  // namespace mongo
