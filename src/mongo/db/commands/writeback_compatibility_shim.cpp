/**
 *    Copyright (C) 2014 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/base/init.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"

namespace mongo {

using std::string;
using std::stringstream;

using mongoutils::str::stream;

/**
 * This command is required in v3.0 mongod to prevent v2.6 mongos from entering a tight loop and
 * spamming the server with invalid writebacklisten requests.  This command reports an error
 * and pauses, which is safe because the original v2.6 WBL command was a long-poll (30s).
 */
class WriteBackCommand : public Command {
public:
    WriteBackCommand() : Command("writebacklisten") {}

    void help(stringstream& helpOut) const {
        helpOut << "v3.0 disallowed internal command, present for compatibility only";
    }


    //
    // Same as v2.6 settings
    //

    virtual bool adminOnly() const {
        return true;
    }
    virtual bool slaveOk() const {
        return true;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    virtual bool run(
        OperationContext* opCtx, const string&, BSONObj&, int, string&, BSONObjBuilder& result) {
        string errMsg = stream() << "Writeback functionality is no longer present in v3.0 mongod, "
                                 << "a v2.6 mongos may be running in the v3.0 cluster at "
                                 << opCtx->getClient()->clientAddress(false);

        error() << errMsg;

        // Prevent v2.6 mongos from spamming writebacklisten retries
        const int kSleepSecsBeforeMessage = 5;
        sleepsecs(kSleepSecsBeforeMessage);

        return appendCommandStatus(result, Status(ErrorCodes::CommandNotFound, errMsg));
    }
};

/**
 * The "writeBacksQueued" field is required in ServerStatus output to avoid v2.6 mongos crashing
 * confusingly when upgrading a cluster.
 */
class WriteBacksQueuedSSM : public ServerStatusMetric {
public:
    WriteBacksQueuedSSM() : ServerStatusMetric(".writeBacksQueued") {}

    virtual void appendAtLeaf(BSONObjBuilder& b) const {
        // always append false, we don't queue writebacks
        b.appendBool(_leafName, false);
    }
};

namespace {
MONGO_INITIALIZER(RegisterWriteBackShim)(InitializerContext* context) {
    // Leaked intentionally: a Command registers itself when constructed.
    new WriteBackCommand();
    // Leaked intentionally: a SSM registers itself when constructed.
    new WriteBacksQueuedSSM();
    return Status::OK();
}
}

}  // namespace
