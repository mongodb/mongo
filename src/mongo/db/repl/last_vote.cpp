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

#include "mongo/db/repl/last_vote.h"

#include "mongo/bson/util/bson_check.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"

namespace mongo {
namespace repl {
namespace {

constexpr StringData kCandidateIndexFieldName = "candidateIndex"_sd;
constexpr StringData kTermFieldName = "term"_sd;
constexpr StringData kIdFieldName = "_id"_sd;

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
