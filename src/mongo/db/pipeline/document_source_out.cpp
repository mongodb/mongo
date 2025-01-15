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


#include <fmt/format.h>
#include <iterator>
#include <mutex>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/writer_util.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

using namespace fmt::literals;

MONGO_FAIL_POINT_DEFINE(hangWhileBuildingDocumentSourceOutBatch);
MONGO_FAIL_POINT_DEFINE(outWaitAfterTempCollectionCreation);
MONGO_FAIL_POINT_DEFINE(outWaitBeforeTempCollectionRename);
MONGO_FAIL_POINT_DEFINE(outWaitAfterTempCollectionRenameBeforeView);

REGISTER_DOCUMENT_SOURCE(out,
                         DocumentSourceOut::LiteParsed::parse,
                         DocumentSourceOut::createFromBson,
                         AllowedWithApiStrict::kAlways);
ALLOCATE_DOCUMENT_SOURCE_ID(out, DocumentSourceOut::id)

DocumentSourceOut::~DocumentSourceOut() {
    if (_tmpCleanUpState == OutCleanUpProgress::kComplete) {
        return;
    }

    try {
        // Make sure we drop the temp collection(s) if anything goes wrong.
        // Errors are ignored here because nothing can be done about them. Additionally, if
        // this fails and the collection is left behind, it will be cleaned up next time the
        // server is started.
        auto cleanupClient =
            pExpCtx->getOperationContext()->getService()->makeClient("$out_replace_coll_cleanup");
        AlternativeClientRegion acr(cleanupClient);

        // Create a new operation context so that any interrupts on the current operation
        // will not affect the dropCollection operation below.
        auto cleanupOpCtx = cc().makeOperationContext();
        DocumentSourceWriteBlock writeBlock(cleanupOpCtx.get());
        auto dropCollectionCmd = [&](NamespaceString dropNs) {
            try {
                pExpCtx->getMongoProcessInterface()->dropTempCollection(cleanupOpCtx.get(), dropNs);
            } catch (const DBException& e) {
                LOGV2_WARNING(7466203,
                              "Unexpected error dropping temporary collection; drop will complete "
                              "on next server restart",
                              "error"_attr = e.toString(),
                              "coll"_attr = dropNs);
            };
        };

        switch (_tmpCleanUpState) {
            case OutCleanUpProgress::kTmpCollExists:
                dropCollectionCmd(_tempNs);
                break;
            case OutCleanUpProgress::kRenameComplete:
                // For time-series collections, since we haven't created a view in this state, we
                // must drop the buckets collection.
                // TODO SERVER-92272 Update this to only drop the collection iff a time-series view
                // doesn't exist.
                if (_timeseries) {
                    auto collType = pExpCtx->getMongoProcessInterface()->getCollectionType(
                        cleanupOpCtx.get(), getOutputNs());
                    if (collType != query_shape::CollectionType::kTimeseries) {
                        dropCollectionCmd(getOutputNs().makeTimeseriesBucketsNamespace());
                    }
                }
                [[fallthrough]];
            case OutCleanUpProgress::kViewCreatedIfNeeded:
                // This state indicates that the rename succeeded, but 'dropTempCollection' hasn't
                // finished. For sharding we must also explicitly call 'dropTempCollection' on the
                // temporary namespace to remove the namespace from the list of in-use temporary
                // collections.
                dropCollectionCmd(_tempNs);
                break;
            default:
                MONGO_UNREACHABLE;
                break;
        }
    } catch (...) {
        reportFailedDestructor(MONGO_SOURCE_LOCATION());
    }
}

StageConstraints DocumentSourceOut::constraints(Pipeline::SplitState pipeState) const {
    StageConstraints result{StreamType::kStreaming,
                            PositionRequirement::kLast,
                            HostTypeRequirement::kNone,
                            DiskUseRequirement::kWritesPersistentData,
                            FacetRequirement::kNotAllowed,
                            TransactionRequirement::kNotAllowed,
                            LookupRequirement::kNotAllowed,
                            UnionRequirement::kNotAllowed};
    if (pipeState == Pipeline::SplitState::kSplitForMerge) {
        // If output collection resides on a single shard, we should route $out to it to perform
        // local writes. Note that this decision is inherently racy and subject to become stale.
        // This is okay because either choice will work correctly, we are simply applying a
        // heuristic optimization.
        result.mergeShardId = getMergeShardId();
    }
    return result;
}

