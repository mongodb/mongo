/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"

namespace mongo {
namespace {

/**
 * getDiagnosticData is a MongoD only command. We implement in MongoS to give users a better error
 * message.
 */
class GetDiagnosticDataCommand final : public Command {
public:
    GetDiagnosticDataCommand() : Command("getDiagnosticData") {}

    bool adminOnly() const override {
        return true;
    }

    void help(std::stringstream& help) const override {
        help << "get latest diagnostic data collection snapshot";
    }

    bool slaveOk() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& db,
             BSONObj& cmdObj,
             std::string& errmsg,
             BSONObjBuilder& result) override {

        errmsg = "getDiagnosticData not allowed through mongos";

        return false;
    }
};

Command* ftdcCommand;

MONGO_INITIALIZER(CreateDiagnosticDataCommand)(InitializerContext* context) {
    ftdcCommand = new GetDiagnosticDataCommand();

    return Status::OK();
}

}  // namespace
}  // namespace mongo
