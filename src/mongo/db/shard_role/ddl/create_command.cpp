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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/index_key_validate.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/create_gen.h"
#include "mongo/db/shard_role/ddl/replica_set_ddl_tracker.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(preImagesEnabledOnAllCollectionsByDefault);

constexpr auto kCreateCommandHelp =
    "explicitly creates a collection or view\n"
    "{\n"
    "  create: <string: collection or view name> [,\n"
    "  capped: <bool: capped collection>,\n"
    "  idIndex: <document: _id index specification>,\n"
    "  size: <int: size in bytes of the capped collection>,\n"
    "  max: <int: max number of documents in the capped collection>,\n"
    "  storageEngine: <document: storage engine configuration>,\n"
    "  validator: <document: validation rules>,\n"
    "  validationLevel: <string: validation level>,\n"
    "  validationAction: <string: validation action>,\n"
    "  indexOptionDefaults: <document: default configuration for indexes>,\n"
    "  viewOn: <string: name of source collection or view>,\n"
    "  pipeline: <array<object>: aggregation pipeline stage>,\n"
    "  collation: <document: default collation for the collection or view>,\n"
    "  changeStreamPreAndPostImages: <document: pre- and post-images options for change streams>,\n"
    "  writeConcern: <document: write concern expression for the operation>]\n"
    "}"_sd;

