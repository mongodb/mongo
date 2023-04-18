/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/auth/authorization_checks.h"

#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"

namespace mongo {
namespace auth {
namespace {

// Checks if this connection has the privileges necessary to create or modify the view 'viewNs'
// to be a view on 'viewOnNs' with pipeline 'viewPipeline'. Call this function after verifying
// that the user has the 'createCollection' or 'collMod' action, respectively.
Status checkAuthForCreateOrModifyView(OperationContext* opCtx,
                                      AuthorizationSession* authzSession,
                                      const NamespaceString& viewNs,
                                      const NamespaceString& viewOnNs,
                                      const BSONArray& viewPipeline,
                                      bool isMongos) {
    // It's safe to allow a user to create or modify a view if they can't read it anyway.
    if (!authzSession->isAuthorizedForActionsOnNamespace(viewNs, ActionType::find)) {
        return Status::OK();
    }

    auto request = aggregation_request_helper::parseFromBSON(
        opCtx,
        viewNs,
        BSON("aggregate" << viewOnNs.coll() << "pipeline" << viewPipeline << "cursor" << BSONObj()
                         << "$db" << viewOnNs.db()),
        boost::none,
        false);

    auto statusWithPrivs = getPrivilegesForAggregate(authzSession, viewOnNs, request, isMongos);
    PrivilegeVector privileges = uassertStatusOK(statusWithPrivs);
    if (!authzSession->isAuthorizedForPrivileges(privileges)) {
        return Status(ErrorCodes::Unauthorized, "unauthorized");
    }
    return Status::OK();
}

}  // namespace

Status checkAuthForFind(AuthorizationSession* authSession,
                        const NamespaceString& ns,
                        bool hasTerm) {
    if (MONGO_unlikely(ns.isCommand())) {
        return Status(ErrorCodes::InternalError,
                      str::stream() << "Checking query auth on command namespace "
                                    << ns.toStringForErrorMsg());
    }
    if (!authSession->isAuthorizedForActionsOnNamespace(ns, ActionType::find)) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "not authorized for query on " << ns.toStringForErrorMsg());
    }

    // Only internal clients (such as other nodes in a replica set) are allowed to use
    // the 'term' field in a find operation. Use of this field could trigger changes
    // in the receiving server's replication state and should be protected.
    if (hasTerm &&
        !authSession->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                       ActionType::internal)) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream()
                          << "not authorized for query with term on " << ns.toStringForErrorMsg());
    }

    return Status::OK();
}

Status checkAuthForGetMore(AuthorizationSession* authSession,
                           const NamespaceString& ns,
                           long long cursorID,
                           bool hasTerm) {
    // Since users can only getMore their own cursors, we verify that a user either is authenticated
    // or does not need to be.
    if (!authSession->shouldIgnoreAuthChecks() && !authSession->isAuthenticated()) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream()
                          << "not authorized for getMore on " << ns.dbName().toStringForErrorMsg());
    }

    // Only internal clients (such as other nodes in a replica set) are allowed to use
    // the 'term' field in a getMore operation. Use of this field could trigger changes
    // in the receiving server's replication state and should be protected.
    if (hasTerm &&
        !authSession->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                       ActionType::internal)) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "not authorized for getMore with term on "
                                    << ns.toStringForErrorMsg());
    }

    return Status::OK();
}

Status checkAuthForInsert(AuthorizationSession* authSession,
                          OperationContext* opCtx,
                          const NamespaceString& ns) {
    ActionSet required{ActionType::insert};
    if (DocumentValidationSettings::get(opCtx).isSchemaValidationDisabled()) {
        required.addAction(ActionType::bypassDocumentValidation);
    }
    if (!authSession->isAuthorizedForActionsOnNamespace(ns, required)) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "not authorized for insert on " << ns.toStringForErrorMsg());
    }

    return Status::OK();
}

Status checkAuthForUpdate(AuthorizationSession* authSession,
                          OperationContext* opCtx,
                          const NamespaceString& ns,
                          const BSONObj& query,
                          const write_ops::UpdateModification& update,
                          bool upsert) {
    ActionSet required{ActionType::update};
    StringData operationType = "update"_sd;

    if (upsert) {
        required.addAction(ActionType::insert);
        operationType = "upsert"_sd;
    }

    if (DocumentValidationSettings::get(opCtx).isSchemaValidationDisabled()) {
        required.addAction(ActionType::bypassDocumentValidation);
    }

    if (!authSession->isAuthorizedForActionsOnNamespace(ns, required)) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "not authorized for " << operationType << " on "
                                    << ns.toStringForErrorMsg());
    }

    return Status::OK();
}

Status checkAuthForDelete(AuthorizationSession* authSession,
                          OperationContext* opCtx,
                          const NamespaceString& ns,
                          const BSONObj& query) {
    if (!authSession->isAuthorizedForActionsOnNamespace(ns, ActionType::remove)) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream()
                          << "not authorized to remove from " << ns.toStringForErrorMsg());
    }
    return Status::OK();
}

Status checkAuthForKillCursors(AuthorizationSession* authSession,
                               const NamespaceString& ns,
                               const boost::optional<UserName>& cursorOwner) {
    if (authSession->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                      ActionType::killAnyCursor)) {
        return Status::OK();
    }

    if (authSession->isCoauthorizedWith(cursorOwner)) {
        return Status::OK();
    }

    ResourcePattern target;
    if (ns.isListCollectionsCursorNS()) {
        target = ResourcePattern::forDatabaseName(ns.db());
    } else {
        target = ResourcePattern::forExactNamespace(ns);
    }

    if (authSession->isAuthorizedForActionsOnResource(target, ActionType::killAnyCursor)) {
        return Status::OK();
    }

    return Status(ErrorCodes::Unauthorized,
                  str::stream() << "not authorized to kill cursor on " << ns.toStringForErrorMsg());
}

