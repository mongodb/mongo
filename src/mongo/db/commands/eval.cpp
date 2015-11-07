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
#include "mongo/scripting/engine.h"
#include "mongo/util/log.h"

namespace mongo {

using std::unique_ptr;
using std::dec;
using std::endl;
using std::string;
using std::stringstream;

namespace {

const int edebug = 0;

bool dbEval(OperationContext* txn,
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

    if (!globalScriptEngine) {
        errmsg = "db side execution is disabled";
        return false;
    }

    unique_ptr<Scope> s(globalScriptEngine->newScope());
    s->registerOperation(txn);

    ScriptingFunction f = s->createFunction(code);
    if (f == 0) {
        errmsg = string("compile failed: ") + s->getError();
        return false;
    }

    s->localConnectForDbEval(txn, dbName.c_str());

    if (e.type() == CodeWScope) {
        s->init(e.codeWScopeScopeDataUnsafe());
    }

    BSONObj args;
    {
        BSONElement argsElement = cmd.getField("args");
        if (argsElement.type() == Array) {
            args = argsElement.embeddedObject();
            if (edebug) {
                log() << "args:" << args.toString() << endl;
                log() << "code:\n" << code << endl;
            }
        }
    }

    int res;
    {
        Timer t;
        res = s->invoke(f, &args, 0, 0);
        int m = t.millis();
        if (m > serverGlobalParams.slowMS) {
            log() << "dbeval slow, time: " << dec << m << "ms " << dbName << endl;
            if (m >= 1000)
                log() << code << endl;
            else
                OCCASIONALLY log() << code << endl;
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


class CmdEval : public Command {
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

    CmdEval() : Command("eval", false, "$eval") {}

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) {
        if (cmdObj["nolock"].trueValue()) {
            return dbEval(txn, dbname, cmdObj, result, errmsg);
        }

        ScopedTransaction transaction(txn, MODE_X);
        Lock::GlobalWrite lk(txn->lockState());

        OldClientContext ctx(txn, dbname, false /* no shard version checking */);

        return dbEval(txn, dbname, cmdObj, result, errmsg);
    }

} cmdeval;

}  // namespace
}  // namespace mongo
