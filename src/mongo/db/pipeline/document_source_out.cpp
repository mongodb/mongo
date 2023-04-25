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

#include "mongo/db/pipeline/document_source_out.h"

#include <fmt/format.h>

#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
using namespace fmt::literals;

MONGO_FAIL_POINT_DEFINE(hangWhileBuildingDocumentSourceOutBatch);
MONGO_FAIL_POINT_DEFINE(outWaitAfterTempCollectionCreation);
REGISTER_DOCUMENT_SOURCE(out,
                         DocumentSourceOut::LiteParsed::parse,
                         DocumentSourceOut::createFromBson,
                         AllowedWithApiStrict::kAlways);

DocumentSourceOut::~DocumentSourceOut() {
    DESTRUCTOR_GUARD(
        // Make sure we drop the temp collection if anything goes wrong. Errors are ignored
        // here because nothing can be done about them. Additionally, if this fails and the
        // collection is left behind, it will be cleaned up next time the server is started.

        // If creating a time-series collection, we must drop the "real" buckets collection, if
        // anything goes wrong creating the view.

        // If creating a time-series collection, '_tempNs' is translated to include the
        // "system.buckets" prefix.
        if (_tempNs.size() || (_timeseries && !_timeseriesViewCreated)) {
            auto cleanupClient =
                pExpCtx->opCtx->getServiceContext()->makeClient("$out_replace_coll_cleanup");

            // TODO(SERVER-74662): Please revisit if this thread could be made killable.
            {
                stdx::lock_guard<Client> lk(*cleanupClient.get());
                cleanupClient.get()->setSystemOperationUnkillableByStepdown(lk);
            }

            AlternativeClientRegion acr(cleanupClient);
            // Create a new operation context so that any interrupts on the current operation will
            // not affect the dropCollection operation below.
            auto cleanupOpCtx = cc().makeOperationContext();

            DocumentSourceWriteBlock writeBlock(cleanupOpCtx.get());

            auto deleteNs = _tempNs.size() ? _tempNs : makeBucketNsIfTimeseries(getOutputNs());
            pExpCtx->mongoProcessInterface->dropCollection(cleanupOpCtx.get(), deleteNs);
        });
}

DocumentSourceOutSpec DocumentSourceOut::parseOutSpecAndResolveTargetNamespace(
    const BSONElement& spec, const DatabaseName& defaultDB) {
    DocumentSourceOutSpec outSpec;
    if (spec.type() == BSONType::String) {
        outSpec.setColl(spec.valueStringData());
        outSpec.setDb(defaultDB.db());
    } else if (spec.type() == BSONType::Object) {
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
    NamespaceString targetNss = NamespaceStringUtil::parseNamespaceFromRequest(
        nss.dbName().tenantId(), outSpec.getDb(), outSpec.getColl());

    uassert(ErrorCodes::InvalidNamespace,
            "Invalid {} target namespace, {}"_format(kStageName, targetNss.toStringForErrorMsg()),
            targetNss.isValid());
    return std::make_unique<DocumentSourceOut::LiteParsed>(spec.fieldName(), std::move(targetNss));
}

void DocumentSourceOut::validateTimeseries() {
    const NamespaceString& outNs = getOutputNs();
    if (!_timeseries) {
        return;
    }
    // check if a time-series collection already exists in that namespace.
    auto timeseriesOpts = mongo::timeseries::getTimeseriesOptions(pExpCtx->opCtx, outNs, true);
    if (timeseriesOpts) {
        uassert(7268701,
                "Time field inputted does not match the time field of the existing time-series "
                "collection.",
                _timeseries->getTimeField() == timeseriesOpts->getTimeField());
        uassert(7268702,
                "Meta field inputted does not match the time field of the existing time-series "
                "collection.",
                !_timeseries->getMetaField() ||
                    _timeseries->getMetaField() == timeseriesOpts->getMetaField());
    } else {
        // if a time-series collection doesn't exist, the namespace should not have a
        // collection nor a conflicting view.
        auto collection = CollectionCatalog::get(pExpCtx->opCtx)
                              ->lookupCollectionByNamespace(pExpCtx->opCtx, outNs);
        uassert(7268700,
                "Cannot create a time-series collection from a non time-series collection.",
                !collection);
        auto view = CollectionCatalog::get(pExpCtx->opCtx)->lookupView(pExpCtx->opCtx, outNs);
        uassert(
            7268703, "Cannot create a time-series collection from a non time-series view.", !view);
    }
}

void DocumentSourceOut::initialize() {
    DocumentSourceWriteBlock writeBlock(pExpCtx->opCtx);

    const NamespaceString& outputNs = makeBucketNsIfTimeseries(getOutputNs());

    // We will write all results into a temporary collection, then rename the temporary
    // collection to be the target collection once we are done. Note that this temporary
    // collection name is used by MongoMirror and thus should not be changed without
    // consultation.
    _tempNs = NamespaceStringUtil::parseNamespaceFromRequest(
        getOutputNs().tenantId(),
        str::stream() << getOutputNs().dbName().toString() << ".tmp.agg_out." << UUID::gen());

    _timeseriesViewCreated = false;
    validateTimeseries();
    // Save the original collection options and index specs so we can check they didn't change
    // during computation.
    _originalOutOptions =
        // The uuid field is considered an option, but cannot be passed to createCollection.
        pExpCtx->mongoProcessInterface->getCollectionOptions(pExpCtx->opCtx, outputNs)
            .removeField("uuid");
    _originalIndexes = pExpCtx->mongoProcessInterface->getIndexSpecs(
        pExpCtx->opCtx, outputNs, false /* includeBuildUUIDs */);

    // Check if it's capped to make sure we have a chance of succeeding before we do all the work.
    // If the collection becomes capped during processing, the collection options will have changed,
    // and the $out will fail.
    uassert(17152,
            "namespace '{}' is capped so it can't be used for {}"_format(
                outputNs.toStringForErrorMsg(), kStageName),
            _originalOutOptions["capped"].eoo());

    {
        BSONObjBuilder cmd;
        cmd << "create" << _tempNs.coll();
        cmd << "temp" << true;
        cmd.appendElementsUnique(_originalOutOptions);
        if (_timeseries) {
            cmd << DocumentSourceOutSpec::kTimeseriesFieldName << _timeseries->toBSON();
            pExpCtx->mongoProcessInterface->createTimeseries(
                pExpCtx->opCtx, _tempNs, cmd.done(), false);
        } else {
            pExpCtx->mongoProcessInterface->createCollection(
                pExpCtx->opCtx, _tempNs.dbName(), cmd.done());
        }
    }

    // After creating the tmp collection we should update '_tempNs' to represent the buckets
    // collection if the collection is time-series.
    _tempNs = makeBucketNsIfTimeseries(_tempNs);

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &outWaitAfterTempCollectionCreation,
        pExpCtx->opCtx,
        "outWaitAfterTempCollectionCreation",
        []() {
            LOGV2(20901,
                  "Hanging aggregation due to 'outWaitAfterTempCollectionCreation' failpoint");
        });
    if (_originalIndexes.empty()) {
        return;
    }

    // Copy the indexes of the output collection to the temp collection.
    try {
        std::vector<BSONObj> tempNsIndexes = {std::begin(_originalIndexes),
                                              std::end(_originalIndexes)};
        pExpCtx->mongoProcessInterface->createIndexesOnEmptyCollection(
            pExpCtx->opCtx, _tempNs, tempNsIndexes);
    } catch (DBException& ex) {
        ex.addContext("Copying indexes for $out failed");
        throw;
    }
}

