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

#include "mongo/s/query/exec/document_source_merge_cursors.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/shard_role/resource_yielders.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

ALLOCATE_STAGE_PARAMS_ID(mergeCursors, MergeCursorsStageParams::id);

REGISTER_INTERNAL_DOCUMENT_SOURCE(mergeCursors,
                                  MergeCursorsLiteParsed::parse,
                                  DocumentSourceMergeCursors::createFromBson,
                                  true);
ALLOCATE_DOCUMENT_SOURCE_ID(mergeCursors, DocumentSourceMergeCursors::id)

constexpr StringData DocumentSourceMergeCursors::kStageName;

DocumentSourceMergeCursors::DocumentSourceMergeCursors(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, AsyncResultsMergerParams armParams)
    : DocumentSource(kStageName, expCtx), _armParams(std::move(armParams)) {}

DocumentSourceMergeCursors::~DocumentSourceMergeCursors() {
    if (_ownCursors) {
        tassert(10561404, "_armParams must be set", _armParams);
        BSONObj armParamsForLog = _armParams->toBSON();
        if (getExpCtx()->getOperationContext()) {
            // Method call 'populateMerger()->kill()' needs a valid operation context.
            try {
                populateMerger()->kill(getExpCtx()->getOperationContext());
            } catch (const std::exception& ex) {
                // When something goes wrong we might leak remote cursors, but we still want to keep
                // this process running, therefore we do not let the exception out of the
                // destructor.
                LOGV2_ERROR(10561405,
                            "remote cursors might be leaked due to an unexpected error",
                            "armParams"_attr = armParamsForLog,
                            "error"_attr = ex.what());
            }
        } else {
            // We expect this object will be destroyed while still attached to the original
            // operation context, but we still want to keep this process running, therefore no
            // assertions here.
            LOGV2_ERROR(
                10561406,
                "destroying a detached $mergeCursors stage at this point could leak remote cursors",
                "armParams"_attr = armParamsForLog);
        }
    }
}

std::shared_ptr<BlockingResultsMerger>& DocumentSourceMergeCursors::populateMerger() {
    tassert(9535001, "_blockingResultsMerger must not yet be set", !_blockingResultsMerger);
    tassert(9535002, "_armParams must be set", _armParams);

    auto* opCtx = getExpCtx()->getOperationContext();
    const auto& resourceYielder = ResourceYielderFactory::get(*opCtx->getService());
    _blockingResultsMerger = std::make_shared<BlockingResultsMerger>(
        opCtx,
        std::move(*_armParams),
        getExpCtx()->getMongoProcessInterface()->taskExecutor,
        // Assumes this is only called from the 'aggregate' or 'getMore' commands.  The code which
        // relies on this parameter does not distinguish/care about the difference so we simply
        // always pass 'aggregate'.
        resourceYielder ? resourceYielder->make(opCtx, "aggregate"_sd) : nullptr);
    _armParams = boost::none;

    // '_blockingResultsMerger' now owns the cursors.
    _ownCursors = false;

    // Returning the created 'BlockingResultsMerger' instance for convenience.
    return _blockingResultsMerger;
}

std::unique_ptr<RouterStageMerge> DocumentSourceMergeCursors::convertToRouterStage() {
    tassert(9535003, "Expected conversion to happen before execution", _armParams);
    auto result =
        std::make_unique<RouterStageMerge>(getExpCtx()->getOperationContext(),
                                           getExpCtx()->getMongoProcessInterface()->taskExecutor,
                                           std::move(*_armParams));
    _armParams = boost::none;
    _ownCursors = false;
    return result;
}

Value DocumentSourceMergeCursors::serialize(const SerializationOptions& opts) const {
    // This method is the only reason 'DocumentSourceMergeCursors' needs '_blockingResultsMerger'.
    // We cannot cache '_armParams->toBSON()', because remotes can change during the execution in
    // case of change streams.
    if (_blockingResultsMerger) {
        return Value(Document{
            {kStageName, _blockingResultsMerger->asyncResultsMergerParams().toBSON(opts)}});
    }
    tassert(9535004, "_armParams must be set", _armParams);
    return Value(Document{{kStageName, _armParams->toBSON(opts)}});
}

boost::intrusive_ptr<DocumentSource> DocumentSourceMergeCursors::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(17026,
            "$mergeCursors stage expected an object as argument",
            elem.type() == BSONType::object);
    auto armParams = AsyncResultsMergerParams::parseOwned(
        elem.embeddedObject().getOwned(),
        IDLParserContext(kStageName,
                         auth::ValidatedTenancyScope::get(expCtx->getOperationContext()),
                         expCtx->getNamespaceString().tenantId(),
                         SerializationContext::stateDefault()));
    return new DocumentSourceMergeCursors(expCtx, std::move(armParams));
}

boost::intrusive_ptr<DocumentSourceMergeCursors> DocumentSourceMergeCursors::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, AsyncResultsMergerParams params) {
    return new DocumentSourceMergeCursors(expCtx, std::move(params));
}
void DocumentSourceMergeCursors::detachSourceFromOperationContext() {
    if (_blockingResultsMerger) {
        _blockingResultsMerger->detachFromOperationContext();
    }
}

void DocumentSourceMergeCursors::reattachSourceToOperationContext(OperationContext* opCtx) {
    if (_blockingResultsMerger) {
        _blockingResultsMerger->reattachToOperationContext(opCtx);
    }
}

boost::optional<NamespaceString> DocumentSourceMergeCursors::getAsyncResultMergerParamsNss_forTest()
    const {
    if (_armParams) {
        return _armParams->getNss();
    }
    return boost::none;
}

}  // namespace mongo
