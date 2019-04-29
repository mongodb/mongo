/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#include "mongo/platform/basic.h"

#include "mongo/bson/util/bson_check.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/oplog_application_checks.h"

namespace mongo {
UUID OplogApplicationChecks::getUUIDFromOplogEntry(const BSONObj& oplogEntry) {
    BSONElement uiElem = oplogEntry["ui"];
    return uassertStatusOK(UUID::parse(uiElem));
};

Status OplogApplicationChecks::checkOperationAuthorization(OperationContext* opCtx,
                                                           const std::string& dbname,
                                                           const BSONObj& oplogEntry,
                                                           AuthorizationSession* authSession,
                                                           bool alwaysUpsert) {
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

    if (oplogEntry.hasField("ui"_sd)) {
        // ns by UUID overrides the ns specified if they are different.
        auto& catalog = CollectionCatalog::get(opCtx);
        boost::optional<NamespaceString> uuidCollNS =
            catalog.lookupNSSByUUID(getUUIDFromOplogEntry(oplogEntry));
        if (uuidCollNS && *uuidCollNS != ns)
            ns = *uuidCollNS;
    }

    BSONElement oElem = oplogEntry["o"];
    checkBSONType(BSONType::Object, oElem);
    BSONObj o = oElem.Obj();

    if (opType == "c"_sd) {
        StringData commandName = o.firstElement().fieldNameStringData();
        Command* commandInOplogEntry = CommandHelpers::findCommand(commandName);
        if (!commandInOplogEntry) {
            return Status(ErrorCodes::FailedToParse, "Unrecognized command in op");
        }

        std::string dbNameForAuthCheck = ns.db().toString();
        if (commandName == "renameCollection") {
            // renameCollection commands must be run on the 'admin' database. Its arguments are
            // fully qualified namespaces. Catalog internals don't know the op produced by running
            // renameCollection was originally run on 'admin', so we must restore this.
            dbNameForAuthCheck = "admin";
        }

        // TODO reuse the parse result for when we run() later. Note that when running,
        // we must use a potentially different dbname.
        return [&] {
            try {
                auto request = OpMsgRequest::fromDBAndBody(dbNameForAuthCheck, o);
                commandInOplogEntry->parse(opCtx, request)->checkAuthorization(opCtx, request);
                return Status::OK();
            } catch (const DBException& e) {
                return e.toStatus();
            }
        }();
    }

    if (opType == "i"_sd) {
        return authSession->checkAuthForInsert(opCtx, ns);
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

Status OplogApplicationChecks::checkOperationArray(const BSONElement& opsElement) {
    if (opsElement.type() != Array) {
        return {ErrorCodes::FailedToParse, "ops has to be an array"};
    }
    const auto& ops = opsElement.Obj();
    // check input
    BSONObjIterator i(ops);
    while (i.more()) {
        BSONElement e = i.next();
        auto status = checkOperation(e);
        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

Status OplogApplicationChecks::checkOperation(const BSONElement& e) {
    if (e.type() != Object) {
        return {ErrorCodes::FailedToParse, str::stream() << "op not an object: " << e.fieldName()};
    }
    BSONObj obj = e.Obj();
    // op - operation type
    BSONElement opElement = obj.getField("op");
    if (opElement.eoo()) {
        return {ErrorCodes::IllegalOperation,
                str::stream() << "op does not contain required \"op\" field: " << e.fieldName()};
    }
    if (opElement.type() != mongo::String) {
        return {ErrorCodes::IllegalOperation,
                str::stream() << "\"op\" field is not a string: " << e.fieldName()};
    }
    // operation type -- see logOp() comments for types
    const char* opType = opElement.valuestrsafe();
    if (*opType == '\0') {
        return {ErrorCodes::IllegalOperation,
                str::stream() << "\"op\" field value cannot be empty: " << e.fieldName()};
    }

    // ns - namespace
    // Only operations of type 'n' are allowed to have an empty namespace.
    BSONElement nsElement = obj.getField("ns");
    if (nsElement.eoo()) {
        return {ErrorCodes::IllegalOperation,
                str::stream() << "op does not contain required \"ns\" field: " << e.fieldName()};
    }
    if (nsElement.type() != mongo::String) {
        return {ErrorCodes::IllegalOperation,
                str::stream() << "\"ns\" field is not a string: " << e.fieldName()};
    }
    if (nsElement.String().find('\0') != std::string::npos) {
        return {ErrorCodes::IllegalOperation,
                str::stream() << "namespaces cannot have embedded null characters"};
    }
    if (*opType != 'n' && nsElement.String().empty()) {
        return {ErrorCodes::IllegalOperation,
                str::stream() << "\"ns\" field value cannot be empty when op type is not 'n': "
                              << e.fieldName()};
    }
    return Status::OK();
}

Status OplogApplicationChecks::checkAuthForCommand(OperationContext* opCtx,
                                                   const std::string& dbname,
                                                   const BSONObj& cmdObj,
                                                   OplogApplicationValidity validity) {
    AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());
    if (validity == OplogApplicationValidity::kNeedsSuperuser) {
        std::vector<Privilege> universalPrivileges;
        RoleGraph::generateUniversalPrivileges(&universalPrivileges);
        if (!authSession->isAuthorizedForPrivileges(universalPrivileges)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }
    if (validity == OplogApplicationValidity::kNeedsForceAndUseUUID) {
        if (!authSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(),
                {ActionType::forceUUID, ActionType::useUUID})) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        validity = OplogApplicationValidity::kOk;
    }
    if (validity == OplogApplicationValidity::kNeedsUseUUID) {
        if (!authSession->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::useUUID)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        validity = OplogApplicationValidity::kOk;
    }
    fassert(40314, validity == OplogApplicationValidity::kOk);

    boost::optional<DisableDocumentValidation> maybeDisableValidation;
    if (shouldBypassDocumentValidationForCommand(cmdObj))
        maybeDisableValidation.emplace(opCtx);

    const bool alwaysUpsert =
        cmdObj.hasField("alwaysUpsert") ? cmdObj["alwaysUpsert"].trueValue() : true;

    checkBSONType(BSONType::Array, cmdObj.firstElement());
    for (const BSONElement& e : cmdObj.firstElement().Array()) {
        checkBSONType(BSONType::Object, e);
        Status status = OplogApplicationChecks::checkOperationAuthorization(
            opCtx, dbname, e.Obj(), authSession, alwaysUpsert);
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
