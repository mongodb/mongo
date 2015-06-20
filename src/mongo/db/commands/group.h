/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/commands.h"

namespace mongo {

class ClientBasic;
class Database;
class OperationContext;
class PlanExecutor;
class Scope;

struct GroupRequest;

class GroupCommand : public Command {
public:
    GroupCommand();

private:
    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual bool maintenanceOk() const {
        return false;
    }

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool slaveOverrideOk() const {
        return true;
    }

    virtual void help(std::stringstream& help) const {
        help << "http://dochub.mongodb.org/core/aggregation";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj);

    virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const;

    virtual bool run(OperationContext* txn,
                     const std::string& dbname,
                     BSONObj& jsobj,
                     int,
                     std::string& errmsg,
                     BSONObjBuilder& result);

    virtual Status explain(OperationContext* txn,
                           const std::string& dbname,
                           const BSONObj& cmdObj,
                           ExplainCommon::Verbosity verbosity,
                           BSONObjBuilder* out) const;

    /**
     * Parse a group command object.
     *
     * If 'cmdObj' is well-formed, returns Status::OK() and fills in out-argument 'request'.
     *
     * If a parsing error is encountered, returns an error Status.
     */
    Status parseRequest(const std::string& dbname,
                        const BSONObj& cmdObj,
                        GroupRequest* request) const;
};

}  // namespace mongo
