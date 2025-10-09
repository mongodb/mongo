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

#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

constexpr auto kCreateCommandHelp =
    "explicitly creates a collection or view\n"
    "{\n"
    "  create: <string: collection or view name> [,\n"
    "  capped: <bool: capped collection>,\n"
    "  autoIndexId: <bool: automatic creation of _id index>,\n"
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

BSONObj pipelineAsBsonObj(const std::vector<BSONObj>& pipeline) {
    BSONArrayBuilder builder;
    for (const auto& stage : pipeline) {
        builder.append(stage);
    }
    return builder.obj();
}

/**
 * Compares the provided `CollectionOptions` to the the options for the provided `NamespaceString`
 * in the storage catalog.
 * If the options match, does nothing.
 * If the options do not match, throws an exception indicating what doesn't match.
 * If `ns` is not found in the storage catalog (because it was dropped between checking for its
 * existence and calling this function), throws the original `NamespaceExists` exception.
 */
void checkCollectionOptions(OperationContext* opCtx,
                            const Status& originalStatus,
                            const NamespaceString& ns,
                            const CollectionOptions& options) {
    AutoGetDb autoDb(opCtx, ns.dbName(), MODE_IS);
    Lock::CollectionLock collLock(opCtx, ns, MODE_IS);

    auto collatorFactory = CollatorFactoryInterface::get(opCtx->getServiceContext());

    const auto catalog = CollectionCatalog::get(opCtx);
    const auto coll = catalog->lookupCollectionByNamespace(opCtx, ns);
    if (coll) {
        auto actualOptions = coll->getCollectionOptions();
        uassert(ErrorCodes::NamespaceExists,
                str::stream() << "namespace " << ns.ns()
                              << " already exists, but with different options: "
                              << actualOptions.toBSON(),
                options.matchesStorageOptions(actualOptions, collatorFactory));
        return;
    }

    const auto view = catalog->lookupView(opCtx, ns);
    if (!view) {
        // If the collection/view disappeared in between attempting to create it
        // and retrieving the options, just propagate the original error.
        uassertStatusOK(originalStatus);
        // The assertion above should always fail, as this function should only ever be called
        // if the original attempt to create the collection failed.
        MONGO_UNREACHABLE;
    }

    auto fullNewNamespace =
        NamespaceStringUtil::parseNamespaceFromRequest(ns.dbName(), options.viewOn);
    uassert(ErrorCodes::NamespaceExists,
            str::stream() << "namespace " << ns.ns() << " already exists, but is a view on "
                          << view->viewOn() << " rather than " << fullNewNamespace,
            view->viewOn() == fullNewNamespace);

    auto existingPipeline = pipelineAsBsonObj(view->pipeline());
    uassert(ErrorCodes::NamespaceExists,
            str::stream() << "namespace " << ns.ns() << " already exists, but with pipeline "
                          << existingPipeline << " rather than " << options.pipeline,
            existingPipeline.woCompare(options.pipeline) == 0);

    // Note: the server can add more values to collation options which were not
    // specified in the original user request. Use the collator to check for
    // equivalence.
    auto newCollator = options.collation.isEmpty()
        ? nullptr
        : uassertStatusOK(collatorFactory->makeFromBSON(options.collation));

    if (!CollatorInterface::collatorsMatch(view->defaultCollator(), newCollator.get())) {
        const auto defaultCollatorSpecBSON =
            view->defaultCollator() ? view->defaultCollator()->getSpec().toBSON() : BSONObj();
        uasserted(ErrorCodes::NamespaceExists,
                  str::stream() << "namespace " << ns.ns()
                                << " already exists, but with collation: "
                                << defaultCollatorSpecBSON << " rather than " << options.collation);
    }
}

void checkTimeseriesBucketsCollectionOptions(OperationContext* opCtx,
                                             const Status& error,
                                             const NamespaceString& bucketsNs,
                                             CollectionOptions& options) {
    AutoGetCollectionForReadMaybeLockFree coll{opCtx, bucketsNs};
    uassert(error.code(), error.reason(), coll);

    auto existingOptions = coll->getCollectionOptions();
    uassert(error.code(), error.reason(), existingOptions.timeseries);

    uassertStatusOK(timeseries::validateAndSetBucketingParameters(*options.timeseries));

    // When checking that the options for the buckets collection are the same, filter out the
    // options that were internally generated upon time-series collection creation (i.e. were not
    // specified by the user).
    uassert(error.code(),
            error.reason(),
            options.matchesStorageOptions(
                uassertStatusOK(CollectionOptions::parse(existingOptions.toBSON(
                    false /* includeUUID */, timeseries::kAllowedCollectionCreationOptions))),
                CollatorFactoryInterface::get(opCtx->getServiceContext())));
}