DocumentSourceOutSpec DocumentSourceOut::parseOutSpecAndResolveTargetNamespace(
    const BSONElement& spec, const DatabaseName& defaultDB) {
    DocumentSourceOutSpec outSpec;
    if (spec.type() == BSONType::String) {
        outSpec.setColl(spec.valueStringData());
        // TODO SERVER-77000: access a SerializationContext object to serialize properly
        outSpec.setDb(defaultDB.serializeWithoutTenantPrefix_UNSAFE());
    } else if (spec.type() == BSONType::Object) {
        // TODO SERVER-77000: access a SerializationContext object to pass into the IDLParserContext
        outSpec = mongo::DocumentSourceOutSpec::parse(IDLParserContext(kStageName),
                                                      spec.embeddedObject());
    } else {
        uassert(16990,
                "{} only supports a string or object argument, but found {}"_format(
                    kStageName, typeName(spec.type())),
                spec.type() == BSONType::String);
    }

    return outSpec;
}

NamespaceString DocumentSourceOut::makeBucketNsIfTimeseries(const NamespaceString& ns) {
    return _timeseries ? ns.makeTimeseriesBucketsNamespace() : ns;
}

std::unique_ptr<DocumentSourceOut::LiteParsed> DocumentSourceOut::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec) {
    auto outSpec = parseOutSpecAndResolveTargetNamespace(spec, nss.dbName());
    NamespaceString targetNss = NamespaceStringUtil::deserialize(nss.dbName().tenantId(),
                                                                 outSpec.getDb(),
                                                                 outSpec.getColl(),
                                                                 outSpec.getSerializationContext());

    uassert(ErrorCodes::InvalidNamespace,
            "Invalid {} target namespace, {}"_format(kStageName, targetNss.toStringForErrorMsg()),
            targetNss.isValid());
    return std::make_unique<DocumentSourceOut::LiteParsed>(spec.fieldName(), std::move(targetNss));
}

boost::optional<TimeseriesOptions> DocumentSourceOut::validateTimeseries() {
    const BSONElement targetTimeseriesElement = _originalOutOptions["timeseries"];
    boost::optional<TimeseriesOptions> targetTSOpts;
    if (targetTimeseriesElement) {
        tassert(
            9072001, "Invalid time-series options received", targetTimeseriesElement.isABSONObj());
        targetTSOpts = TimeseriesOptions::parseOwned(IDLParserContext("TimeseriesOptions"),
                                                     targetTimeseriesElement.Obj());
    }

    // If the user did not specify the 'timeseries' option in the input, but the target
    // namespace is a time-series collection, then we will use the target collection time-series
    // options, and treat this operation as a write to time-series collection.
    if (!_timeseries) {
        // Must update '_originalOutOptions' to be on the buckets namespace, since previously we
        // didn't know we will be writing a time-series collection.
        if (targetTSOpts) {
            _originalOutOptions =
                pExpCtx->getMongoProcessInterface()
                    ->getCollectionOptions(pExpCtx->getOperationContext(),
                                           getOutputNs().makeTimeseriesBucketsNamespace())
                    .removeField("uuid");
        }
        return targetTSOpts;
    }

    // If the user specified 'timeseries' options, the target namespace must be a time-series
    // collection. Note that the result of 'getCollectionType' can become stale at
    // anytime and shouldn't be referenced at any other point. $out should account for
    // concurrent view or collection creation during each step of its execution.
    uassert(7268700,
            "Cannot create a time-series collection from a non time-series collection or view.",
            targetTSOpts ||
                pExpCtx->getMongoProcessInterface()->getCollectionType(
                    pExpCtx->getOperationContext(), getOutputNs()) ==
                    query_shape::CollectionType::kNonExistent);

    // If the user did specify 'timeseries' options and the target namespace is a time-series
    // collection, then the time-series options should match.
    uassert(7406103,
            str::stream() << "Time-series options inputted must match the existing time-series "
                             "collection. Received: "
                          << _timeseries->toBSON().toString()
                          << "Found: " << targetTimeseriesElement.toString(),
            !targetTSOpts ||
                timeseries::optionsAreEqual(_timeseries.value(), targetTSOpts.value()));

    return _timeseries;
}

void DocumentSourceOut::createTemporaryCollection() {
    BSONObjBuilder createCommandOptions;
    if (_timeseries) {
        // Append the original collection options without the 'validator' and 'clusteredIndex'
        // fields since these fields are invalid with the 'timeseries' field and will be
        // recreated when the buckets collection is created.
        _originalOutOptions.isEmpty()
            ? createCommandOptions << DocumentSourceOutSpec::kTimeseriesFieldName
                                   << _timeseries->toBSON()
            : createCommandOptions.appendElementsUnique(
                  _originalOutOptions.removeFields(StringDataSet{"clusteredIndex", "validator"}));
    } else {
        createCommandOptions.appendElementsUnique(_originalOutOptions);
    }

    // If the output collection exists, we should create the temp collection on the shard that
    // owns the output collection.
    auto targetShard = getMergeShardId();

    // Set the enum state to 'kTmpCollExists' first, because 'createTempCollection' can throw
    // after constructing the collection.
    _tmpCleanUpState = OutCleanUpProgress::kTmpCollExists;
    pExpCtx->getMongoProcessInterface()->createTempCollection(
        pExpCtx->getOperationContext(), _tempNs, createCommandOptions.done(), targetShard);
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &outWaitAfterTempCollectionCreation,
        pExpCtx->getOperationContext(),
        "outWaitAfterTempCollectionCreation",
        []() {
            LOGV2(20901,
                  "Hanging aggregation due to 'outWaitAfterTempCollectionCreation' failpoint");
        });
}

