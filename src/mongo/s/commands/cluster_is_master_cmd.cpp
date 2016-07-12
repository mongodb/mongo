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

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/wire_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/map_util.h"

namespace mongo {
namespace {

class CmdIsMaster : public Command {
public:
    CmdIsMaster() : Command("isMaster", false, "ismaster") {}


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual bool slaveOk() const {
        return true;
    }

    virtual void help(std::stringstream& help) const {
        help << "test if this is master half of a replica pair";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        // No auth required
    }

    virtual bool run(OperationContext* txn,
                     const std::string& dbname,
                     BSONObj& cmdObj,
                     int options,
                     std::string& errmsg,
                     BSONObjBuilder& result) {
        result.appendBool("ismaster", true);
        result.append("msg", "isdbgrid");
        result.appendNumber("maxBsonObjectSize", BSONObjMaxUserSize);
        result.appendNumber("maxMessageSizeBytes", MaxMessageSizeBytes);
        result.appendNumber("maxWriteBatchSize", BatchedCommandRequest::kMaxWriteBatchSize);
        result.appendDate("localTime", jsTime());

        // Mongos tries to keep exactly the same version range of the server for which
        // it is compiled.
        result.append("maxWireVersion", WireSpec::instance().maxWireVersionIncoming);
        result.append("minWireVersion", WireSpec::instance().minWireVersionIncoming);

        const auto parameter = mapFindWithDefault(ServerParameterSet::getGlobal()->getMap(),
                                                  "automationServiceDescriptor");
        if (parameter)
            parameter->append(txn, result, "automationServiceDescriptor");

        return true;
    }

} isMaster;

}  // namespace
}  // namespace mongo
