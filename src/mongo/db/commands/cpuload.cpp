// cpuload.cpp

/**
*    Copyright (C) 2017 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/db/commands.h"

namespace mongo {

using std::string;
using std::stringstream;

// Testing-only, enabled via command line.
class CPULoadCommand : public Command {
public:
    CPULoadCommand() : Command("cpuload") {}
    virtual bool slaveOk() const {
        return true;
    }
    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }
    virtual void help(stringstream& help) const {
        help << "internal. for testing only.";
        help << "{ cpuload : 1, cpuFactor : 1 } Runs a straight CPU load. Length of execution ";
        help << "scaled by cpuFactor. Puts no additional load on the server beyond the cpu use.";
        help << "Useful for testing the stability of the performance of the underlying system,";
        help << "by running the command repeatedly and observing the variation in execution time.";
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}  // No auth required
    virtual bool run(OperationContext* txn,
                     const string& badns,
                     const BSONObj& cmdObj,
                     string& errmsg,
                     BSONObjBuilder& result) {
        double cpuFactor = 1;
        if (cmdObj["cpuFactor"].isNumber()) {
            cpuFactor = cmdObj["cpuFactor"].number();
        }
        long long limit = 10000 * cpuFactor;
        // volatile used to ensure that loop is not optimized away
        volatile uint64_t lresult = 0;  // NOLINT
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

MONGO_INITIALIZER(RegisterCpuLoadCmd)(InitializerContext* context) {
    if (Command::testCommandsEnabled) {
        new CPULoadCommand();
    }
    return Status::OK();
}


}  // namespace mongo