void checkTimeseriesViewOptions(OperationContext* opCtx,
                                const Status& error,
                                const NamespaceString& viewNs,
                                const CollectionOptions& options) {
    AutoGetCollectionForReadMaybeLockFree acquisition{
        opCtx,
        viewNs,
        AutoGetCollection::Options{}.viewMode(auto_get_collection::ViewMode::kViewsPermitted)};
    uassert(error.code(), error.reason(), acquisition.getView());
    const auto& view = *acquisition.getView();

    uassert(error.code(), error.reason(), view.viewOn() == viewNs.makeTimeseriesBucketsNamespace());
    uassert(error.code(),
            error.reason(),
            CollatorInterface::collatorsMatch(
                view.defaultCollator(),
                !options.collation.isEmpty()
                    ? uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                          ->makeFromBSON(options.collation))
                          .get()
                    : nullptr));
}

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
        return kCreateCommandHelp.toString();
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

        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassertStatusOK(auth::checkAuthForCreate(
                opCtx, AuthorizationSession::get(opCtx->getClient()), request(), false));
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        CreateCommandReply typedRun(OperationContext* opCtx) final {
            auto cmd = request();

            CreateCommandReply reply;
            if (cmd.getAutoIndexId()) {
#define DEPR_23800 "The autoIndexId option is deprecated and will be removed in a future release"
                LOGV2_WARNING(23800, DEPR_23800);
                reply.setNote(StringData(DEPR_23800));
#undef DEPR_23800
            }

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
                if (cmd.getCapped()) {
                    uassert(ErrorCodes::Error(6127800),
                            "Clustered capped collection only available with 'enableTestCommands' "
                            "server parameter",
                            getTestCommandsEnabled());
                }

                uassert(ErrorCodes::Error(6049200),
                        str::stream() << "'size' field for capped collections is not "
                                         "allowed on clustered collections. "
                                      << "Did you mean 'capped: true' with 'expireAfterSeconds'?",
                        !cmd.getSize());
                uassert(ErrorCodes::Error(6049204),
                        str::stream() << "'max' field for capped collections is not "
                                         "allowed on clustered collections. "
                                      << "Did you mean 'capped: true' with 'expireAfterSeconds'?",
                        !cmd.getMax());
                if (cmd.getCapped()) {
                    uassert(ErrorCodes::Error(6049201),
                            "A capped clustered collection requires the 'expireAfterSeconds' field",
                            cmd.getExpireAfterSeconds());
                }
            }

            // The 'temp' field is only allowed to be used internally and isn't available to
            // clients.
            if (cmd.getTemp()) {
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "the 'temp' field is an invalid option",
                        opCtx->getClient()->isInDirectClient() ||
                            (opCtx->getClient()->session()->getTags() &
                             transport::Session::kInternalClient));
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
                        repl::ReplicationCoordinator::get(opCtx)->getReplicationMode() ==
                            repl::ReplicationCoordinator::Mode::modeReplSet);

                if (hasQueryType(cmd.getEncryptedFields().get(), QueryTypeEnum::RangePreview)) {
                    uassert(
                        6775220,
                        "Queryable Encryption Range support is only supported when FCV supports "
                        "6.1",
                        gFeatureFlagFLE2Range.isEnabled(
                            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));
                }
            }

            if (auto timeseries = cmd.getTimeseries()) {
                for (auto&& option : cmd.toBSON({})) {
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
                                << cmd.getNamespace() << ": 'timeseries' is not allowed with '"
                                << fieldName << "'",
                            timeseries::kAllowedCollectionCreationOptions.contains(fieldName));
                }

                auto hasDot = [](StringData field) -> bool {
                    return field.find('.') != std::string::npos;
                };
                auto mustBeTopLevel = [&cmd](StringData field) -> std::string {
                    return str::stream()
                        << cmd.getNamespace() << ": '" << field << "' must be a top-level field "
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

                uassert(ErrorCodes::InvalidOptions,
                        str::stream()
                            << "'idIndex' is not allowed with 'autoIndexId': " << idIndexSpec,
                        !cmd.getAutoIndexId());

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
                    invariant(collatorStatus.isOK());
                    idIndexCollator = std::move(collatorStatus.getValue());
                }
                if (!CollatorInterface::collatorsMatch(defaultCollator.get(),
                                                       idIndexCollator.get())) {
                    uasserted(ErrorCodes::BadValue,
                              "'idIndex' must have the same collation as the collection.");
                }

                cmd.setIdIndex(idIndexSpec);
            }

            if (cmd.getValidator() || cmd.getValidationLevel() || cmd.getValidationAction()) {
                // Check for config.settings in the user command since a validator is allowed
                // internally on this collection but the user may not modify the validator.
                uassert(ErrorCodes::InvalidOptions,
                        str::stream()
                            << "Document validators not allowed on system collection " << ns(),
                        ns() != NamespaceString::kConfigSettingsNamespace);
            }

            OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                unsafeCreateCollection(opCtx);

            const auto createStatus = createCollection(opCtx, cmd);
            // NamespaceExists will cause multi-document transactions to implicitly abort, so
            // in that case we should surface the error to the client. Otherwise, return success
            // if a collection with identical options already exists.
            if (createStatus == ErrorCodes::NamespaceExists &&
                !opCtx->inMultiDocumentTransaction()) {
                auto options = CollectionOptions::fromCreateCommand(cmd);
                if (options.timeseries) {
                    checkTimeseriesBucketsCollectionOptions(
                        opCtx,
                        createStatus,
                        cmd.getNamespace().makeTimeseriesBucketsNamespace(),
                        options);
                    checkTimeseriesViewOptions(opCtx, createStatus, cmd.getNamespace(), options);
                } else {
                    checkCollectionOptions(opCtx, createStatus, cmd.getNamespace(), options);
                }
            } else {
                uassertStatusOK(createStatus);
            }

            return reply;
        }
    };
} cmdCreate;

}  // namespace
}  // namespace mongo
