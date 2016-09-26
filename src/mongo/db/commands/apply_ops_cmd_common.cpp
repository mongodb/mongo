/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/db/commands/apply_ops_cmd_common.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/bson_check.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

namespace {

Status checkOperationAuthorization(OperationContext* txn,
                                   const std::string& dbname,
                                   const BSONObj& oplogEntry,
                                   bool alwaysUpsert) {
    AuthorizationSession* authSession = AuthorizationSession::get(txn->getClient());

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

        return Command::checkAuthorization(command, txn, dbname, o);
    }

    if (opType == "i"_sd) {
        return authSession->checkAuthForInsert(txn, ns, o);
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

        return authSession->checkAuthForUpdate(txn, ns, o, o2, upsert);
    } else if (opType == "d"_sd) {

        return authSession->checkAuthForDelete(txn, ns, o);
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

}  // namespace


Status checkAuthForApplyOpsCommand(OperationContext* txn,
                                   const std::string& dbname,
                                   const BSONObj& cmdObj) {
    AuthorizationSession* authSession = AuthorizationSession::get(txn->getClient());


    std::vector<Privilege> universalPrivileges;
    RoleGraph::generateUniversalPrivileges(&universalPrivileges);
    if (!authSession->isAuthorizedForPrivileges(universalPrivileges)) {
        return Status(ErrorCodes::Unauthorized, "Unauthorized");
    }


    boost::optional<DisableDocumentValidation> maybeDisableValidation;
    if (shouldBypassDocumentValidationForCommand(cmdObj))
        maybeDisableValidation.emplace(txn);


    const bool alwaysUpsert =
        cmdObj.hasField("alwaysUpsert") ? cmdObj["alwaysUpsert"].trueValue() : true;

    checkBSONType(BSONType::Array, cmdObj.firstElement());
    for (const BSONElement& e : cmdObj.firstElement().Array()) {
        checkBSONType(BSONType::Object, e);
        Status status = checkOperationAuthorization(txn, dbname, e.Obj(), alwaysUpsert);
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
}  // namespace mongo
