// s_only.cpp

/*    Copyright 2009 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <tuple>

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/rpc/request_interface.h"
#include "mongo/s/cluster_last_error_info.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;
using std::stringstream;


bool isMongos() {
    return true;
}

/** When this callback is run, we record a shard that we've used for useful work
 *  in an operation to be read later by getLastError()
*/
void usingAShardConnection(const std::string& addr) {
    ClusterLastErrorInfo::get(cc()).addShardHost(addr);
}

// called into by the web server. For now we just translate the parameters
// to their old style equivalents.
void Command::execCommand(OperationContext* txn,
                          Command* command,
                          const rpc::RequestInterface& request,
                          rpc::ReplyBuilderInterface* replyBuilder) {
    int queryFlags = 0;
    BSONObj cmdObj;

    std::tie(cmdObj, queryFlags) = uassertStatusOK(
        rpc::downconvertRequestMetadata(request.getCommandArgs(), request.getMetadata()));

    std::string db = request.getDatabase().rawData();
    BSONObjBuilder result;

    execCommandClientBasic(txn,
                           command,
                           *txn->getClient(),
                           queryFlags,
                           request.getDatabase().rawData(),
                           cmdObj,
                           result);

    replyBuilder->setCommandReply(result.done()).setMetadata(rpc::makeEmptyMetadata());
}

void Command::execCommandClientBasic(OperationContext* txn,
                                     Command* c,
                                     ClientBasic& client,
                                     int queryOptions,
                                     const char* ns,
                                     BSONObj& cmdObj,
                                     BSONObjBuilder& result) {
    std::string dbname = nsToDatabase(ns);

    if (cmdObj.getBoolField("help")) {
        stringstream help;
        help << "help for: " << c->name << " ";
        c->help(help);
        result.append("help", help.str());
        appendCommandStatus(result, true, "");
        return;
    }

    Status status = _checkAuthorization(c, &client, dbname, cmdObj);
    if (!status.isOK()) {
        appendCommandStatus(result, status);
        return;
    }

    c->_commandsExecuted.increment();

    if (c->shouldAffectCommandCounter()) {
        globalOpCounters.gotCommand();
    }

    std::string errmsg;
    bool ok = false;
    try {
        ok = c->run(txn, dbname, cmdObj, queryOptions, errmsg, result);
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

    appendCommandStatus(result, ok, errmsg);
}

void Command::runAgainstRegistered(OperationContext* txn,
                                   const char* ns,
                                   BSONObj& jsobj,
                                   BSONObjBuilder& anObjBuilder,
                                   int queryOptions) {
    // It should be impossible for this uassert to fail since there should be no way to get
    // into this function with any other collection name.
    uassert(16618,
            "Illegal attempt to run a command against a namespace other than $cmd.",
            nsToCollectionSubstring(ns) == "$cmd");

    BSONElement e = jsobj.firstElement();
    std::string commandName = e.fieldName();
    Command* c = e.type() ? Command::findCommand(commandName) : NULL;
    if (!c) {
        Command::appendCommandStatus(
            anObjBuilder, false, str::stream() << "no such cmd: " << commandName);
        anObjBuilder.append("code", ErrorCodes::CommandNotFound);
        Command::unknownCommands.increment();
        return;
    }

    execCommandClientBasic(txn, c, cc(), queryOptions, ns, jsobj, anObjBuilder);
}

void Command::registerError(OperationContext* txn, const DBException& exception) {}

}  // namespace mongo
