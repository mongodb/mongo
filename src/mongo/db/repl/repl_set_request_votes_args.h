// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {

class BSONObj;

namespace repl {

class ReplSetRequestVotesArgs {
public:
    Status initialize(const BSONObj& argsObj);

    const std::string& getSetName() const;
    long long getTerm() const;
    long long getCandidateIndex() const;
    long long getConfigVersion() const;
    long long getConfigTerm() const;
    ConfigVersionAndTerm getConfigVersionAndTerm() const;
    OpTime getLastWrittenOpTime() const;
    OpTime getLastAppliedOpTime() const;
    bool isADryRun() const;

    void addToBSON(BSONObjBuilder* builder) const;
    std::string toString() const;

private:
    std::string _setName;  // Name of the replset.
    long long _term = -1;  // Current known term of the command issuer.
    // replSet config index of the member who sent the replSetRequestVotesCmd.
    long long _candidateIndex = -1;
    long long _cfgVer = -1;  // replSet config version known to the command issuer.
    // replSet config term known to the command issuer.
    long long _cfgTerm = OpTime::kUninitializedTerm;
    OpTime _lastWrittenOpTime;  // The OpTime of the last known written op of the command issuer.
    OpTime _lastAppliedOpTime;  // The OpTime of the last known applied op of the command issuer.
    bool _dryRun = false;       // Indicates this is a pre-election check when true.
};

class ReplSetRequestVotesResponse {
public:
    Status initialize(const BSONObj& argsObj);

    void setVoteGranted(bool voteGranted);
    void setTerm(long long term);
    void setReason(const std::string& reason);

    long long getTerm() const;
    bool getVoteGranted() const;
    const std::string& getReason() const;

    void addToBSON(BSONObjBuilder* builder) const;
    BSONObj toBSON() const;
    std::string toString() const;

private:
    long long _term = -1;
    bool _voteGranted = false;
    std::string _reason;
};

}  // namespace repl
}  // namespace mongo
