// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/last_vote.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"

#include <string_view>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace repl {
namespace {
using namespace std::literals::string_view_literals;

constexpr std::string_view kCandidateIndexFieldName = "candidateIndex"sv;
constexpr std::string_view kTermFieldName = "term"sv;
constexpr std::string_view kIdFieldName = "_id"sv;

}  // namespace

LastVote::LastVote(long long term, long long candidateIndex)
    : _candidateIndex(candidateIndex), _term(term) {}

StatusWith<LastVote> LastVote::readFromLastVote(const BSONObj& doc) {
    long long term;
    Status status = bsonExtractIntegerField(doc, kTermFieldName, &term);
    if (!status.isOK())
        return status;

    long long candidateIndex;
    status = bsonExtractIntegerField(doc, kCandidateIndexFieldName, &candidateIndex);
    if (!status.isOK())
        return status;

    return LastVote{term, candidateIndex};
}

void LastVote::setTerm(long long term) {
    _term = term;
}

void LastVote::setCandidateIndex(long long candidateIndex) {
    _candidateIndex = candidateIndex;
}

long long LastVote::getTerm() const {
    return _term;
}

long long LastVote::getCandidateIndex() const {
    return _candidateIndex;
}


BSONObj LastVote::toBSON() const {
    BSONObjBuilder builder;
    builder.append(kTermFieldName, _term);
    builder.append(kCandidateIndexFieldName, _candidateIndex);
    return builder.obj();
}

}  // namespace repl
}  // namespace mongo
