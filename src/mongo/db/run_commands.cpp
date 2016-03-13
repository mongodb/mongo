/*    Copyright 2016 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/run_commands.h"

#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/rpc/request_interface.h"
#include "mongo/util/log.h"

namespace mongo {

void runCommands(OperationContext* txn,
                 const rpc::RequestInterface& request,
                 rpc::ReplyBuilderInterface* replyBuilder) {
    try {
        dassert(replyBuilder->getState() == rpc::ReplyBuilderInterface::State::kCommandReply);

        Command* c = nullptr;
        // In the absence of a Command object, no redaction is possible. Therefore
        // to avoid displaying potentially sensitive information in the logs,
        // we restrict the log message to the name of the unrecognized command.
        // However, the complete command object will still be echoed to the client.
        if (!(c = Command::findCommand(request.getCommandName()))) {
            Command::unknownCommands.increment();
            std::string msg = str::stream() << "no such command: '" << request.getCommandName()
                                            << "'";
            LOG(2) << msg;
            uasserted(ErrorCodes::CommandNotFound,
                      str::stream() << msg << ", bad cmd: '" << request.getCommandArgs() << "'");
        }

        LOG(2) << "run command " << request.getDatabase() << ".$cmd" << ' '
               << c->getRedactedCopyForLogging(request.getCommandArgs());

        {
            // Try to set this as early as possible, as soon as we have figured out the command.
            stdx::lock_guard<Client> lk(*txn->getClient());
            CurOp::get(txn)->setLogicalOp_inlock(c->getLogicalOp());
        }

        Command::execCommand(txn, c, request, replyBuilder);
    }

    catch (const DBException& ex) {
        Command::generateErrorResponse(txn, replyBuilder, ex, request);
    }
}

}  // namespace mongo
