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

#include "mongo/db/jsobj.h"

namespace mongo {
namespace repl {

    void ReplSetRequestVotesArgs::initialize(const BSONObj& argsObj) {
            _setName = argsObj["setName"].String();
            _term = argsObj["term"].Long();
            _candidateId = argsObj["candidateId"].Int();
            _cfgver = argsObj["cfgver"].Long();
            BSONObj lastCommittedOp = argsObj["lastCommittedOp"].Obj();
            _lastCommittedOp = OpTime(lastCommittedOp["optime"].timestamp(),
                                      lastCommittedOp["term"].Long());
    }

    const std::string& ReplSetRequestVotesArgs::getSetName() const {
        return _setName;
    }

    long long ReplSetRequestVotesArgs::getTerm() const {
        return _term;
    }

    int ReplSetRequestVotesArgs::getCandidateId() const {
        return _candidateId;
    }

    int ReplSetRequestVotesArgs::getConfigVersion() const {
        return _cfgver;
    }

    OpTime ReplSetRequestVotesArgs::getLastCommittedOp() const {
        return _lastCommittedOp;
    }

    void ReplSetRequestVotesArgs::addToBSON(BSONObjBuilder* builder) const {
        builder->append("replSetRequestVotes", 1);
        builder->append("setName", _setName);
        builder->append("term", _term);
        builder->appendIntOrLL("candidateId", _candidateId);
        builder->appendIntOrLL("cfgver", _cfgver);
        BSONObjBuilder lastCommittedOp(builder->subobjStart("lastCommittedOp"));
        lastCommittedOp.append("optime", _lastCommittedOp.getTimestamp());
        lastCommittedOp.append("term", _lastCommittedOp.getTerm());
        lastCommittedOp.done();
    }

    void ReplSetRequestVotesResponse::initialize(const BSONObj& argsObj) {
        _ok = argsObj["ok"].Bool();
        _term = argsObj["term"].Long();
        _voteGranted = argsObj["voteGranted"].Bool();
        _reason = argsObj["reason"].String();
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
        builder->append("ok", _ok);
        builder->append("term", _term);
        builder->append("voteGranted", _voteGranted);
        builder->append("reason", _reason);
    }

} // namespace repl
} // namespace mongo
