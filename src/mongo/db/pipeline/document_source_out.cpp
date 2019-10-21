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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_out.h"

#include <fmt/format.h>

#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/log.h"

namespace mongo {
using namespace fmt::literals;

static AtomicWord<unsigned> aggOutCounter;

MONGO_FAIL_POINT_DEFINE(hangWhileBuildingDocumentSourceOutBatch);
REGISTER_DOCUMENT_SOURCE(out,
                         DocumentSourceOut::LiteParsed::parse,
                         DocumentSourceOut::createFromBson);

DocumentSourceOut::~DocumentSourceOut() {
    DESTRUCTOR_GUARD(
        // Make sure we drop the temp collection if anything goes wrong. Errors are ignored
        // here because nothing can be done about them. Additionally, if this fails and the
        // collection is left behind, it will be cleaned up next time the server is started.
        if (_tempNs.size()) {
            auto cleanupClient =
                pExpCtx->opCtx->getServiceContext()->makeClient("$out_replace_coll_cleanup");
            AlternativeClientRegion acr(cleanupClient);
            // Create a new operation context so that any interrupts on the current operation will
            // not affect the dropCollection operation below.
            auto cleanupOpCtx = cc().makeOperationContext();

            DocumentSourceWriteBlock writeBlock(cleanupOpCtx.get());

            // Reset the operation context back to original once dropCollection is done.
            ON_BLOCK_EXIT(
                [this] { pExpCtx->mongoProcessInterface->setOperationContext(pExpCtx->opCtx); });

            pExpCtx->mongoProcessInterface->setOperationContext(cleanupOpCtx.get());
            pExpCtx->mongoProcessInterface->directClient()->dropCollection(_tempNs.ns());
        });
}

std::unique_ptr<DocumentSourceOut::LiteParsed> DocumentSourceOut::LiteParsed::parse(
    const AggregationRequest& request, const BSONElement& spec) {

    uassert(ErrorCodes::TypeMismatch,
            "{} stage requires a string argument, but found {}"_format(kStageName,
                                                                       typeName(spec.type())),
            spec.type() == BSONType::String);

    NamespaceString targetNss{request.getNamespaceString().db(), spec.valueStringData()};
    uassert(ErrorCodes::InvalidNamespace,
            "Invalid {} target namespace, {}"_format(kStageName, targetNss.ns()),
            targetNss.isValid());

    ActionSet actions{ActionType::insert, ActionType::remove};
    if (request.shouldBypassDocumentValidation()) {
        actions.addAction(ActionType::bypassDocumentValidation);
    }

    PrivilegeVector privileges{Privilege(ResourcePattern::forExactNamespace(targetNss), actions)};

    return std::make_unique<DocumentSourceOut::LiteParsed>(std::move(targetNss),
                                                           std::move(privileges));
}

void DocumentSourceOut::initialize() {
    DocumentSourceWriteBlock writeBlock(pExpCtx->opCtx);

    DBClientBase* conn = pExpCtx->mongoProcessInterface->directClient();

    const auto& outputNs = getOutputNs();
    _tempNs = NamespaceString(str::stream()
                              << outputNs.db() << ".tmp.agg_out." << aggOutCounter.addAndFetch(1));

    // Save the original collection options and index specs so we can check they didn't change
    // during computation.
    _originalOutOptions = pExpCtx->mongoProcessInterface->getCollectionOptions(outputNs);
    _originalIndexes = conn->getIndexSpecs(outputNs);

    // Check if it's capped to make sure we have a chance of succeeding before we do all the work.
    // If the collection becomes capped during processing, the collection options will have changed,
    // and the $out will fail.
    uassert(17152,
            "namespace '{}' is capped so it can't be used for {}"_format(outputNs.ns(), kStageName),
            _originalOutOptions["capped"].eoo());

    // We will write all results into a temporary collection, then rename the temporary
    // collection to be the target collection once we are done.
    _tempNs = NamespaceString(str::stream()
                              << outputNs.db() << ".tmp.agg_out." << aggOutCounter.addAndFetch(1));

    // Create temp collection, copying options from the existing output collection if any.
    {
        BSONObjBuilder cmd;
        cmd << "create" << _tempNs.coll();
        cmd << "temp" << true;
        cmd.appendElementsUnique(_originalOutOptions);

        BSONObj info;
        uassert(16994,
                "failed to create temporary {} collection '{}': {}"_format(
                    kStageName, _tempNs.ns(), getStatusFromCommandResult(info).reason()),
                conn->runCommand(outputNs.db().toString(), cmd.done(), info));
    }

    if (_originalIndexes.empty()) {
        return;
    }

    // Copy the indexes of the output collection to the temp collection.
    try {
        std::vector<BSONObj> tempNsIndexes = {std::begin(_originalIndexes),
                                              std::end(_originalIndexes)};
        conn->createIndexes(_tempNs.ns(), tempNsIndexes);
    } catch (DBException& ex) {
        ex.addContext("Copying indexes for $out failed");
        throw;
    }
}

void DocumentSourceOut::finalize() {
    DocumentSourceWriteBlock writeBlock(pExpCtx->opCtx);

    const auto& outputNs = getOutputNs();
    auto renameCommandObj =
        BSON("renameCollection" << _tempNs.ns() << "to" << outputNs.ns() << "dropTarget" << true);

    pExpCtx->mongoProcessInterface->renameIfOptionsAndIndexesHaveNotChanged(
        pExpCtx->opCtx, renameCommandObj, outputNs, _originalOutOptions, _originalIndexes);

    // The rename succeeded, so the temp collection no longer exists.
    _tempNs = {};
}

boost::intrusive_ptr<DocumentSource> DocumentSourceOut::create(
    NamespaceString outputNs, const boost::intrusive_ptr<ExpressionContext>& expCtx) {

    // TODO (SERVER-36832): Allow this combination.
    uassert(50939,
            "{} is not supported when the output collection is in a different "
            "database"_format(kStageName),
            outputNs.db() == expCtx->ns.db());

    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            "{} cannot be used in a transaction"_format(kStageName),
            !expCtx->inMultiDocumentTransaction);

