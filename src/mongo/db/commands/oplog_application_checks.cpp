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
#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_check.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/builtin_roles.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/validated_tenancy_scope_factory.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/oplog_application_checks.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/tenant_id.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

namespace mongo {
UUID OplogApplicationChecks::getUUIDFromOplogEntry(const BSONObj& oplogEntry) {
    BSONElement uiElem = oplogEntry["ui"];
    return uassertStatusOK(UUID::parse(uiElem));
};

Status OplogApplicationChecks::checkOperationAuthorization(OperationContext* opCtx,
                                                           const DatabaseName& dbName,
                                                           const BSONObj& oplogEntry,
                                                           AuthorizationSession* authSession,
                                                           bool alwaysUpsert) {
    BSONElement opTypeElem = oplogEntry["op"];
    checkBSONType(BSONType::String, opTypeElem);
    const StringData opType = opTypeElem.checkAndGetStringData();

    if (opType == "n"_sd) {
        // oplog notes require cluster permissions, and may not have a ns
        if (!authSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(dbName.tenantId()),
                ActionType::appendOplogNote)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    BSONElement nsElem = oplogEntry["ns"];
    checkBSONType(BSONType::String, nsElem);
    boost::optional<TenantId> tid = oplogEntry.hasElement("tid")
        ? boost::make_optional<TenantId>(TenantId::parseFromBSON(oplogEntry["tid"]))
        : boost::none;
    NamespaceString nss = NamespaceStringUtil::deserialize(
        tid, oplogEntry["ns"].checkAndGetStringData(), SerializationContext::stateDefault());

    if (oplogEntry.hasField("ui"_sd)) {
        // ns by UUID overrides the ns specified if they are different.
        auto catalog = CollectionCatalog::get(opCtx);
        boost::optional<NamespaceString> uuidCollNS =
            catalog->lookupNSSByUUID(opCtx, getUUIDFromOplogEntry(oplogEntry));
        if (uuidCollNS && *uuidCollNS != nss)
            nss = *uuidCollNS;
    }

    BSONElement oElem = oplogEntry["o"];
    checkBSONType(BSONType::Object, oElem);
    BSONObj o = oElem.Obj();

    if (opType == "c"_sd) {
        StringData commandName = o.firstElement().fieldNameStringData();
        Command* commandInOplogEntry = CommandHelpers::findCommand(opCtx, commandName);
        if (!commandInOplogEntry) {
            return Status(ErrorCodes::FailedToParse, "Unrecognized command in op");
        }

        auto dbNameForAuthCheck = nss.dbName();
        if (commandName == "renameCollection") {
            // renameCollection commands must be run on the 'admin' database. Its arguments are
            // fully qualified namespaces. Catalog internals don't know the op produced by running
            // renameCollection was originally run on 'admin', so we must restore this.
            dbNameForAuthCheck = DatabaseNameUtil::deserialize(
                nss.tenantId(), "admin", SerializationContext::stateDefault());
        }

        // TODO reuse the parse result for when we run() later. Note that when running,
        // we must use a potentially different dbname.
        return [&] {
            try {
                boost::optional<auth::ValidatedTenancyScope> vts;
                if (dbNameForAuthCheck.tenantId()) {
                    vts = boost::optional<auth::ValidatedTenancyScope>(
                        auth::ValidatedTenancyScopeFactory::create(
                            dbNameForAuthCheck.tenantId().value(),
                            auth::ValidatedTenancyScopeFactory::TrustedForInnerOpMsgRequestTag{}));
                }

                auto request = OpMsgRequestBuilder::create(vts, dbNameForAuthCheck, o);
                commandInOplogEntry->parse(opCtx, request)->checkAuthorization(opCtx, request);
                return Status::OK();
            } catch (const DBException& e) {
                return e.toStatus();
            }
        }();
    }

    if (opType == "i"_sd) {
        return auth::checkAuthForInsert(authSession, opCtx, nss);
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

        return auth::checkAuthForUpdate(authSession,
                                        opCtx,
                                        nss,
                                        o2,
                                        write_ops::UpdateModification::parseFromOplogEntry(
                                            o, write_ops::UpdateModification::DiffOptions{}),
                                        upsert);
    } else if (opType == "d"_sd) {

        return auth::checkAuthForDelete(authSession, opCtx, nss, o);
    } else if (opType == "db"_sd) {
        // It seems that 'db' isn't used anymore. Require all actions to prevent casual use.
        ActionSet allActions;
        allActions.addAllActions();
        if (!authSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forAnyResource(nss.tenantId()), allActions)) {
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
    StringData opType = opElement.valueStringDataSafe();
    if (opType.empty()) {
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
    if (opType != "n"_sd && nsElement.String().empty()) {
        return {ErrorCodes::IllegalOperation,
                str::stream() << "\"ns\" field value cannot be empty when op type is not 'n': "
                              << e.fieldName()};
    }
    return Status::OK();
}

Status OplogApplicationChecks::checkAuthForOperation(OperationContext* opCtx,
                                                     const DatabaseName& dbName,
                                                     const BSONObj& cmdObj,
                                                     OplogApplicationValidity validity) {
    AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());
    if (!authSession->isAuthorizedForActionsOnResource(
            ResourcePattern::forClusterResource(dbName.tenantId()), ActionType::applyOps)) {
        return Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    if (validity == OplogApplicationValidity::kNeedsSuperuser) {
        std::vector<Privilege> universalPrivileges;
        auth::generateUniversalPrivileges(&universalPrivileges, dbName.tenantId());
        if (!authSession->isAuthorizedForPrivileges(universalPrivileges)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }
    if (validity == OplogApplicationValidity::kNeedsForceAndUseUUID) {
        if (!authSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(dbName.tenantId()),
                {ActionType::forceUUID, ActionType::useUUID})) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        validity = OplogApplicationValidity::kOk;
    }
    if (validity == OplogApplicationValidity::kNeedsUseUUID) {
        if (!authSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(dbName.tenantId()), ActionType::useUUID)) {
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
            opCtx, dbName, e.Obj(), authSession, alwaysUpsert);
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

}  // namespace mongo
