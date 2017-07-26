/**
 *    Copyright (C) 2008-2016 MongoDB Inc.
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

#include <string>
#include <vector>

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/apply_ops.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/apply_ops_cmd_common.h"
#include "mongo/db/commands/dbhash.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/service_context.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::string;
using std::stringstream;

namespace {

class ApplyOpsCmd : public Command {
public:
    ApplyOpsCmd() : Command("applyOps") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    virtual void help(stringstream& help) const {
        help << "internal (sharding)\n{ applyOps : [ ] , preCondition : [ { ns : ... , q : ... , "
                "res : ... } ] }";
    }


    virtual Status checkAuthForOperation(OperationContext* txn,
                                         const std::string& dbname,
                                         const BSONObj& cmdObj) {
        return checkAuthForApplyOpsCommand(txn, dbname, cmdObj);
    }

    virtual bool run(OperationContext* txn,
                     const string& dbname,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        validateApplyOpsCommand(cmdObj);

        boost::optional<DisableDocumentValidation> maybeDisableValidation;
        if (shouldBypassDocumentValidationForCommand(cmdObj))
            maybeDisableValidation.emplace(txn);

        if (cmdObj.firstElement().type() != Array) {
            errmsg = "ops has to be an array";
            return false;
        }

        BSONObj ops = cmdObj.firstElement().Obj();

        {
            // check input
            BSONObjIterator i(ops);
            while (i.more()) {
                BSONElement e = i.next();
                if (!_checkOperation(e, errmsg)) {
                    return false;
                }
            }
        }

        // TODO (SERVER-30217): When a write concern is provided to the applyOps command, we
        // normally wait on the OpTime of whichever operation successfully completed last. This is
        // erroneous, however, if the last operation in the array happens to be a write no-op and
        // thus isn’t assigned an OpTime. Let the second to last operation in the applyOps be write
        // A, the last operation in applyOps be write B. Let B do a no-op write and let the
        // operation that caused B to be a no-op be C. If C has an OpTime after A but before B,
        // then we won’t wait for C to be replicated and it could be rolled back, even though B
        // was acknowledged. To fix this, we should wait for replication of the node’s last applied
        // OpTime if the last write operation was a no-op write.
        auto applyOpsStatus = appendCommandStatus(result, applyOps(txn, dbname, cmdObj, &result));

        return applyOpsStatus;
    }

private:
    /**
     * Returns true if 'e' contains a valid operation.
     */
    bool _checkOperation(const BSONElement& e, string& errmsg) {
        if (e.type() != Object) {
            errmsg = str::stream() << "op not an object: " << e.fieldName();
            return false;
        }
        BSONObj obj = e.Obj();
        // op - operation type
        BSONElement opElement = obj.getField("op");
        if (opElement.eoo()) {
            errmsg = str::stream() << "op does not contain required \"op\" field: "
                                   << e.fieldName();
            return false;
        }
        if (opElement.type() != mongo::String) {
            errmsg = str::stream() << "\"op\" field is not a string: " << e.fieldName();
            return false;
        }
        // operation type -- see logOp() comments for types
        const char* opType = opElement.valuestrsafe();
        if (*opType == '\0') {
            errmsg = str::stream() << "\"op\" field value cannot be empty: " << e.fieldName();
            return false;
        }

        // ns - namespace
        // Only operations of type 'n' are allowed to have an empty namespace.
        BSONElement nsElement = obj.getField("ns");
        if (nsElement.eoo()) {
            errmsg = str::stream() << "op does not contain required \"ns\" field: "
                                   << e.fieldName();
            return false;
        }
        if (nsElement.type() != mongo::String) {
            errmsg = str::stream() << "\"ns\" field is not a string: " << e.fieldName();
            return false;
        }
        if (nsElement.String().find('\0') != std::string::npos) {
            errmsg = str::stream() << "namespaces cannot have embedded null characters";
            return false;
        }
        if (*opType != 'n' && nsElement.String().empty()) {
            errmsg = str::stream() << "\"ns\" field value cannot be empty when op type is not 'n': "
                                   << e.fieldName();
            return false;
        }
        return true;
    }

} applyOpsCmd;

}  // namespace
}  // namespace mongo
