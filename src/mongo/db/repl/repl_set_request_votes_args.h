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

#pragma once

#include <string>

#include "mongo/db/repl/optime.h"

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
    OpTime getLastDurableOpTime() const;
    bool isADryRun() const;

    void addToBSON(BSONObjBuilder* builder) const;

private:
    std::string _setName;  // Name of the replset.
    long long _term = -1;  // Current known term of the command issuer.
    // replSet config index of the member who sent the replSetRequestVotesCmd.
    long long _candidateIndex = -1;
    long long _cfgver = -1;     // replSet config version known to the command issuer.
    OpTime _lastDurableOpTime;  // The last known durable op of the command issuer.
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

private:
    long long _term = -1;
    bool _voteGranted = false;
    std::string _reason;
};

}  // namespace repl
}  // namespace mongo