class CmdCreate final : public CreateCmdVersion1Gen<CmdCreate> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const final {
        return false;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }

    std::string help() const final {
        return std::string{kCreateCommandHelp};
    }

    bool allowedInTransactions() const final {
        return true;
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return true;
        }

        bool isSubjectToIngressAdmissionControl() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassertStatusOK(auth::checkAuthForCreate(
                opCtx, AuthorizationSession::get(opCtx->getClient()), request(), false));
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        CreateCommandReply typedRun(OperationContext* opCtx) final {
            VersionContext::FixedOperationFCVRegion fixedOfcvRegion(opCtx);

            // Intentional copy of request made here, as request object can be modified below.
            auto cmd = request();

            ReplicaSetDDLTracker::ScopedReplicaSetDDL scopedReplicaSetDDL(
                opCtx, std::vector<NamespaceString>{cmd.getNamespace()});

            CreateCommandReply reply;

            if (!cmd.getClusteredIndex()) {
                // Ensure that the 'size' field is present if 'capped' is set to true and this is
                // not a clustered collection.
                if (cmd.getCapped()) {
                    uassert(ErrorCodes::InvalidOptions,
                            str::stream() << "the 'size' field is required when 'capped' is true",
                            cmd.getSize());
                }

                // If the 'size' or 'max' fields are present and this is not a clustered collection,
                // then 'capped' must be set to true.
                if (cmd.getSize() || cmd.getMax()) {
                    uassert(ErrorCodes::InvalidOptions,
                            str::stream()
                                << "the 'capped' field needs to be true when either the 'size'"
                                << " or 'max' fields are present",
                            cmd.getCapped());
                }
            } else {
                // Clustered collection.
                clustered_util::checkCreationOptions(cmd);
            }

            // The 'temp' field is only allowed to be used internally and isn't available to
            // clients.
            if (cmd.getTemp()) {
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "the 'temp' field is an invalid option",
                        opCtx->getClient()->isInDirectClient() ||
                            (opCtx->getClient()->isInternalClient()));
            }

            if (cmd.getPipeline()) {
                uassert(ErrorCodes::InvalidOptions,
                        "'pipeline' requires 'viewOn' to also be specified",
                        cmd.getViewOn());
            }

            if (cmd.getEncryptedFields()) {
                uassert(6367301,
                        "Encrypted fields cannot be used with capped collections",
                        !cmd.getCapped());

                uassert(6346401,
                        "Encrypted fields cannot be used with views or timeseries collections",
                        !(cmd.getViewOn() || cmd.getTimeseries()));

                uassert(6346402,
                        "Encrypted collections are not supported on standalone",
                        repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet());

                uassert(8575605,
                        "Cannot create a collection with an encrypted field with query "
                        "type rangePreview, as it is deprecated",
                        !hasQueryType(cmd.getEncryptedFields().get(),
                                      QueryTypeEnum::RangePreviewDeprecated));

                if (!gFeatureFlagQETextSearchPreview.isEnabledUseLastLTSFCVWhenUninitialized(
                        VersionContext::getDecoration(opCtx),
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                    uassert(9783415,
                            "Cannot create a collection with an encrypted field with query type "
                            "substringPreview unless featureFlagQETextSearchPreview is enabled",
                            !hasQueryType(cmd.getEncryptedFields().get(),
                                          QueryTypeEnum::SubstringPreview));
                    uassert(9783416,
                            "Cannot create a collection with an encrypted field with query type "
                            "suffixPreview unless featureFlagQETextSearchPreview is enabled",
                            !hasQueryType(cmd.getEncryptedFields().get(),
                                          QueryTypeEnum::SuffixPreview));
                    uassert(9783417,
                            "Cannot create a collection with an encrypted field with query type "
                            "prefixPreview unless featureFlagQETextSearchPreview is enabled",
                            !hasQueryType(cmd.getEncryptedFields().get(),
                                          QueryTypeEnum::PrefixPreview));
                    uassert(10075600,
                            "Cannot create a collection with a strEncodeVersion set unless "
                            "featureFlagQETextSearchPreview is enabled",
                            !cmd.getEncryptedFields()->getStrEncodeVersion());
                } else {
                    EncryptionInformationHelpers::checkTagLimitsAndStorageNotExceeded(
                        cmd.getEncryptedFields().get());
                    EncryptionInformationHelpers::checkSubstringPreviewParameterLimitsNotExceeded(
                        cmd.getEncryptedFields().get());
                }
            }

            if (auto timeseries = cmd.getTimeseries()) {
                for (auto&& option : cmd.toBSON()) {
                    auto fieldName = option.fieldNameStringData();

                    if (fieldName == CreateCommand::kCommandName) {
                        continue;
                    }

                    // The 'capped' option defaults to false. We accept it unless it is set to true.
                    if (fieldName == CreateCommand::kCappedFieldName && !option.Bool()) {
                        continue;
                    }

                    // The 'timeseries' option may be passed with a 'validator' or 'clusteredIndex'
                    // if a buckets collection is being restored. We assume the caller knows what
                    // they are doing.
                    if ((fieldName == CreateCommand::kValidatorFieldName ||
                         fieldName == CreateCommand::kClusteredIndexFieldName) &&
                        cmd.getNamespace().isTimeseriesBucketsCollection()) {
                        continue;
                    }

                    uassert(ErrorCodes::InvalidOptions,
                            str::stream()
                                << cmd.getNamespace().toStringForErrorMsg()
                                << ": 'timeseries' is not allowed with '" << fieldName << "'",
                            timeseries::kAllowedCollectionCreationOptions.contains(fieldName) ||
                                isGenericArgument(fieldName));
                }

                auto hasDot = [](StringData field) -> bool {
                    return field.find('.') != std::string::npos;
                };
                auto mustBeTopLevel = [&cmd](StringData field) -> std::string {
                    return str::stream() << cmd.getNamespace().toStringForErrorMsg() << ": '"
                                         << field << "' must be a top-level field "
                                         << "and not contain a '.'";
                };
                uassert(ErrorCodes::InvalidOptions,
                        mustBeTopLevel("timeField"),
                        !hasDot(timeseries->getTimeField()));

                if (auto metaField = timeseries->getMetaField()) {
                    uassert(ErrorCodes::InvalidOptions,
                            "'metaField' cannot be \"_id\"",
                            *metaField != "_id");
                    uassert(ErrorCodes::InvalidOptions,
                            "'metaField' cannot be the same as 'timeField'",
                            *metaField != timeseries->getTimeField());
                    uassert(ErrorCodes::InvalidOptions,
                            mustBeTopLevel("metaField"),
                            !hasDot(*metaField));
                }
            }

            if (cmd.getExpireAfterSeconds()) {
                uassert(ErrorCodes::InvalidOptions,
                        "'expireAfterSeconds' is only supported on time-series collections or "
                        "when the 'clusteredIndex' option is specified",
                        cmd.getTimeseries() || cmd.getClusteredIndex());
            }

            // Validate _id index spec and fill in missing fields.
            if (cmd.getIdIndex()) {
                auto idIndexSpec = *cmd.getIdIndex();

                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "'idIndex' is not allowed with 'viewOn': " << idIndexSpec,
                        !cmd.getViewOn());

                // Perform index spec validation.
                idIndexSpec =
                    uassertStatusOK(index_key_validate::validateIndexSpec(opCtx, idIndexSpec));
                uassertStatusOK(index_key_validate::validateIdIndexSpec(idIndexSpec));

                // Validate or fill in _id index collation.
                std::unique_ptr<CollatorInterface> defaultCollator;
                if (cmd.getCollation()) {
                    auto collatorStatus = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                              ->makeFromBSON(cmd.getCollation()->toBSON());
                    uassertStatusOK(collatorStatus.getStatus());
                    defaultCollator = std::move(collatorStatus.getValue());
                }

                idIndexSpec = uassertStatusOK(index_key_validate::validateIndexSpecCollation(
                    opCtx, idIndexSpec, defaultCollator.get()));

                std::unique_ptr<CollatorInterface> idIndexCollator;
                if (auto collationElem = idIndexSpec["collation"]) {
                    auto collatorStatus = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                              ->makeFromBSON(collationElem.Obj());
                    // validateIndexSpecCollation() should have checked that the _id index collation
                    // spec is valid.
                    invariant(collatorStatus.getStatus());
                    idIndexCollator = std::move(collatorStatus.getValue());
                }
                if (!CollatorInterface::collatorsMatch(defaultCollator.get(),
                                                       idIndexCollator.get())) {
                    uasserted(ErrorCodes::BadValue,
                              "'idIndex' must have the same collation as the collection.");
                }

                cmd.getCreateCollectionRequest().setIdIndex(idIndexSpec);
            }

            if (cmd.getValidator() || cmd.getValidationLevel() || cmd.getValidationAction()) {
                // Check for config.settings in the user command since a validator is allowed
                // internally on this collection but the user may not modify the validator.
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "Document validators not allowed on system collection "
                                      << ns().toStringForErrorMsg(),
                        ns() != NamespaceString::kConfigSettingsNamespace);

                const auto fcvSnapshot =
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
                if (cmd.getValidationAction() == ValidationActionEnum::errorAndLog) {
                    uassert(ErrorCodes::InvalidOptions,
                            "Validation action 'errorAndLog' is not supported with current FCV",
                            gFeatureFlagErrorAndLogValidationAction
                                .isEnabledUseLastLTSFCVWhenUninitialized(
                                    VersionContext::getDecoration(opCtx), fcvSnapshot));
                }
                if (cmd.getValidationLevel() == ValidationLevelEnum::validated) {
                    uassert(ErrorCodes::InvalidOptions,
                            "Validation level 'validated' is not supported with current FCV",
                            gFeatureFlagValidatedValidationLevel
                                .isEnabledUseLastLTSFCVWhenUninitialized(
                                    VersionContext::getDecoration(opCtx), fcvSnapshot));
                }
            }

            OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                unsafeCreateCollection(opCtx, cmd.getNamespace());

            preImagesEnabledOnAllCollectionsByDefault.execute([&](const auto&) {
                if (!cmd.getViewOn() && !cmd.getTimeseries() &&
                    validateChangeStreamPreAndPostImagesOptionIsPermitted(cmd.getNamespace())
                        .isOK()) {
                    cmd.getCreateCollectionRequest().setChangeStreamPreAndPostImages(
                        ChangeStreamPreAndPostImagesOptions{true});
                }
            });

            const auto createStatus = createCollection(opCtx, cmd);
            // NamespaceExists will cause multi-document transactions to implicitly abort, so
            // in that case we should surface the error to the client. Otherwise, return success
            // if a collection with identical options already exists.
            if (createStatus == ErrorCodes::NamespaceExists &&
                !opCtx->inMultiDocumentTransaction()) {
                auto collExists = checkNamespaceAndTimeseriesBucketsAlreadyExists(opCtx, cmd);
                if (!collExists) {
                    // If the collection/view disappeared in between attempting to create it
                    // and retrieving the options, just propagate the original error.
                    uassertStatusOK(createStatus);
                }
            } else {
                uassertStatusOK(createStatus);
            }

            return reply;
        }
    };
};
MONGO_REGISTER_COMMAND(CmdCreate).forShard();

}  // namespace
}  // namespace mongo