void DocumentSourceOut::initialize() {
    DocumentSourceWriteBlock writeBlock(pExpCtx->getOperationContext());
    // We will create a temporary collection with the same indexes and collection options as the
    // target collection if it exists. We will write all results into a temporary collection, then
    // rename the temporary collection to be the target collection once we are done.

    try {
        // Save the original collection options and index specs so we can check they didn't change
        // during computation. For time-series collections, these should be run on the buckets
        // namespace.
        _originalOutOptions =
            // The uuid field is considered an option, but cannot be passed to createCollection.
            pExpCtx->getMongoProcessInterface()
                ->getCollectionOptions(pExpCtx->getOperationContext(),
                                       makeBucketNsIfTimeseries(getOutputNs()))
                .removeField("uuid");

        // Use '_originalOutOptions' to correctly determine if we are writing to a time-series
        // collection.
        _timeseries = validateTimeseries();
        _originalIndexes = pExpCtx->getMongoProcessInterface()->getIndexSpecs(
            pExpCtx->getOperationContext(),
            makeBucketNsIfTimeseries(getOutputNs()),
            false /* includeBuildUUIDs */);

        // Check if it's capped to make sure we have a chance of succeeding before we do all the
        // work. If the collection becomes capped during processing, the collection options will
        // have changed, and the $out will fail.
        uassert(17152,
                "namespace '{}' is capped so it can't be used for {}"_format(
                    getOutputNs().toStringForErrorMsg(), kStageName),
                _originalOutOptions["capped"].eoo());
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        LOGV2_DEBUG(7585601,
                    5,
                    "Database for $out target collection doesn't exist. Assuming default indexes "
                    "and options");
    }

    //  Note that this temporary collection name is used by MongoMirror and thus should not be
    //  changed without consultation.
    _tempNs = makeBucketNsIfTimeseries(NamespaceStringUtil::deserialize(
        getOutputNs().dbName(),
        str::stream() << NamespaceString::kOutTmpCollectionPrefix << UUID::gen()));

    uassert(7406100,
            "$out to time-series collections is only supported on FCV greater than or equal to 7.1",
            feature_flags::gFeatureFlagAggOutTimeseries.isEnabled(
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) ||
                !_timeseries);

    createTemporaryCollection();
    if (_originalIndexes.empty()) {
        return;
    }

    // Copy the indexes of the output collection to the temp collection.
    // Note that on timeseries collections, indexes are to be created on the buckets collection.
    try {
        std::vector<BSONObj> tempNsIndexes = {std::begin(_originalIndexes),
                                              std::end(_originalIndexes)};
        pExpCtx->getMongoProcessInterface()->createIndexesOnEmptyCollection(
            pExpCtx->getOperationContext(), _tempNs, tempNsIndexes);
    } catch (DBException& ex) {
        ex.addContext("Copying indexes for $out failed");
        throw;
    }
}

void DocumentSourceOut::renameTemporaryCollection() {
    // If the collection is time-series, we must rename to the "real" buckets collection.
    const NamespaceString& outputNs = makeBucketNsIfTimeseries(getOutputNs());
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &outWaitBeforeTempCollectionRename,
        pExpCtx->getOperationContext(),
        "outWaitBeforeTempCollectionRename",
        []() {
            LOGV2(7585602,
                  "Hanging aggregation due to 'outWaitBeforeTempCollectionRename' failpoint");
        });
    pExpCtx->getMongoProcessInterface()->renameIfOptionsAndIndexesHaveNotChanged(
        pExpCtx->getOperationContext(),
        _tempNs,
        outputNs,
        true /* dropTarget */,
        false /* stayTemp */,
        _originalOutOptions,
        _originalIndexes);
}

void DocumentSourceOut::createTimeseriesView() {
    _tmpCleanUpState = OutCleanUpProgress::kRenameComplete;
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &outWaitAfterTempCollectionRenameBeforeView,
        pExpCtx->getOperationContext(),
        "outWaitAfterTempCollectionRenameBeforeView",
        []() {
            LOGV2(8961400,
                  "Hanging aggregation due to 'outWaitAfterTempCollectionRenameBeforeView' "
                  "failpoint");
        });

    BSONObjBuilder cmd;
    cmd << "create" << getOutputNs().coll();
    cmd << DocumentSourceOutSpec::kTimeseriesFieldName << _timeseries->toBSON();
    pExpCtx->getMongoProcessInterface()->createTimeseriesView(
        pExpCtx->getOperationContext(), getOutputNs(), cmd.done(), _timeseries.value());
}