    auto readConcernLevel = repl::ReadConcernArgs::get(expCtx->opCtx).getLevel();
    uassert(ErrorCodes::InvalidOptions,
            "{} cannot be used with a 'linearizable' read concern level"_format(kStageName),
            readConcernLevel != repl::ReadConcernLevel::kLinearizableReadConcern);

    uassert(ErrorCodes::InvalidNamespace,
            "Invalid {} target namespace, {}"_format(kStageName, outputNs.ns()),
            outputNs.isValid());

    uassert(17017,
            "{} is not supported to an existing *sharded* output collection"_format(kStageName),
            !expCtx->mongoProcessInterface->isSharded(expCtx->opCtx, outputNs));

    uassert(17385,
            "Can't {} to special collection: {}"_format(kStageName, outputNs.coll()),
            !outputNs.isSystem());

    uassert(31321,
            "Can't {} to internal database: {}"_format(kStageName, outputNs.db()),
            !outputNs.isOnInternalDb());

    return new DocumentSourceOut(std::move(outputNs), expCtx);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceOut::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(16990,
            "{} only supports a string argument, but found {}"_format(kStageName,
                                                                      typeName(elem.type())),
            elem.type() == BSONType::String);

    return create({expCtx->ns.db(), elem.str()}, expCtx);
}

Value DocumentSourceOut::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    massert(17000,
            "{} shouldn't have different db than input"_format(kStageName),
            _outputNs.db() == pExpCtx->ns.db());

    return Value(DOC(getSourceName() << _outputNs.coll()));
}

void DocumentSourceOut::waitWhileFailPointEnabled() {
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangWhileBuildingDocumentSourceOutBatch,
        pExpCtx->opCtx,
        "hangWhileBuildingDocumentSourceOutBatch",
        []() {
            log() << "Hanging aggregation due to 'hangWhileBuildingDocumentSourceOutBatch' "
                  << "failpoint";
        });
}

}  // namespace mongo
