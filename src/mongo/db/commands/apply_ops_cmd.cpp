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

#include <vector>

#include "mongo/bson/util/bson_check.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/service_context.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

Status checkOperationAuthorization(OperationContext* opCtx,
                                   const std::string& dbname,
                                   const BSONObj& oplogEntry,
                                   bool alwaysUpsert) {
    AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());

    BSONElement opTypeElem = oplogEntry["op"];
    checkBSONType(BSONType::String, opTypeElem);
    const StringData opType = opTypeElem.checkAndGetStringData();

    if (opType == "n"_sd) {
        // oplog notes require cluster permissions, and may not have a ns
        if (!authSession->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::appendOplogNote)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    BSONElement nsElem = oplogEntry["ns"];
    checkBSONType(BSONType::String, nsElem);
    NamespaceString ns(oplogEntry["ns"].checkAndGetStringData());

    BSONElement oElem = oplogEntry["o"];
    checkBSONType(BSONType::Object, oElem);
    BSONObj o = oElem.Obj();

    if (opType == "c"_sd) {
        Command* command = Command::findCommand(o.firstElement().fieldNameStringData());
        if (!command) {
            return Status(ErrorCodes::FailedToParse, "Unrecognized command in op");
        }

        return Command::checkAuthorization(command, opCtx, OpMsgRequest::fromDBAndBody(dbname, o));
    }

    if (opType == "i"_sd) {
        return authSession->checkAuthForInsert(opCtx, ns, o);
    } else if (opType == "u"_sd) {
        BSONElement o2Elem = oplogEntry["o2"];
        checkBSONType(BSONType::Object, o2Elem);
        BSONObj o2 = o2Elem.Obj();

        BSONElement bElem = oplogEntry["b"];
        if (!bElem.eoo()) {
            checkBSONType(BSONType::Bool, bElem);
        }
        bool b = bElem.trueValue();

        const bool upsert = b || alwaysUpsert;

        return authSession->checkAuthForUpdate(opCtx, ns, o, o2, upsert);
    } else if (opType == "d"_sd) {

        return authSession->checkAuthForDelete(opCtx, ns, o);
    } else if (opType == "db"_sd) {
        // It seems that 'db' isn't used anymore. Require all actions to prevent casual use.
        ActionSet allActions;
        allActions.addAllActions();
        if (!authSession->isAuthorizedForActionsOnResource(ResourcePattern::forAnyResource(),
                                                           allActions)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    return Status(ErrorCodes::FailedToParse, "Unrecognized opType");
}

enum class ApplyOpsValidity { kOk, kNeedsSuperuser };

/**
 * Returns either kNeedsSuperuser, if the provided applyOps command contains an empty applyOps
 * command, or kOk if no other handlable conditions detected. May throw exceptions if the input
 * is malformed.
 */
ApplyOpsValidity validateApplyOpsCommand(const BSONObj& cmdObj) {
    const size_t maxApplyOpsDepth = 10;
    std::stack<std::pair<size_t, BSONObj>> toCheck;

    auto operationContainsApplyOps = [](const BSONObj& opObj) {
        BSONElement opTypeElem = opObj["op"];
        checkBSONType(BSONType::String, opTypeElem);
        const StringData opType = opTypeElem.checkAndGetStringData();

        if (opType == "c"_sd) {
            BSONElement oElem = opObj["o"];
            checkBSONType(BSONType::Object, oElem);
            BSONObj o = oElem.Obj();

            if (o.firstElement().fieldNameStringData() == "applyOps"_sd) {
                return true;
            }
        }
        return false;
    };

    // Insert the top level applyOps command into the stack.
    toCheck.emplace(std::make_pair(0, cmdObj));

    while (!toCheck.empty()) {
        std::pair<size_t, BSONObj> item = toCheck.top();
        toCheck.pop();

        checkBSONType(BSONType::Array, item.second.firstElement());
        // Check if the applyOps command is empty. This is probably not something that should
        // happen, so require a superuser to do this.
        if (item.second.firstElement().Array().empty()) {
            return ApplyOpsValidity::kNeedsSuperuser;
        }

        // For each applyOps command, iterate the ops.
        for (BSONElement element : item.second.firstElement().Array()) {
            checkBSONType(BSONType::Object, element);
            BSONObj elementObj = element.Obj();

            // If the op itself contains an applyOps...
            if (operationContainsApplyOps(elementObj)) {
                // And we've recursed too far, then bail out.
                uassert(ErrorCodes::FailedToParse,
                        "Too many nested applyOps",
                        item.first < maxApplyOpsDepth);

                // Otherwise, if the op contains an applyOps, but we haven't recursed too far:
                // extract the applyOps command, and insert it into the stack.
                checkBSONType(BSONType::Object, elementObj["o"]);
                BSONObj oObj = elementObj["o"].Obj();
                toCheck.emplace(std::make_pair(item.first + 1, std::move(oObj)));
            }
        }
    }

    return ApplyOpsValidity::kOk;
}

class ApplyOpsCmd : public ErrmsgCommandDeprecated {
public:
    ApplyOpsCmd() : ErrmsgCommandDeprecated("applyOps") {}

    bool slaveOk() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    void help(std::stringstream& help) const override {
        help << "internal (sharding)\n{ applyOps : [ ] , preCondition : [ { ns : ... , q : ... , "
                "res : ... } ] }";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) override {
        AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());

        ApplyOpsValidity validity = validateApplyOpsCommand(cmdObj);
        if (validity == ApplyOpsValidity::kNeedsSuperuser) {
            std::vector<Privilege> universalPrivileges;
            RoleGraph::generateUniversalPrivileges(&universalPrivileges);
            if (!authSession->isAuthorizedForPrivileges(universalPrivileges)) {
                return Status(ErrorCodes::Unauthorized, "Unauthorized");
            }
            return Status::OK();
        }
        fassert(40314, validity == ApplyOpsValidity::kOk);

        boost::optional<DisableDocumentValidation> maybeDisableValidation;
        if (shouldBypassDocumentValidationForCommand(cmdObj))
            maybeDisableValidation.emplace(opCtx);

        const bool alwaysUpsert =
            cmdObj.hasField("alwaysUpsert") ? cmdObj["alwaysUpsert"].trueValue() : true;

        checkBSONType(BSONType::Array, cmdObj.firstElement());
        for (const BSONElement& e : cmdObj.firstElement().Array()) {
            checkBSONType(BSONType::Object, e);
            Status status = checkOperationAuthorization(opCtx, dbname, e.Obj(), alwaysUpsert);
            if (!status.isOK()) {
                return status;
            }
        }

        BSONElement preconditions = cmdObj["preCondition"];
        if (!preconditions.eoo()) {
            for (const BSONElement& precondition : preconditions.Array()) {
                checkBSONType(BSONType::Object, precondition);
                BSONElement nsElem = precondition.Obj()["ns"];
                checkBSONType(BSONType::String, nsElem);
                NamespaceString nss(nsElem.checkAndGetStringData());

                if (!authSession->isAuthorizedForActionsOnResource(
                        ResourcePattern::forExactNamespace(nss), ActionType::find)) {
                    return Status(ErrorCodes::Unauthorized, "Unauthorized to check precondition");
                }
            }
        }

        return Status::OK();
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        validateApplyOpsCommand(cmdObj);

        boost::optional<DisableDocumentValidation> maybeDisableValidation;
        if (shouldBypassDocumentValidationForCommand(cmdObj))
            maybeDisableValidation.emplace(opCtx);

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
        auto applyOpsStatus = appendCommandStatus(result, applyOps(opCtx, dbname, cmdObj, &result));

        return applyOpsStatus;
    }

private:
    /**
     * Returns true if 'e' contains a valid operation.
     */
    static bool _checkOperation(const BSONElement& e, std::string& errmsg) {
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
