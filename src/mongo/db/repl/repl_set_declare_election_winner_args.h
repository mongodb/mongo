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

#include "mongo/base/status.h"

namespace mongo {

    class BSONObj;
    class BSONObjBuilder;

namespace repl {

    class ReplSetDeclareElectionWinnerArgs {
    public:
        Status initialize(const BSONObj& argsObj);

        long long getTerm() const;
        long long getWinnerId() const;
        long long getConfigVersion() const;

        void addToBSON(BSONObjBuilder* builder) const;

    private:
        long long _term = -1; // The term for which the winner is being declared.
        long long _winnerId = -1; // replSet id of the member who was the winner.
        long long _configVersion = -1; // replSet config version known to the command issuer.
    };

    class ReplSetDeclareElectionWinnerResponse {
    public:
        Status initialize(const BSONObj& argsObj);
        
        bool getOk() const;
        long long getTerm() const;
        long long getErrorCode() const;
        const std::string& getErrorMsg() const;

        void addToBSON(BSONObjBuilder* builder) const;

    private:
        bool _ok = false;
        long long _term = -1;
        long long _code = ErrorCodes::OK;
        std::string _errmsg;
    };

} // namespace repl
} // namespace mongo
