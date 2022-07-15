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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/validate_db_metadata_common.h"
#include "mongo/db/commands/validate_db_metadata_gen.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/multitenancy.h"
#include "mongo/db/views/view_catalog_helpers.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {
void overrideAPIParams(OperationContext* opCtx, const APIParamsForCmd& params) {
    APIParameters apiParameters;
    apiParameters.setAPIVersion(params.getVersion());
    apiParameters.setAPIStrict(params.getStrict());
    apiParameters.setAPIDeprecationErrors(params.getDeprecationErrors());
    APIParameters::get(opCtx) = std::move(apiParameters);
}

}  // namespace

/**
 * Example validate command:
 *   {
 *       validateDBMeta: 1,
 *       db: <string>,
 *       collection: <string>,
 *       apiParameters: {version: <string>, strict: <bool>, deprecationErrors: <bool>}
 *   }
 */
class ValidateDBMetadataCmd : public TypedCommand<ValidateDBMetadataCmd> {
    using _TypedCommandInvocationBase =
        typename TypedCommand<ValidateDBMetadataCmd>::InvocationBase;

public:
    using Request = ValidateDBMetadataCommandRequest;
    using Reply = ValidateDBMetadataCommandReply;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool maintenanceOk() const {
        return false;
    }

    std::string help() const override {
        return str::stream()
            << "validateDBMetadata checks that the stored metadata of a database/collection is "
               "valid within a particular API version. If 'db' parameter is specified, only runs "
               "validation against that database, if not the validation will be run againt all "
               "dbs. Similarly if 'collection' parameter is specified, the validation is only "
               "run against that collection, if not the validation is run against all collections.";
    }
    class Invocation : public _TypedCommandInvocationBase {
    public:
        using _TypedCommandInvocationBase::_TypedCommandInvocationBase;

        bool supportsWriteConcern() const final {
            return false;
        }
        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }
        void doCheckAuthorization(OperationContext* opCtx) const final {
            assertUserCanRunValidate(opCtx, request());
        }

        Reply typedRun(OperationContext* opCtx) {
            overrideAPIParams(opCtx, request().getApiParameters());
            runApiVersionValidation(opCtx);

            _reply.setApiVersionErrors(std::move(apiVersionErrors));

            // Reset API parameters.
            APIParameters::get(opCtx) = APIParameters();
            return _reply;
        }

    private:
        void runApiVersionValidation(OperationContext* opCtx) {
            auto collectionCatalog = CollectionCatalog::get(opCtx);
            auto validateCmdRequest = this->request();

            // If there is no database name present in the input, run validation against all the
            // databases.
            auto dbNames = validateCmdRequest.getDb()
                ? std::vector<DatabaseName>{DatabaseName(getActiveTenant(opCtx),
                                                         validateCmdRequest.getDb()->toString())}
                : collectionCatalog->getAllDbNames();

            for (const auto& dbName : dbNames) {
                AutoGetDb autoDb(opCtx, dbName, LockMode::MODE_IS);
                if (!autoDb.getDb()) {
                    continue;
                }

                if (validateCmdRequest.getCollection()) {
                    if (!_validateNamespace(
                            opCtx,
                            NamespaceString(dbName.db(), *validateCmdRequest.getCollection()))) {
                        return;
                    }
                    continue;
                }

                // If there is no collection name present in the input, run validation against all
                // the collections.
                collectionCatalog->iterateViews(
                    opCtx, dbName, [this, opCtx](const ViewDefinition& view) {
                        return _validateView(opCtx, view);
                    });

                for (auto collIt = collectionCatalog->begin(opCtx, dbName);
                     collIt != collectionCatalog->end(opCtx);
                     ++collIt) {
                    if (!_validateNamespace(
                            opCtx,
                            collectionCatalog->lookupNSSByUUID(opCtx, collIt.uuid().get()).get())) {
                        return;
                    }
                }
            }
        }

        /**
         * Returns false, if the evaluation needs to be aborted.
         */
        bool _validateView(OperationContext* opCtx, const ViewDefinition& view) {
            auto pipelineStatus = view_catalog_helpers::validatePipeline(opCtx, view);
            if (!pipelineStatus.isOK()) {
                ErrorReplyElement error(view.name().ns(),
                                        ErrorCodes::APIStrictError,
                                        ErrorCodes::errorString(ErrorCodes::APIStrictError),
                                        pipelineStatus.getStatus().reason());
                if (!_sizeTracker.incrementAndCheckOverflow(error)) {
                    _reply.setHasMoreErrors(true);
                    return false;
                }
                apiVersionErrors.push_back(error);
            }
            return true;
        }

        /**
         * Returns false, if the evaluation needs to be aborted.
         */
        bool _validateNamespace(OperationContext* opCtx, const NamespaceStringOrUUID& coll) {
            bool apiStrict = APIParameters::get(opCtx).getAPIStrict().value_or(false);
            auto apiVersion = APIParameters::get(opCtx).getAPIVersion().value_or("");

            // We permit views here so that user requested views can be allowed.
            AutoGetCollection collection(
                opCtx, coll, LockMode::MODE_IS, AutoGetCollectionViewMode::kViewsPermitted);

            // If it view, just do the validations for view.
            if (auto viewDef = collection.getView()) {
                return _validateView(opCtx, *viewDef);
            }

            if (!collection.getCollection()) {
                return true;
            }
            const auto status = collection->checkValidatorAPIVersionCompatability(opCtx);
            if (!status.isOK()) {
                ErrorReplyElement error(coll.nss()->ns(),
                                        ErrorCodes::APIStrictError,
                                        ErrorCodes::errorString(ErrorCodes::APIStrictError),
                                        status.reason());

                if (!_sizeTracker.incrementAndCheckOverflow(error)) {
                    _reply.setHasMoreErrors(true);
                    return false;
                }
                apiVersionErrors.push_back(error);
            }

            // Ensure there are no unstable indexes.
            const auto* indexCatalog = collection->getIndexCatalog();
            auto ii = indexCatalog->getIndexIterator(
                opCtx,
                IndexCatalog::InclusionPolicy::kReady | IndexCatalog::InclusionPolicy::kUnfinished |
                    IndexCatalog::InclusionPolicy::kFrozen);
            while (ii->more()) {
                // Check if the index is allowed in API version 1.
                const IndexDescriptor* desc = ii->next()->descriptor();
                if (apiStrict && apiVersion == "1" &&
                    !index_key_validate::isIndexAllowedInAPIVersion1(*desc)) {
                    ErrorReplyElement error(coll.nss()->ns(),
                                            ErrorCodes::APIStrictError,
                                            ErrorCodes::errorString(ErrorCodes::APIStrictError),
                                            str::stream()
                                                << "The index with name " << desc->indexName()
                                                << " is not allowed in API version 1.");
                    if (!_sizeTracker.incrementAndCheckOverflow(error)) {
                        _reply.setHasMoreErrors(true);
                        return false;
                    }
                    apiVersionErrors.push_back(error);
                }
            }
            return true;
        }

        ValidateDBMetadataSizeTracker _sizeTracker;
        std::vector<ErrorReplyElement> apiVersionErrors;
        ValidateDBMetadataCommandReply _reply;
    };
} validateDBMetadataCmd;
}  // namespace mongo
