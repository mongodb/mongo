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

#include "mongo/db/repl/repl_set_request_votes_args.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/repl/bson_extract_optime.h"

namespace mongo {
namespace repl {
namespace {

const std::string kCandidateIndexFieldName = "candidateIndex";
const std::string kCommandName = "replSetRequestVotes";
const std::string kConfigVersionFieldName = "configVersion";
const std::string kConfigTermFieldName = "configTerm";
const std::string kDryRunFieldName = "dryRun";
const std::string kLastWrittenOpTimeFieldName = "lastWrittenOpTime";
const std::string kLastAppliedOpTimeFieldName = "lastAppliedOpTime";
const std::string kOkFieldName = "ok";
const std::string kReasonFieldName = "reason";
const std::string kSetNameFieldName = "setName";
const std::string kTermFieldName = "term";
const std::string kVoteGrantedFieldName = "voteGranted";
const std::string kOperationTime = "operationTime";
}  // namespace


Status ReplSetRequestVotesArgs::initialize(const BSONObj& argsObj) {
    Status status = bsonExtractIntegerField(argsObj, kTermFieldName, &_term);
    if (!status.isOK())
        return status;

    status = bsonExtractIntegerField(argsObj, kCandidateIndexFieldName, &_candidateIndex);
    if (!status.isOK())
        return status;

    status = bsonExtractIntegerField(argsObj, kConfigVersionFieldName, &_cfgVer);
    if (!status.isOK())
        return status;

    status = bsonExtractIntegerFieldWithDefault(
        argsObj, kConfigTermFieldName, OpTime::kUninitializedTerm, &_cfgTerm);
    if (!status.isOK())
        return status;

    status = bsonExtractStringField(argsObj, kSetNameFieldName, &_setName);
    if (!status.isOK())
        return status;

    status = bsonExtractBooleanField(argsObj, kDryRunFieldName, &_dryRun);
    if (!status.isOK())
        return status;

    status = bsonExtractOpTimeField(argsObj, kLastAppliedOpTimeFieldName, &_lastAppliedOpTime);
    if (!status.isOK()) {
        return status;
    }

    status = bsonExtractOpTimeField(argsObj, kLastWrittenOpTimeFieldName, &_lastWrittenOpTime);
    if (status.code() == ErrorCodes::NoSuchKey) {
        _lastWrittenOpTime = _lastAppliedOpTime;
        status = Status::OK();
    } else if (!status.isOK()) {
        return status;
    }

    return Status::OK();
}

const std::string& ReplSetRequestVotesArgs::getSetName() const {
    return _setName;
}

long long ReplSetRequestVotesArgs::getTerm() const {
    return _term;
}

long long ReplSetRequestVotesArgs::getCandidateIndex() const {
    return _candidateIndex;
}

long long ReplSetRequestVotesArgs::getConfigVersion() const {
    return _cfgVer;
}

long long ReplSetRequestVotesArgs::getConfigTerm() const {
    return _cfgTerm;
}

ConfigVersionAndTerm ReplSetRequestVotesArgs::getConfigVersionAndTerm() const {
    return ConfigVersionAndTerm(_cfgVer, _cfgTerm);
}

OpTime ReplSetRequestVotesArgs::getLastWrittenOpTime() const {
    return _lastWrittenOpTime;
}

OpTime ReplSetRequestVotesArgs::getLastAppliedOpTime() const {
    return _lastAppliedOpTime;
}

bool ReplSetRequestVotesArgs::isADryRun() const {
    return _dryRun;
}

void ReplSetRequestVotesArgs::addToBSON(BSONObjBuilder* builder) const {
    builder->append(kCommandName, 1);
    builder->append(kSetNameFieldName, _setName);
    builder->append(kDryRunFieldName, _dryRun);
    builder->append(kTermFieldName, _term);
    builder->appendNumber(kCandidateIndexFieldName, _candidateIndex);
    builder->appendNumber(kConfigVersionFieldName, _cfgVer);
    builder->appendNumber(kConfigTermFieldName, _cfgTerm);
    _lastWrittenOpTime.append(kLastWrittenOpTimeFieldName, builder);
    _lastAppliedOpTime.append(kLastAppliedOpTimeFieldName, builder);
}

std::string ReplSetRequestVotesArgs::toString() const {
    BSONObjBuilder builder;
    addToBSON(&builder);
    return builder.done().toString();
}

Status ReplSetRequestVotesResponse::initialize(const BSONObj& argsObj) {
    auto status = bsonExtractIntegerField(argsObj, kTermFieldName, &_term);
    if (!status.isOK())
        return status;

    status = bsonExtractBooleanField(argsObj, kVoteGrantedFieldName, &_voteGranted);
    if (!status.isOK())
        return status;

    status = bsonExtractStringField(argsObj, kReasonFieldName, &_reason);
    if (!status.isOK())
        return status;

    return Status::OK();
}

void ReplSetRequestVotesResponse::setVoteGranted(bool voteGranted) {
    _voteGranted = voteGranted;
}

void ReplSetRequestVotesResponse::setTerm(long long term) {
    _term = term;
}

void ReplSetRequestVotesResponse::setReason(const std::string& reason) {
    _reason = reason;
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
    builder->append(kTermFieldName, _term);
    builder->append(kVoteGrantedFieldName, _voteGranted);
    builder->append(kReasonFieldName, _reason);
}

BSONObj ReplSetRequestVotesResponse::toBSON() const {
    BSONObjBuilder builder;
    addToBSON(&builder);
    return builder.obj();
}

std::string ReplSetRequestVotesResponse::toString() const {
    BSONObjBuilder builder;
    addToBSON(&builder);
    return builder.done().toString();
}

}  // namespace repl
}  // namespace mongo
