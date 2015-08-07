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

#include "mongo/db/repl/repl_set_request_votes_args.h"

#include "mongo/bson/util/bson_check.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"

namespace mongo {
namespace repl {
namespace {

const std::string kCandidateIdFieldName = "candidateId";
const std::string kCommandName = "replSetRequestVotes";
const std::string kConfigVersionFieldName = "configVersion";
const std::string kDryRunFieldName = "dryRun";
const std::string kLastCommittedOpFieldName = "lastCommittedOp";
const std::string kOkFieldName = "ok";
const std::string kOpTimeFieldName = "ts";
const std::string kReasonFieldName = "reason";
const std::string kSetNameFieldName = "setName";
const std::string kTermFieldName = "term";
const std::string kVoteGrantedFieldName = "voteGranted";

const std::string kLegalArgsFieldNames[] = {
    kCandidateIdFieldName,
    kCommandName,
    kConfigVersionFieldName,
    kDryRunFieldName,
    kLastCommittedOpFieldName,
    kOpTimeFieldName,
    kSetNameFieldName,
    kTermFieldName,
};

const std::string kLegalResponseFieldNames[] = {
    kOkFieldName, kReasonFieldName, kTermFieldName, kVoteGrantedFieldName,
};

}  // namespace


Status ReplSetRequestVotesArgs::initialize(const BSONObj& argsObj) {
    Status status = bsonCheckOnlyHasFields("ReplSetRequestVotes", argsObj, kLegalArgsFieldNames);
    if (!status.isOK())
        return status;

    status = bsonExtractIntegerField(argsObj, kTermFieldName, &_term);
    if (!status.isOK())
        return status;

    status = bsonExtractIntegerField(argsObj, kCandidateIdFieldName, &_candidateId);
    if (!status.isOK())
        return status;

    status = bsonExtractIntegerField(argsObj, kConfigVersionFieldName, &_cfgver);
    if (!status.isOK())
        return status;

    status = bsonExtractStringField(argsObj, kSetNameFieldName, &_setName);
    if (!status.isOK())
        return status;

    status = bsonExtractBooleanField(argsObj, kDryRunFieldName, &_dryRun);
    if (!status.isOK())
        return status;

    status = bsonExtractOpTimeField(argsObj, kLastCommittedOpFieldName, &_lastCommittedOp);
    if (!status.isOK())
        return status;

    return Status::OK();
}

const std::string& ReplSetRequestVotesArgs::getSetName() const {
    return _setName;
}

long long ReplSetRequestVotesArgs::getTerm() const {
    return _term;
}

long long ReplSetRequestVotesArgs::getCandidateId() const {
    return _candidateId;
}

long long ReplSetRequestVotesArgs::getConfigVersion() const {
    return _cfgver;
}

OpTime ReplSetRequestVotesArgs::getLastCommittedOp() const {
    return _lastCommittedOp;
}

bool ReplSetRequestVotesArgs::isADryRun() const {
    return _dryRun;
}

void ReplSetRequestVotesArgs::addToBSON(BSONObjBuilder* builder) const {
    builder->append(kCommandName, 1);
    builder->append(kSetNameFieldName, _setName);
    builder->append(kDryRunFieldName, _dryRun);
    builder->append(kTermFieldName, _term);
    builder->appendIntOrLL(kCandidateIdFieldName, _candidateId);
    builder->appendIntOrLL(kConfigVersionFieldName, _cfgver);
    BSONObjBuilder lastCommittedOp(builder->subobjStart(kLastCommittedOpFieldName));
    lastCommittedOp.append(kOpTimeFieldName, _lastCommittedOp.getTimestamp());
    lastCommittedOp.append(kTermFieldName, _lastCommittedOp.getTerm());
    lastCommittedOp.done();
}

Status ReplSetRequestVotesResponse::initialize(const BSONObj& argsObj) {
    Status status =
        bsonCheckOnlyHasFields("ReplSetRequestVotes", argsObj, kLegalResponseFieldNames);
    if (!status.isOK())
        return status;

    status = bsonExtractIntegerField(argsObj, kTermFieldName, &_term);
    if (!status.isOK())
        return status;

    status = bsonExtractBooleanField(argsObj, kVoteGrantedFieldName, &_voteGranted);
    if (!status.isOK())
        return status;

    status = bsonExtractStringField(argsObj, kReasonFieldName, &_reason);
    if (!status.isOK())
        return status;

    status = bsonExtractBooleanField(argsObj, kOkFieldName, &_ok);
    if (!status.isOK())
        return status;

    return Status::OK();
}

bool ReplSetRequestVotesResponse::getOk() const {
    return _ok;
}

long long ReplSetRequestVotesResponse::getTerm() const {
    return _term;
}

bool ReplSetRequestVotesResponse::getVoteGranted() const {
    return _voteGranted;
}

const std::string& ReplSetRequestVotesResponse::getReason() const {
    return _reason;
}

void ReplSetRequestVotesResponse::addToBSON(BSONObjBuilder* builder) const {
    builder->append(kOkFieldName, _ok);
    builder->append(kTermFieldName, _term);
    builder->append(kVoteGrantedFieldName, _voteGranted);
    builder->append(kReasonFieldName, _reason);
}

BSONObj ReplSetRequestVotesResponse::toBSON() const {
    BSONObjBuilder builder;
    addToBSON(&builder);
    return builder.obj();
}

}  // namespace repl
}  // namespace mongo
