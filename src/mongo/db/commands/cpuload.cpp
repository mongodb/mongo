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

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"

namespace mongo {

using std::string;
using std::stringstream;

// Testing-only, enabled via command line.
class CPULoadCommand : public BasicCommand {
public:
    CPULoadCommand() : BasicCommand("cpuload") {}
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }
    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }
    std::string help() const override {
        return "internal. for testing only."
               "{ cpuload : 1, cpuFactor : 1 } Runs a straight CPU load. Length of execution "
               "scaled by cpuFactor. Puts no additional load on the server beyond the cpu use."
               "Useful for testing the stability of the performance of the underlying system,"
               "by running the command repeatedly and observing the variation in execution time.";
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {}  // No auth required
    virtual bool run(OperationContext* txn,
                     const string& badns,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        double cpuFactor = 1;
        if (cmdObj["cpuFactor"].isNumber()) {
            cpuFactor = cmdObj["cpuFactor"].number();
        }
        long long limit = 10000 * cpuFactor;
        // volatile used to ensure that loop is not optimized away
        volatile uint64_t lresult [[maybe_unused]] = 0;  // NOLINT
        uint64_t x = 100;
        for (long long i = 0; i < limit; i++) {
            x *= 13;
        }
        lresult = x;
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const {
        return false;
    }
};

MONGO_REGISTER_TEST_COMMAND(CPULoadCommand);


}  // namespace mongo
