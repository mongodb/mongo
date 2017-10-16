// dbeval.cpp

/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/introspect.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/s/stale_exception.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::unique_ptr;
using std::dec;
using std::endl;
using std::string;
using std::stringstream;

namespace {

const int edebug = 0;

bool dbEval(OperationContext* opCtx,
            const string& dbName,
            const BSONObj& cmd,
            BSONObjBuilder& result,
            string& errmsg) {
    RARELY {
        warning() << "the eval command is deprecated" << startupWarningsLog;
    }

    const BSONElement e = cmd.firstElement();
    uassert(
        10046, "eval needs Code", e.type() == Code || e.type() == CodeWScope || e.type() == String);

    const char* code = 0;
    switch (e.type()) {
        case String:
        case Code:
            code = e.valuestr();
            break;
        case CodeWScope:
            code = e.codeWScopeCode();
            break;
        default:
            verify(0);
    }

    verify(code);

    if (!getGlobalScriptEngine()) {
        errmsg = "db side execution is disabled";
        return false;
    }

    unique_ptr<Scope> s(getGlobalScriptEngine()->newScope());
    s->registerOperation(opCtx);

    ScriptingFunction f = s->createFunction(code);
    if (f == 0) {
        errmsg = string("compile failed: ") + s->getError();
        return false;
    }

    s->localConnectForDbEval(opCtx, dbName.c_str());

    if (e.type() == CodeWScope) {
        s->init(e.codeWScopeScopeDataUnsafe());
    }

    BSONObj args;
    {
        BSONElement argsElement = cmd.getField("args");
        if (argsElement.type() == Array) {
            args = argsElement.embeddedObject();
            if (edebug) {
                log() << "args:" << args;
                log() << "code:\n" << redact(code);
            }
        }
    }

    int res;
    {
        Timer t;
        res = s->invoke(f, &args, 0, 0);
        int m = t.millis();
        if (m > serverGlobalParams.slowMS) {
            log() << "dbeval slow, time: " << dec << m << "ms " << dbName;
            if (m >= 1000)
                log() << redact(code);
            else
                OCCASIONALLY log() << redact(code);
        }
    }

    if (res || s->isLastRetNativeCode()) {
        result.append("errno", (double)res);
        errmsg = "invoke failed: ";
        if (s->isLastRetNativeCode())
            errmsg += "cannot return native function";
        else
            errmsg += s->getError();

        return false;
    }

    s->append(result, "retval", "__returnValue");

    return true;
}


class CmdEval : public ErrmsgCommandDeprecated {
public:
    virtual bool slaveOk() const {
        return false;
    }

    virtual void help(stringstream& help) const {
        help << "DEPRECATED\n"
             << "Evaluate javascript at the server.\n"
             << "http://dochub.mongodb.org/core/serversidecodeexecution";
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        RoleGraph::generateUniversalPrivileges(out);
    }

    CmdEval() : ErrmsgCommandDeprecated("eval", "$eval") {}

    bool errmsgRun(OperationContext* opCtx,
                   const string& dbname,
                   const BSONObj& cmdObj,
                   string& errmsg,
                   BSONObjBuilder& result) {
        // Note: 'eval' is not allowed to touch sharded namespaces, but we can't check the
        // shardVersions of the namespaces accessed in the script until the script is evaluated.
        // Instead, we enforce that the script does not access sharded namespaces by ensuring the
        // shardVersion is set to UNSHARDED on the OperationContext before sending the script to be
        // evaluated.
        auto& oss = OperationShardingState::get(opCtx);
        uassert(ErrorCodes::IllegalOperation,
                "can't send a shardVersion with the 'eval' command, since you can't use sharded "
                "collections from 'eval'",
                !oss.hasShardVersion());

        // Set the shardVersion to UNSHARDED. The "namespace" used does not matter, because if a
        // shardVersion is set on the OperationContext, a check for a different namespace will
        // default to UNSHARDED.
        oss.setShardVersion(NamespaceString(dbname), ChunkVersion::UNSHARDED());
        const auto shardVersionGuard =
            MakeGuard([&]() { oss.unsetShardVersion(NamespaceString(dbname)); });

        try {
            if (cmdObj["nolock"].trueValue()) {
                return dbEval(opCtx, dbname, cmdObj, result, errmsg);
            }

            Lock::GlobalWrite lk(opCtx);

            OldClientContext ctx(opCtx, dbname, false /* no shard version checking here */);

            return dbEval(opCtx, dbname, cmdObj, result, errmsg);
        } catch (const AssertionException& ex) {
            // Convert a stale shardVersion error to a stronger error to prevent this node or the
            // sending node from believing it needs to refresh its routing table.
            if (ex.code() == ErrorCodes::StaleConfig) {
                uasserted(ErrorCodes::BadValue,
                          str::stream() << "can't use sharded collection from db.eval");
            }
            throw;
        }
    }

} cmdeval;

}  // namespace
}  // namespace mongo