void DocumentSourceOut::finalize() {
    DocumentSourceWriteBlock writeBlock(pExpCtx->opCtx);

    // If the collection is timeseries, must rename to the "real" buckets collection
    const NamespaceString& outputNs = makeBucketNsIfTimeseries(getOutputNs());

    pExpCtx->mongoProcessInterface->renameIfOptionsAndIndexesHaveNotChanged(
        pExpCtx->opCtx,
        _tempNs,
        outputNs,
        true /* dropTarget */,
        false /* stayTemp */,
        _timeseries ? true : false /* allowBuckets */,
        _originalOutOptions,
        _originalIndexes);

    // The rename succeeded, so the temp collection no longer exists.
    _tempNs = {};

    // If the collection is timeseries, try to create the view.
    if (_timeseries) {
        BSONObjBuilder cmd;
        cmd << DocumentSourceOutSpec::kTimeseriesFieldName << _timeseries->toBSON();
        pExpCtx->mongoProcessInterface->createTimeseries(
            pExpCtx->opCtx, getOutputNs(), cmd.done(), true);
    }

    // Creating the view succeeded, so the boolean should be set to true.
    _timeseriesViewCreated = true;
}

boost::intrusive_ptr<DocumentSource> DocumentSourceOut::create(
    NamespaceString outputNs,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<TimeseriesOptions> timeseries) {
    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            "{} cannot be used in a transaction"_format(kStageName),
            !expCtx->opCtx->inMultiDocumentTransaction());

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
    auto outSpec = parseOutSpecAndResolveTargetNamespace(elem, expCtx->ns.dbName());
    NamespaceString targetNss = NamespaceStringUtil::parseNamespaceFromRequest(
        expCtx->ns.dbName().tenantId(), outSpec.getDb(), outSpec.getColl());
    return create(std::move(targetNss), expCtx, std::move(outSpec.getTimeseries()));
}

Value DocumentSourceOut::serialize(SerializationOptions opts) const {
    BSONObjBuilder bob;
    DocumentSourceOutSpec spec;
    spec.setDb(_outputNs.dbName().db());
    spec.setColl(_outputNs.coll());
    spec.setTimeseries(_timeseries);
    spec.serialize(&bob, opts);
    return Value(Document{{kStageName, bob.done()}});
}

void DocumentSourceOut::waitWhileFailPointEnabled() {
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangWhileBuildingDocumentSourceOutBatch,
        pExpCtx->opCtx,
        "hangWhileBuildingDocumentSourceOutBatch",
        []() {
            LOGV2(
                20902,
                "Hanging aggregation due to  'hangWhileBuildingDocumentSourceOutBatch' failpoint");
        });
}

}  // namespace mongo
