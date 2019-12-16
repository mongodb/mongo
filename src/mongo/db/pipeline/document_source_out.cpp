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

#include "mongo/db/background.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"
#include "mongo/util/uuid.h"

namespace mongo {
using namespace fmt::literals;

MONGO_FAIL_POINT_DEFINE(hangWhileBuildingDocumentSourceOutBatch);
MONGO_FAIL_POINT_DEFINE(outWaitAfterTempCollectionCreation);
REGISTER_DOCUMENT_SOURCE(out,
                         DocumentSourceOut::LiteParsed::parse,
                         DocumentSourceOut::createFromBson);
REGISTER_DOCUMENT_SOURCE(internalOutToDifferentDB,
                         DocumentSourceOut::LiteParsed::parseToDifferentDB,
                         DocumentSourceOut::createFromBsonToDifferentDB);

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

            pExpCtx->mongoProcessInterface->dropCollection(cleanupOpCtx.get(), _tempNs);
        });
}

std::unique_ptr<DocumentSourceOut::LiteParsed> DocumentSourceOut::LiteParsed::parseToDifferentDB(
    const AggregationRequest& request, const BSONElement& spec) {

    auto specObj = spec.Obj();
    auto dbElem = specObj["db"];
    auto collElem = specObj["coll"];
    uassert(16994,
            str::stream() << kStageName << " must have db and coll string arguments",
            dbElem.type() == BSONType::String && collElem.type() == BSONType::String);
    NamespaceString targetNss{dbElem.String(), collElem.String()};
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

std::unique_ptr<DocumentSourceOut::LiteParsed> DocumentSourceOut::LiteParsed::parse(
    const AggregationRequest& request, const BSONElement& spec) {

    uassert(16990,
            "{} only supports a string argument, but found {}"_format(kStageName,
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

    const auto& outputNs = getOutputNs();
    // We will write all results into a temporary collection, then rename the temporary collection
    // to be the target collection once we are done.
    _tempNs = NamespaceString(str::stream() << outputNs.db() << ".tmp.agg_out." << UUID::gen());

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
            "namespace '{}' is capped so it can't be used for {}"_format(outputNs.ns(), kStageName),
            _originalOutOptions["capped"].eoo());

    // Create temp collection, copying options from the existing output collection if any.
    {
        BSONObjBuilder cmd;
        cmd << "create" << _tempNs.coll();
        cmd << "temp" << true;
        cmd.appendElementsUnique(_originalOutOptions);

        pExpCtx->mongoProcessInterface->createCollection(
            pExpCtx->opCtx, _tempNs.db().toString(), cmd.done());
    }

    // Disallows drops and renames on this namespace.
    BackgroundOperation backgroundOp(_tempNs.ns());
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &outWaitAfterTempCollectionCreation,
        pExpCtx->opCtx,
        "outWaitAfterTempCollectionCreation",
        []() {
            log() << "Hanging aggregation due to 'outWaitAfterTempCollectionCreation' "
                  << "failpoint";
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
    return createAndAllowDifferentDB(outputNs, expCtx);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceOut::createAndAllowDifferentDB(
    NamespaceString outputNs, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            "{} cannot be used in a transaction"_format(kStageName),
            !expCtx->inMultiDocumentTransaction);

    uassert(ErrorCodes::InvalidNamespace,
            "Invalid {} target namespace, {}"_format(kStageName, outputNs.ns()),
            outputNs.isValid());

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
    uassert(31278,
            "{} only supports a string argument, but found {}"_format(kStageName,
                                                                      typeName(elem.type())),
            elem.type() == BSONType::String);
    return create({expCtx->ns.db(), elem.str()}, expCtx);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceOut::createFromBsonToDifferentDB(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {

    auto nsObj = elem.Obj();
    return createAndAllowDifferentDB(NamespaceString(nsObj["db"].String(), nsObj["coll"].String()),
                                     expCtx);
}
Value DocumentSourceOut::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    return _toDifferentDB
        ? Value(DOC(getSourceName() << DOC("db" << _outputNs.db() << "coll" << _outputNs.coll())))
        : Value(DOC(getSourceName() << _outputNs.coll()));
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
