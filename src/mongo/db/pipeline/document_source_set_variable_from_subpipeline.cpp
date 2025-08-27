/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_set_variable_from_subpipeline.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_set_variable_from_subpipeline_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

using boost::intrusive_ptr;

constexpr StringData DocumentSourceSetVariableFromSubPipeline::kStageName;

REGISTER_INTERNAL_DOCUMENT_SOURCE(setVariableFromSubPipeline,
                                  LiteParsedDocumentSourceDefault::parse,
                                  DocumentSourceSetVariableFromSubPipeline::createFromBson,
                                  true);
ALLOCATE_DOCUMENT_SOURCE_ID(setVariableFromSubPipeline,
                            DocumentSourceSetVariableFromSubPipeline::id)

Value DocumentSourceSetVariableFromSubPipeline::serialize(const SerializationOptions& opts) const {
    const auto var = "$$" + Variables::getBuiltinVariableName(_variableID);
    SetVariableFromSubPipelineSpec spec;
    tassert(625298, "SubPipeline cannot be null during serialization", _subPipeline);
    spec.setSetVariable(opts.serializeIdentifier(var));
    spec.setPipeline(_subPipeline->serializeToBson(opts));
    return Value(DOC(getSourceName() << spec.toBSON()));
}

DepsTracker::State DocumentSourceSetVariableFromSubPipeline::getDependencies(
    DepsTracker* deps) const {
    return DepsTracker::State::NOT_SUPPORTED;
}

void DocumentSourceSetVariableFromSubPipeline::addVariableRefs(
    std::set<Variables::Id>* refs) const {
    refs->insert(_variableID);
    _subPipeline->addVariableRefs(refs);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceSetVariableFromSubPipeline::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(
        ErrorCodes::FailedToParse,
        str::stream()
            << "the $setVariableFromSubPipeline stage specification must be an object, but found "
            << typeName(elem.type()),
        elem.type() == BSONType::object);

    auto spec =
        SetVariableFromSubPipelineSpec::parse(elem.embeddedObject(), IDLParserContext(kStageName));
    const auto searchMetaStr = "$$" + Variables::getBuiltinVariableName(Variables::kSearchMetaId);
    uassert(
        625291,
        str::stream() << "SetVariableFromSubPipeline only allows setting $$SEARCH_META variable,  "
                      << spec.getSetVariable() << " is not allowed.",
        spec.getSetVariable() == searchMetaStr);

    std::unique_ptr<Pipeline> pipeline = Pipeline::parse(
        spec.getPipeline(),
        makeCopyForSubPipelineFromExpressionContext(expCtx, expCtx->getNamespaceString()));

    return DocumentSourceSetVariableFromSubPipeline::create(
        expCtx, std::move(pipeline), Variables::kSearchMetaId);
}

intrusive_ptr<DocumentSourceSetVariableFromSubPipeline>
DocumentSourceSetVariableFromSubPipeline::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<Pipeline> subpipeline,
    Variables::Id varID) {
    uassert(625290,
            str::stream()
                << "SetVariableFromSubPipeline only allows setting $$SEARCH_META variable,  '$$"
                << Variables::getBuiltinVariableName(varID) << "' is not allowed.",
            !Variables::isUserDefinedVariable(varID) && varID == Variables::kSearchMetaId);
    return intrusive_ptr<DocumentSourceSetVariableFromSubPipeline>(
        new DocumentSourceSetVariableFromSubPipeline(expCtx, std::move(subpipeline), varID));
};

void DocumentSourceSetVariableFromSubPipeline::addSubPipelineInitialSource(
    boost::intrusive_ptr<DocumentSource> source) {
    _subPipeline->addInitialSource(std::move(source));
}


void DocumentSourceSetVariableFromSubPipeline::detachSourceFromOperationContext() {
    // We have an execution pipeline we're going to execute across multiple commands, so we need to
    // detach it from the operation context when it goes out of scope.
    if (_sharedState->_subExecPipeline) {
        _sharedState->_subExecPipeline->detachFromOperationContext();
    }
    // Some methods require pipeline to have a valid operation context. Normally, a pipeline and the
    // corresponding execution pipeline share the same expression context containing a pointer to
    // the operation context, but it might not be the case anymore when a pipeline is cloned with
    // another expression context.
    if (_subPipeline) {
        _subPipeline->detachFromOperationContext();
    }
}

void DocumentSourceSetVariableFromSubPipeline::reattachSourceToOperationContext(
    OperationContext* opCtx) {
    // We have an execution pipeline we're going to execute across multiple commands, so we need to
    // propagate the new operation context to the pipeline stages.
    if (_sharedState->_subExecPipeline) {
        _sharedState->_subExecPipeline->reattachToOperationContext(opCtx);
    }
    // Some methods require pipeline to have a valid operation context. Normally, a pipeline and the
    // corresponding execution pipeline share the same expression context containing a pointer to
    // the operation context, but it might not be the case anymore when a pipeline is cloned with
    // another expression context.
    if (_subPipeline) {
        _subPipeline->reattachToOperationContext(opCtx);
    }
}

bool DocumentSourceSetVariableFromSubPipeline::validateSourceOperationContext(
    const OperationContext* opCtx) const {
    return getExpCtx()->getOperationContext() == opCtx &&
        (!_subPipeline || _subPipeline->validateOperationContext(opCtx));
}

}  // namespace mongo