void DocumentSourceOut::finalize() {
    DocumentSourceWriteBlock writeBlock(pExpCtx->getOperationContext());
    uassert(7406101,
            "$out to time-series collections is only supported on FCV greater than or equal to 7.1",
            feature_flags::gFeatureFlagAggOutTimeseries.isEnabled(
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) ||
                !_timeseries);

    // Rename the temporary collection to the namespace the user requested, and drop the target
    // collection if $out is writing to a collection that exists.
    renameTemporaryCollection();

    // The rename has succeeded, if the collection is time-series, try to create the view. Creating
    // the view must happen immediately after the rename. We cannot guarantee that both the rename
    // and view creation for time-series will succeed if there is an unclean shutdown. This could
    // lead us to an unsupported state (a buckets collection with no view). To minimize the chance
    // this happens, we should ensure that view creation is tried immediately after the rename
    // succeeds.
    if (_timeseries) {
        createTimeseriesView();
    }

    // The rename succeeded, so the temp collection no longer exists. Call 'dropTempCollection'
    // anyway to ensure that we remove it from the list of in-use temporary collections that will be
    // dropped on stepup (relevant on sharded clusters).
    _tmpCleanUpState = OutCleanUpProgress::kViewCreatedIfNeeded;
    pExpCtx->getMongoProcessInterface()->dropTempCollection(pExpCtx->getOperationContext(),
                                                            _tempNs);

    _tmpCleanUpState = OutCleanUpProgress::kComplete;
}

BatchedCommandRequest DocumentSourceOut::makeBatchedWriteRequest() const {
    const auto& nss =
        _tempNs.isTimeseriesBucketsCollection() ? _tempNs.getTimeseriesViewNamespace() : _tempNs;
    return makeInsertCommand(nss, pExpCtx->getBypassDocumentValidation());
}

boost::intrusive_ptr<DocumentSource> DocumentSourceOut::create(
    NamespaceString outputNs,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<TimeseriesOptions> timeseries) {
    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            "{} cannot be used in a transaction"_format(kStageName),
            !expCtx->getOperationContext()->inMultiDocumentTransaction());

    uassert(ErrorCodes::InvalidNamespace,
            "Invalid {} target namespace, {}"_format(kStageName, outputNs.toStringForErrorMsg()),
            outputNs.isValid());

    uassert(17385,
            "Can't {} to special collection: {}"_format(kStageName, outputNs.coll()),
            !outputNs.isSystem());

    uassert(31321,
            "Can't {} to internal database: {}"_format(kStageName,
                                                       outputNs.dbName().toStringForErrorMsg()),
            !outputNs.isOnInternalDb());
    return new DocumentSourceOut(std::move(outputNs), std::move(timeseries), expCtx);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceOut::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto outSpec =
        parseOutSpecAndResolveTargetNamespace(elem, expCtx->getNamespaceString().dbName());
    NamespaceString targetNss =
        NamespaceStringUtil::deserialize(expCtx->getNamespaceString().dbName().tenantId(),
                                         outSpec.getDb(),
                                         outSpec.getColl(),
                                         outSpec.getSerializationContext());
    return create(std::move(targetNss), expCtx, std::move(outSpec.getTimeseries()));
}

Value DocumentSourceOut::serialize(const SerializationOptions& opts) const {
    BSONObjBuilder bob;
    DocumentSourceOutSpec spec;
    // TODO SERVER-77000: use SerializatonContext from expCtx and DatabaseNameUtil to serialize
    // spec.setDb(DatabaseNameUtil::serialize(
    //     getOutputNs().dbName(),
    //     SerializationContext::stateCommandReply(pExpCtx->getSerializationContext())));
    spec.setDb(getOutputNs().dbName().serializeWithoutTenantPrefix_UNSAFE());
    spec.setColl(getOutputNs().coll());
    spec.setTimeseries(_timeseries);
    spec.serialize(&bob, opts);
    return Value(Document{{kStageName, bob.done()}});
}

void DocumentSourceOut::waitWhileFailPointEnabled() {
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangWhileBuildingDocumentSourceOutBatch,
        pExpCtx->getOperationContext(),
        "hangWhileBuildingDocumentSourceOutBatch",
        []() {
            LOGV2(20902,
                  "Hanging aggregation due to 'hangWhileBuildingDocumentSourceOutBatch' failpoint");
        });
}

}  // namespace mongo
