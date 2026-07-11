// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