Status checkAuthForCreate(OperationContext* opCtx,
                          AuthorizationSession* authSession,
                          const CreateCommand& cmd,
                          bool isMongos) {
    auto ns = cmd.getNamespace();

    if (cmd.getCapped() &&
        !authSession->isAuthorizedForActionsOnNamespace(ns, ActionType::convertToCapped)) {
        return Status(ErrorCodes::Unauthorized, "unauthorized");
    }

    const bool hasCreateCollectionAction =
        authSession->isAuthorizedForActionsOnNamespace(ns, ActionType::createCollection);

    // If attempting to create a view, check for additional required privileges.
    if (auto optViewOn = cmd.getViewOn()) {

        // You need the createCollection action on this namespace; the insert action is not
        // sufficient.
        if (!hasCreateCollectionAction) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        // Parse the viewOn namespace and the pipeline. If no pipeline was specified, use the empty
        // pipeline.
        NamespaceString viewOnNs(
            NamespaceStringUtil::parseNamespaceFromRequest(ns.dbName(), optViewOn.value()));
        auto pipeline = cmd.getPipeline().get_value_or(std::vector<BSONObj>());
        BSONArrayBuilder pipelineArray;
        for (const auto& stage : pipeline) {
            pipelineArray.append(stage);
        }
        return checkAuthForCreateOrModifyView(
            opCtx, authSession, ns, viewOnNs, pipelineArray.arr(), isMongos);
    }

    // To create a regular collection, ActionType::createCollection or ActionType::insert are
    // both acceptable.
    if (hasCreateCollectionAction ||
        authSession->isAuthorizedForActionsOnNamespace(ns, ActionType::insert)) {
        return Status::OK();
    }

    return Status(ErrorCodes::Unauthorized, "unauthorized");
}

Status checkAuthForCollMod(OperationContext* opCtx,
                           AuthorizationSession* authSession,
                           const NamespaceString& ns,
                           const BSONObj& cmdObj,
                           bool isMongos) {
    if (!authSession->isAuthorizedForActionsOnNamespace(ns, ActionType::collMod)) {
        return Status(ErrorCodes::Unauthorized, "unauthorized");
    }

    // Check for additional required privileges if attempting to modify a view. When auth is
    // enabled, users must specify both "viewOn" and "pipeline" together. This prevents a user from
    // exposing more information in the original underlying namespace by only changing "pipeline",
    // or looking up more information via the original pipeline by only changing "viewOn".
    const bool hasViewOn = cmdObj.hasField("viewOn");
    const bool hasPipeline = cmdObj.hasField("pipeline");
    if (hasViewOn != hasPipeline) {
        return Status(
            ErrorCodes::InvalidOptions,
            "Must specify both 'viewOn' and 'pipeline' when modifying a view and auth is enabled");
    }
    if (hasViewOn) {
        NamespaceString viewOnNs(NamespaceStringUtil::parseNamespaceFromRequest(
            ns.dbName(), cmdObj["viewOn"].checkAndGetStringData()));
        auto viewPipeline = BSONArray(cmdObj["pipeline"].Obj());
        return checkAuthForCreateOrModifyView(
            opCtx, authSession, ns, viewOnNs, viewPipeline, isMongos);
    }

    return Status::OK();
}

StatusWith<PrivilegeVector> getPrivilegesForAggregate(AuthorizationSession* authSession,
                                                      const NamespaceString& nss,
                                                      const AggregateCommandRequest& request,
                                                      bool isMongos) {
    if (!nss.isValid()) {
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "Invalid input namespace, " << nss.toStringForErrorMsg());
    }

    PrivilegeVector privileges;

    // If this connection does not need to be authenticated (for instance, if auth is disabled),
    // returns an empty requirements set.
    if (authSession->shouldIgnoreAuthChecks()) {
        return privileges;
    }

    const auto& pipeline = request.getPipeline();

    // If the aggregation pipeline is empty, confirm the user is authorized for find on 'nss'.
    if (pipeline.empty()) {
        Privilege currentPriv =
            Privilege(ResourcePattern::forExactNamespace(nss), ActionType::find);
        Privilege::addPrivilegeToPrivilegeVector(&privileges, currentPriv);
        return privileges;
    }

    // If the first stage of the pipeline is not an initial source, the pipeline is implicitly
    // reading documents from the underlying collection. The client must be authorized to do so.
    auto liteParsedDocSource = LiteParsedDocumentSource::parse(nss, pipeline[0]);
    if (!liteParsedDocSource->isInitialSource()) {
        Privilege currentPriv =
            Privilege(ResourcePattern::forExactNamespace(nss), ActionType::find);
        Privilege::addPrivilegeToPrivilegeVector(&privileges, currentPriv);
    }

    // Confirm privileges for the pipeline.
    for (auto&& pipelineStage : pipeline) {
        liteParsedDocSource = LiteParsedDocumentSource::parse(nss, pipelineStage);
        PrivilegeVector currentPrivs = liteParsedDocSource->requiredPrivileges(
            isMongos, request.getBypassDocumentValidation().value_or(false));
        Privilege::addPrivilegesToPrivilegeVector(&privileges, currentPrivs);
    }
    return privileges;
}

}  // namespace auth
}  // namespace mongo
