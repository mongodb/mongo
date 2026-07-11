// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_extra_info.h"
#include "mongo/base/status_with.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PARENT_PRIVATE]] mongo {

class BSONObj;
class BSONObjBuilder;

namespace repl {

class LastVote {
public:
    LastVote(long long term, long long candidateIndex);

    static StatusWith<LastVote> readFromLastVote(const BSONObj& doc);

    long long getTerm() const;
    long long getCandidateIndex() const;

    void setTerm(long long term);
    void setCandidateIndex(long long candidateIndex);
    BSONObj toBSON() const;

private:
    long long _candidateIndex;
    long long _term;
};

}  // namespace repl
}  // namespace mongo
