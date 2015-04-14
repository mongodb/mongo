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
        /**
         * Initializes from the parsed contents of argsObj.
         */
        void initialize(const BSONObj& argsObj);

        const std::string& getSetName() const;
        long long getTerm() const;
        int getCandidateId() const;
        int getConfigVersion() const;
        OpTime getLastCommittedOp() const;

        void addToBSON(BSONObjBuilder* builder) const;

    private:
        std::string _setName; // Name of the replset
        long long _term; // Current known term of the command issuer
        int _candidateId; // replSet id of the member that sent the replSetRequestVotes command
        int _cfgver; // replSet config version known to the command issuer
        OpTime _lastCommittedOp; // The last known committed op of the command issuer 
    };

    class ReplSetRequestVotesResponse {
    public:
        void initialize(const BSONObj& argsObj);
        
        bool getOk() const;
        long long getTerm() const;
        bool getVoteGranted() const;
        const std::string& getReason() const;

        void addToBSON(BSONObjBuilder* builder) const;

    private:
        bool _ok;
        long long _term;
        bool _voteGranted;
        std::string _reason;
    };

} // namespace repl
} // namespace mongo
