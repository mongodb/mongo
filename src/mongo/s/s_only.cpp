/**
 *    Copyright (C) 2009-2016 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <tuple>

#include "mongo/base/init.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/rpc/request_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

void mongo::execCommandClient(OperationContext* txn,
                              Command* c,
                              int queryOptions,
                              const char* ns,
                              BSONObj& cmdObj,
                              BSONObjBuilder& result) {
    std::string dbname = nsToDatabase(ns);

    if (cmdObj.getBoolField("help")) {
        std::stringstream help;
        help << "help for: " << c->getName() << " ";
        c->help(help);
        result.append("help", help.str());
        Command::appendCommandStatus(result, true, "");
        return;
    }

    Status status = Command::checkAuthorization(c, txn, dbname, cmdObj);
    if (!status.isOK()) {
        Command::appendCommandStatus(result, status);
        return;
    }

    c->_commandsExecuted.increment();

    if (c->shouldAffectCommandCounter()) {
        globalOpCounters.gotCommand();
    }

    StatusWith<WriteConcernOptions> wcResult =
        WriteConcernOptions::extractWCFromCommand(cmdObj, dbname);
    if (!wcResult.isOK()) {
        Command::appendCommandStatus(result, wcResult.getStatus());
        return;
    }

    bool supportsWriteConcern = c->supportsWriteConcern(cmdObj);
    if (!supportsWriteConcern && !wcResult.getValue().usedDefault) {
        // This command doesn't do writes so it should not be passed a writeConcern.
        // If we did not use the default writeConcern, one was provided when it shouldn't have
        // been by the user.
        Command::appendCommandStatus(
            result, Status(ErrorCodes::InvalidOptions, "Command does not support writeConcern"));
        return;
    }


    // attach tracking
    rpc::TrackingMetadata trackingMetadata;
    trackingMetadata.initWithOperName(c->getName());
    rpc::TrackingMetadata::get(txn) = trackingMetadata;

    std::string errmsg;
    bool ok = false;
    try {
        if (!supportsWriteConcern) {
            ok = c->run(txn, dbname, cmdObj, queryOptions, errmsg, result);
        } else {
            // Change the write concern while running the command.
            const auto oldWC = txn->getWriteConcern();
            ON_BLOCK_EXIT([&] { txn->setWriteConcern(oldWC); });
            txn->setWriteConcern(wcResult.getValue());

            ok = c->run(txn, dbname, cmdObj, queryOptions, errmsg, result);
        }
    } catch (const DBException& e) {
        result.resetToEmpty();
        const int code = e.getCode();

        // Codes for StaleConfigException
        if (code == ErrorCodes::RecvStaleConfig || code == ErrorCodes::SendStaleConfig) {
            throw;
        }

        errmsg = e.what();
        result.append("code", code);
    }

    if (!ok) {
        c->_commandsFailed.increment();
    }

    Command::appendCommandStatus(result, ok, errmsg);
}

namespace mongo {

using std::string;
using std::stringstream;

namespace {
// called into by the web server. For now we just translate the parameters
// to their old style equivalents.
void execCommandHandler(OperationContext* txn,
                        Command* command,
                        const rpc::RequestInterface& request,
                        rpc::ReplyBuilderInterface* replyBuilder) {
    int queryFlags = 0;
    BSONObj cmdObj;

    std::tie(cmdObj, queryFlags) = uassertStatusOK(
        rpc::downconvertRequestMetadata(request.getCommandArgs(), request.getMetadata()));

    std::string db = request.getDatabase().rawData();
    BSONObjBuilder result;

    execCommandClient(txn, command, queryFlags, request.getDatabase().rawData(), cmdObj, result);

    replyBuilder->setCommandReply(result.done()).setMetadata(rpc::makeEmptyMetadata());
}
MONGO_INITIALIZER(InitializeCommandExecCommandHandler)(InitializerContext* const) {
    Command::registerExecCommand(execCommandHandler);
    return Status::OK();
}
}  // namespace

namespace {
void registerErrorImpl(OperationContext* txn, const DBException& exception) {}

MONGO_INITIALIZER(InitializeRegisterErrorHandler)(InitializerContext* const) {
    Command::registerRegisterError(registerErrorImpl);
    return Status::OK();
}
}  // namespace

}  // namespace mongo
