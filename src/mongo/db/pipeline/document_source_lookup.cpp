/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_lookup.h"

#include "mongo/base/init.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using boost::intrusive_ptr;
using std::vector;

namespace {
std::string pipelineToString(const vector<BSONObj>& pipeline) {
    StringBuilder sb;
    sb << "[";

    auto first = true;
    for (auto& stageSpec : pipeline) {
        if (!first) {
            sb << ", ";
        } else {
            first = false;
        }
        sb << stageSpec;
    }
    sb << "]";
    return sb.str();
}
}  // namespace

DocumentSourceLookUp::DocumentSourceLookUp(NamespaceString fromNs,
                                           std::string as,
                                           const boost::intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSourceNeedsMongod(pExpCtx),
      _fromNs(std::move(fromNs)),
      _as(std::move(as)),
      _variablesParseState(_variables.useIdGenerator()) {
    const auto& resolvedNamespace = pExpCtx->getResolvedNamespace(_fromNs);
    _resolvedNs = resolvedNamespace.ns;
    _resolvedPipeline = resolvedNamespace.pipeline;
    _fromExpCtx = pExpCtx->copyWith(_resolvedNs);
}

DocumentSourceLookUp::DocumentSourceLookUp(NamespaceString fromNs,
                                           std::string as,
                                           std::string localField,
                                           std::string foreignField,
                                           const boost::intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSourceLookUp(fromNs, as, pExpCtx) {
    _localField = std::move(localField);
    _foreignField = std::move(foreignField);
    // We append an additional BSONObj to '_resolvedPipeline' as a placeholder for the $match stage
    // we'll eventually construct from the input document.
    _resolvedPipeline.reserve(_resolvedPipeline.size() + 1);
    _resolvedPipeline.push_back(BSONObj());
}

DocumentSourceLookUp::DocumentSourceLookUp(NamespaceString fromNs,
                                           std::string as,
                                           std::vector<BSONObj> pipeline,
                                           BSONObj letVariables,
                                           const boost::intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSourceLookUp(fromNs, as, pExpCtx) {
    // '_resolvedPipeline' will first be initialized by the constructor delegated to within this
    // constructor's initializer list. It will be populated with view pipeline prefix if 'fromNs'
    // represents a view. We append the user 'pipeline' to the end of '_resolvedPipeline' to ensure
    // any view prefix is not overwritten.
    _resolvedPipeline.insert(_resolvedPipeline.end(), pipeline.begin(), pipeline.end());

    _userPipeline = std::move(pipeline);

    for (auto&& varElem : letVariables) {
        const auto varName = varElem.fieldNameStringData();
        Variables::uassertValidNameForUserWrite(varName);

        _letVariables.emplace_back(
            varName.toString(),
            Expression::parseOperand(pExpCtx, varElem, pExpCtx->variablesParseState),
            _variablesParseState.defineVariable(varName));
    }
}

std::unique_ptr<DocumentSourceLookUp::LiteParsed> DocumentSourceLookUp::LiteParsed::parse(
    const AggregationRequest& request, const BSONElement& spec) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "the $lookup stage specification must be an object, but found "
                          << typeName(spec.type()),
            spec.type() == BSONType::Object);

    auto specObj = spec.Obj();
    auto fromElement = specObj["from"];
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "missing 'from' option to $lookup stage specification: " << specObj,
            fromElement);
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "'from' option to $lookup must be a string, but was type "
                          << typeName(specObj["from"].type()),
            fromElement.type() == BSONType::String);

    NamespaceString fromNss(request.getNamespaceString().db(), fromElement.valueStringData());
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "invalid $lookup namespace: " << fromNss.ns(),
            fromNss.isValid());

    stdx::unordered_set<NamespaceString> foreignNssSet;

    // Recursively lite parse the nested pipeline, if one exists.
    auto pipelineElem = specObj["pipeline"];
    boost::optional<LiteParsedPipeline> liteParsedPipeline;
    if (pipelineElem) {
        auto pipeline = uassertStatusOK(AggregationRequest::parsePipelineFromBSON(pipelineElem));
        AggregationRequest foreignAggReq(fromNss, std::move(pipeline));
        liteParsedPipeline = LiteParsedPipeline(foreignAggReq);

        auto pipelineInvolvedNamespaces = liteParsedPipeline->getInvolvedNamespaces();
        foreignNssSet.insert(pipelineInvolvedNamespaces.begin(), pipelineInvolvedNamespaces.end());
    }

    foreignNssSet.insert(fromNss);

    return stdx::make_unique<DocumentSourceLookUp::LiteParsed>(
        std::move(fromNss), std::move(foreignNssSet), std::move(liteParsedPipeline));
}

REGISTER_DOCUMENT_SOURCE(lookup,
                         DocumentSourceLookUp::LiteParsed::parse,
                         DocumentSourceLookUp::createFromBson);

const char* DocumentSourceLookUp::getSourceName() const {
    return "$lookup";
}

namespace {

/**
 * Constructs a query of the following shape:
 *  {$or: [
 *    {'fieldName': {$eq: 'values[0]'}},
 *    {'fieldName': {$eq: 'values[1]'}},
 *    ...
 *  ]}
 */
BSONObj buildEqualityOrQuery(const std::string& fieldName, const BSONArray& values) {
    BSONObjBuilder orBuilder;
    {
        BSONArrayBuilder orPredicatesBuilder(orBuilder.subarrayStart("$or"));
        for (auto&& value : values) {
            orPredicatesBuilder.append(BSON(fieldName << BSON("$eq" << value)));
        }
    }
    return orBuilder.obj();
}

}  // namespace

DocumentSource::GetNextResult DocumentSourceLookUp::getNext() {
    pExpCtx->checkForInterrupt();

    if (_unwindSrc) {
        return unwindResult();
    }

    auto nextInput = pSource->getNext();
    if (!nextInput.isAdvanced()) {
        return nextInput;
    }

    auto inputDoc = nextInput.releaseDocument();
    copyVariablesToExpCtx(_variables, _variablesParseState, _fromExpCtx.get());
    resolveLetVariables(inputDoc, &_fromExpCtx->variables);

    // If we have not absorbed a $unwind, we cannot absorb a $match. If we have absorbed a $unwind,
    // '_unwindSrc' would be non-null, and we would not have made it here.
    invariant(!_matchSrc);

    if (!wasConstructedWithPipelineSyntax()) {
        auto matchStage =
            makeMatchStageFromInput(inputDoc, *_localField, _foreignField->fullPath(), BSONObj());
        // We've already allocated space for the trailing $match stage in '_resolvedPipeline'.
        _resolvedPipeline.back() = matchStage;
    }

    auto pipeline = uassertStatusOK(_mongod->makePipeline(_resolvedPipeline, _fromExpCtx));

    std::vector<Value> results;
    int objsize = 0;

    while (auto result = pipeline->getNext()) {
        objsize += result->getApproximateSize();
        uassert(4568,
                str::stream() << "Total size of documents in " << _fromNs.coll()
                              << " matching pipeline "
                              << getUserPipelineDefinition()
                              << " exceeds maximum document size",
                objsize <= BSONObjMaxInternalSize);
        results.emplace_back(std::move(*result));
    }

    MutableDocument output(std::move(inputDoc));
    output.setNestedField(_as, Value(std::move(results)));
    return output.freeze();
}

DocumentSource::GetModPathsReturn DocumentSourceLookUp::getModifiedPaths() const {
    std::set<std::string> modifiedPaths{_as.fullPath()};
    if (_unwindSrc) {
        auto pathsModifiedByUnwind = _unwindSrc->getModifiedPaths();
        invariant(pathsModifiedByUnwind.type == GetModPathsReturn::Type::kFiniteSet);
        modifiedPaths.insert(pathsModifiedByUnwind.paths.begin(),
                             pathsModifiedByUnwind.paths.end());
    }
    return {GetModPathsReturn::Type::kFiniteSet, std::move(modifiedPaths), {}};
}

Pipeline::SourceContainer::iterator DocumentSourceLookUp::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    auto nextUnwind = dynamic_cast<DocumentSourceUnwind*>((*std::next(itr)).get());

    // If we are not already handling an $unwind stage internally, we can combine with the
    // following $unwind stage.
    if (nextUnwind && !_unwindSrc && nextUnwind->getUnwindPath() == _as.fullPath()) {
        _unwindSrc = std::move(nextUnwind);
        container->erase(std::next(itr));
        return itr;
    }

    // Attempt to internalize any predicates of a $match upon the "_as" field.
    auto nextMatch = dynamic_cast<DocumentSourceMatch*>((*std::next(itr)).get());

    if (!nextMatch) {
        return std::next(itr);
    }

    if (!_unwindSrc || _unwindSrc->indexPath() || _unwindSrc->preserveNullAndEmptyArrays()) {
        // We must be unwinding our result to internalize a $match. For example, consider the
        // following pipeline:
        //
        // Input: {_id: 0}
        // Foreign Collection: {a: 0, b: 0}, {a: 0, b: 5}
        // Pipeline:
        //   {$lookup: {localField: "_id", foreignField: "a", as: "foo"}}
        //   {$match: {'foo.b': {$gt: 0}}}
        // Output: {_id: 0, foo: [{a: 0, b: 0}, {a: 0, b: 5}]}
        //
        // If we executed {b: {$gt: 0}} as part of our $lookup, our output would instead be:
        // {_id: 0, foo: [{a: 0, b: 5}]}
        //
        // However, if we are already unwinding 'foo', then we can move the $match inside, since it
        // will have the same effect as filtering the unwound results, that is, the output will be:
        // {_id: 0, foo: {a: 0, b: 5}}
        //
        // Note that we cannot absorb a $match if the absorbed $unwind has
        // "preserveNullAndEmptyArrays" set to true, for the following reason: A document that had
        // an empty output array from $lookup would be preserved by the $unwind, but could be
        // removed by the $match. However, if we absorb the $match into the $lookup, our joined
        // query inside the $lookup will output an empty array, which $unwind will then preserve.
        // Thus, depending on the optimization, the user would see a different output.
        //
        // In addition, we must avoid internalizing a $match if an absorbed $unwind has an
        // "includeArrayIndex" option, since the $match will alter the indices of the returned
        // values.
        return std::next(itr);
    }

    auto outputPath = _as.fullPath();

    // Since $match splitting is handled in a generic way, we expect to have already swapped
    // portions of the $match that do not depend on the 'as' path or on an internalized $unwind's
    // index path before ourselves. But due to the early return above, we know there is no
    // internalized $unwind with an index path.
    //
    // Therefore, 'nextMatch' should only depend on the 'as' path. We now try to absorb the match on
    // the 'as' path in order to push down these predicates into the foreign collection.
    bool isMatchOnlyOnAs = true;
    auto computeWhetherMatchOnAs = [&isMatchOnlyOnAs, &outputPath](MatchExpression* expression,
                                                                   std::string path) -> void {
        // If 'expression' is the child of a $elemMatch, we cannot internalize the $match. For
        // example, {b: {$elemMatch: {$gt: 1, $lt: 4}}}, where "b" is our "_as" field. This is
        // because there's no way to modify the expression to be a match just on 'b'--we cannot
        // change the path to an empty string, or remove the node entirely.
        if (expression->matchType() == MatchExpression::ELEM_MATCH_VALUE ||
            expression->matchType() == MatchExpression::ELEM_MATCH_OBJECT) {
            isMatchOnlyOnAs = false;
        }
        if (expression->numChildren() == 0) {
            // 'expression' is a leaf node; examine the path. It is important that 'outputPath'
            // not equal 'path', because we cannot change the expression {b: {$eq: 3}}, where
            // 'path' is 'b', to be a match on a subfield, since no subfield exists.
            isMatchOnlyOnAs = isMatchOnlyOnAs && expression::isPathPrefixOf(outputPath, path);
        }
    };

    expression::mapOver(nextMatch->getMatchExpression(), computeWhetherMatchOnAs);

    if (!isMatchOnlyOnAs) {
        // "nextMatch" does not contain any predicates that can be absorbed into this stage.
        return std::next(itr);
    }

    // We can internalize the $match.
    if (!_matchSrc) {
        _matchSrc = nextMatch;
    } else {
        // We have already absorbed a $match. We need to join it with 'dependent'.
        _matchSrc->joinMatchWith(nextMatch);
    }

    // Remove the original $match. There may be further optimization between this $lookup and the
    // new neighbor, so we return an iterator pointing to ourself.
    container->erase(std::next(itr));

    // We have internalized a $match, but have not yet computed the descended $match that should
    // be applied to our queries.
    _additionalFilter = DocumentSourceMatch::descendMatchOnPath(
                            _matchSrc->getMatchExpression(), _as.fullPath(), pExpCtx)
                            ->getQuery()
                            .getOwned();

    if (wasConstructedWithPipelineSyntax()) {
        auto matchObj = BSON("$match" << *_additionalFilter);
        _resolvedPipeline.push_back(matchObj);
    }

    return itr;
}

std::string DocumentSourceLookUp::getUserPipelineDefinition() {
    if (wasConstructedWithPipelineSyntax()) {
        return pipelineToString(_userPipeline);
    }

    return _resolvedPipeline.back().toString();
}

void DocumentSourceLookUp::doDispose() {
    if (_pipeline) {
        _pipeline->dispose(pExpCtx->opCtx);
        _pipeline.reset();
    }
}

BSONObj DocumentSourceLookUp::makeMatchStageFromInput(const Document& input,
                                                      const FieldPath& localFieldPath,
                                                      const std::string& foreignFieldName,
                                                      const BSONObj& additionalFilter) {
    // Add the 'localFieldPath' of 'input' into 'localFieldList'. If 'localFieldPath' references a
    // field with an array in its path, we may need to join on multiple values, so we add each
    // element to 'localFieldList'.
    BSONArrayBuilder arrBuilder;
    bool containsRegex = false;
    document_path_support::visitAllValuesAtPath(input, localFieldPath, [&](const Value& nextValue) {
        arrBuilder << nextValue;
        if (!containsRegex && nextValue.getType() == BSONType::RegEx) {
            containsRegex = true;
        }
    });

    if (arrBuilder.arrSize() == 0) {
        // Missing values are treated as null.
        arrBuilder << BSONNULL;
    }

    const auto localFieldListSize = arrBuilder.arrSize();
    const auto localFieldList = arrBuilder.arr();

    // We construct a query of one of the following forms, depending on the contents of
    // 'localFieldList'.
    //
    //   {$and: [{<foreignFieldName>: {$eq: <localFieldList[0]>}}, <additionalFilter>]}
    //     if 'localFieldList' contains a single element.
    //
    //   {$and: [{<foreignFieldName>: {$in: [<value>, <value>, ...]}}, <additionalFilter>]}
    //     if 'localFieldList' contains more than one element but doesn't contain any that are
    //     regular expressions.
    //
    //   {$and: [{$or: [{<foreignFieldName>: {$eq: <value>}},
    //                  {<foreignFieldName>: {$eq: <value>}}, ...]},
    //           <additionalFilter>]}
    //     if 'localFieldList' contains more than one element and it contains at least one element
    //     that is a regular expression.

    // We wrap the query in a $match so that it can be parsed into a DocumentSourceMatch when
    // constructing a pipeline to execute.
    BSONObjBuilder match;
    BSONObjBuilder query(match.subobjStart("$match"));

    BSONArrayBuilder andObj(query.subarrayStart("$and"));
    BSONObjBuilder joiningObj(andObj.subobjStart());

    if (localFieldListSize > 1) {
        // A $lookup on an array value corresponds to finding documents in the foreign collection
        // that have a value of any of the elements in the array value, rather than finding
        // documents that have a value equal to the entire array value. These semantics are
        // automatically provided to us by using the $in query operator.
        if (containsRegex) {
            // A regular expression inside the $in query operator will perform pattern matching on
            // any string values. Since we want regular expressions to only match other RegEx types,
            // we write the query as a $or of equality comparisons instead.
            BSONObj orQuery = buildEqualityOrQuery(foreignFieldName, localFieldList);
            joiningObj.appendElements(orQuery);
        } else {
            // { <foreignFieldName> : { "$in" : <localFieldList> } }
            BSONObjBuilder subObj(joiningObj.subobjStart(foreignFieldName));
            subObj << "$in" << localFieldList;
            subObj.doneFast();
        }
    } else {
        // { <foreignFieldName> : { "$eq" : <localFieldList[0]> } }
        BSONObjBuilder subObj(joiningObj.subobjStart(foreignFieldName));
        subObj << "$eq" << localFieldList[0];
        subObj.doneFast();
    }

    joiningObj.doneFast();

    BSONObjBuilder additionalFilterObj(andObj.subobjStart());
    additionalFilterObj.appendElements(additionalFilter);
    additionalFilterObj.doneFast();

    andObj.doneFast();

    query.doneFast();
    return match.obj();
}

DocumentSource::GetNextResult DocumentSourceLookUp::unwindResult() {
    const boost::optional<FieldPath> indexPath(_unwindSrc->indexPath());

    // Loop until we get a document that has at least one match.
    // Note we may return early from this loop if our source stage is exhausted or if the unwind
    // source was asked to return empty arrays and we get a document without a match.
    while (!_pipeline || !_nextValue) {
        auto nextInput = pSource->getNext();
        if (!nextInput.isAdvanced()) {
            return nextInput;
        }

        _input = nextInput.releaseDocument();

        if (!wasConstructedWithPipelineSyntax()) {
            BSONObj filter = _additionalFilter.value_or(BSONObj());
            auto matchStage =
                makeMatchStageFromInput(*_input, *_localField, _foreignField->fullPath(), filter);
            // We've already allocated space for the trailing $match stage in '_resolvedPipeline'.
            _resolvedPipeline.back() = matchStage;
        }

        if (_pipeline) {
            _pipeline->dispose(pExpCtx->opCtx);
        }

        copyVariablesToExpCtx(_variables, _variablesParseState, _fromExpCtx.get());
        resolveLetVariables(*_input, &_fromExpCtx->variables);
        _pipeline = uassertStatusOK(_mongod->makePipeline(_resolvedPipeline, _fromExpCtx));

        // The $lookup stage takes responsibility for disposing of its Pipeline, since it will
        // potentially be used by multiple OperationContexts, and the $lookup stage is part of an
        // outer Pipeline that will propagate dispose() calls before being destroyed.
        _pipeline.get_deleter().dismissDisposal();

        _cursorIndex = 0;
        _nextValue = _pipeline->getNext();

        if (_unwindSrc->preserveNullAndEmptyArrays() && !_nextValue) {
            // There were no results for this cursor, but the $unwind was asked to preserve empty
            // arrays, so we should return a document without the array.
            MutableDocument output(std::move(*_input));
            // Note this will correctly create objects in the prefix of '_as', to act as if we had
            // created an empty array and then removed it.
            output.setNestedField(_as, Value());
            if (indexPath) {
                output.setNestedField(*indexPath, Value(BSONNULL));
            }
            return output.freeze();
        }
    }

    invariant(bool(_input) && bool(_nextValue));
    auto currentValue = *_nextValue;
    _nextValue = _pipeline->getNext();

    // Move input document into output if this is the last or only result, otherwise perform a copy.
    MutableDocument output(_nextValue ? *_input : std::move(*_input));
    output.setNestedField(_as, Value(currentValue));

    if (indexPath) {
        output.setNestedField(*indexPath, Value(_cursorIndex));
    }

    ++_cursorIndex;
    return output.freeze();
}

void DocumentSourceLookUp::copyVariablesToExpCtx(const Variables& vars,
                                                 const VariablesParseState& vps,
                                                 ExpressionContext* expCtx) {
    expCtx->variables = vars;
    expCtx->variablesParseState = vps.copyWith(expCtx->variables.useIdGenerator());
}

void DocumentSourceLookUp::resolveLetVariables(const Document& localDoc, Variables* variables) {
    invariant(variables);

    for (auto& letVar : _letVariables) {
        auto value = letVar.expression->evaluate(localDoc);
        variables->setValue(letVar.id, value);
    }
}

void DocumentSourceLookUp::serializeToArray(
    std::vector<Value>& array, boost::optional<ExplainOptions::Verbosity> explain) const {
    Document doc;
    if (wasConstructedWithPipelineSyntax()) {
        MutableDocument exprList;
        for (auto letVar : _letVariables) {
            exprList.addField(letVar.name,
                              letVar.expression->serialize(static_cast<bool>(explain)));
        }

        auto pipeline = _userPipeline;
        if (_additionalFilter) {
            pipeline.push_back(BSON("$match" << *_additionalFilter));
        }

        doc = Document{{getSourceName(),
                        Document{{"from", _fromNs.coll()},
                                 {"as", _as.fullPath()},
                                 {"let", exprList.freeze()},
                                 {"pipeline", pipeline}}}};
    } else {
        doc = Document{{getSourceName(),
                        {Document{{"from", _fromNs.coll()},
                                  {"as", _as.fullPath()},
                                  {"localField", _localField->fullPath()},
                                  {"foreignField", _foreignField->fullPath()}}}}};
    }

    MutableDocument output(doc);
    if (explain) {
        if (_unwindSrc) {
            const boost::optional<FieldPath> indexPath = _unwindSrc->indexPath();
            output[getSourceName()]["unwinding"] =
                Value(DOC("preserveNullAndEmptyArrays"
                          << _unwindSrc->preserveNullAndEmptyArrays()
                          << "includeArrayIndex"
                          << (indexPath ? Value(indexPath->fullPath()) : Value())));
        }

        // Only add _matchSrc for explain when $lookup was constructed with localField/foreignField
        // syntax. For pipeline sytax, _matchSrc will be included as part of the pipeline
        // definition.
        if (!wasConstructedWithPipelineSyntax() && _additionalFilter) {
            // Our output does not have to be parseable, so include a "matching" field with the
            // descended match expression.
            output[getSourceName()]["matching"] = Value(*_additionalFilter);
        }

        array.push_back(Value(output.freeze()));
    } else {
        array.push_back(Value(output.freeze()));

        if (_unwindSrc) {
            _unwindSrc->serializeToArray(array);
        }

        if (!wasConstructedWithPipelineSyntax() && _matchSrc) {
            // '_matchSrc' tracks the originally specified $match. We descend upon the $match in the
            // first call to getNext(), at which point we are confident that we no longer need to
            // serialize the $lookup again.
            _matchSrc->serializeToArray(array);
        }
    }
}

DocumentSource::GetDepsReturn DocumentSourceLookUp::getDependencies(DepsTracker* deps) const {
    if (wasConstructedWithPipelineSyntax()) {
        for (auto&& letVar : _letVariables) {
            letVar.expression->addDependencies(deps);
        }
    } else {
        deps->fields.insert(_localField->fullPath());
    }
    return SEE_NEXT;
}

void DocumentSourceLookUp::doDetachFromOperationContext() {
    if (_pipeline) {
        // We have a pipeline we're going to be executing across multiple calls to getNext(), so we
        // use Pipeline::detachFromOperationContext() to take care of updating '_fromExpCtx->opCtx'.
        _pipeline->detachFromOperationContext();
        invariant(_fromExpCtx->opCtx == nullptr);
    } else if (_fromExpCtx) {
        _fromExpCtx->opCtx = nullptr;
    }
}

void DocumentSourceLookUp::doReattachToOperationContext(OperationContext* opCtx) {
    if (_pipeline) {
        // We have a pipeline we're going to be executing across multiple calls to getNext(), so we
        // use Pipeline::reattachToOperationContext() to take care of updating '_fromExpCtx->opCtx'.
        _pipeline->reattachToOperationContext(opCtx);
        invariant(_fromExpCtx->opCtx == opCtx);
    } else if (_fromExpCtx) {
        _fromExpCtx->opCtx = opCtx;
    }
}

intrusive_ptr<DocumentSource> DocumentSourceLookUp::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            "the $lookup specification must be an Object",
            elem.type() == BSONType::Object);

    NamespaceString fromNs;
    std::string as;

    std::string localField;
    std::string foreignField;

    BSONObj letVariables;
    std::vector<BSONObj> pipeline;
    bool hasPipeline = false;
    bool hasLet = false;

    for (auto&& argument : elem.Obj()) {
        const auto argName = argument.fieldNameStringData();

        if (argName == "pipeline") {
            auto result = AggregationRequest::parsePipelineFromBSON(argument);
            if (!result.isOK()) {
                uasserted(ErrorCodes::FailedToParse,
                          str::stream() << "invalid $lookup pipeline definition: "
                                        << result.getStatus().toString());
            }
            pipeline = std::move(result.getValue());
            hasPipeline = true;
            continue;
        }

        if (argName == "let") {
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "$lookup argument '" << argument
                                  << "' must be an object, is type "
                                  << argument.type(),
                    argument.type() == BSONType::Object);
            letVariables = argument.Obj();
            hasLet = true;
            continue;
        }

        uassert(ErrorCodes::FailedToParse,
                str::stream() << "$lookup argument '" << argument << "' must be a string, is type "
                              << argument.type(),
                argument.type() == BSONType::String);

        if (argName == "from") {
            fromNs = NamespaceString(pExpCtx->ns.db().toString() + '.' + argument.String());
        } else if (argName == "as") {
            as = argument.String();
        } else if (argName == "localField") {
            localField = argument.String();
        } else if (argName == "foreignField") {
            foreignField = argument.String();
        } else {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream() << "unknown argument to $lookup: " << argument.fieldName());
        }
    }

    uassert(
        ErrorCodes::FailedToParse, "must specify 'from' field for a $lookup", !fromNs.ns().empty());
    uassert(ErrorCodes::FailedToParse, "must specify 'as' field for a $lookup", !as.empty());

    if (hasPipeline) {
        uassert(ErrorCodes::FailedToParse,
                "$lookup with 'pipeline' may not specify 'localField' or 'foreignField'",
                localField.empty() && foreignField.empty());

        return new DocumentSourceLookUp(std::move(fromNs),
                                        std::move(as),
                                        std::move(pipeline),
                                        std::move(letVariables),
                                        pExpCtx);
    } else {
        uassert(ErrorCodes::FailedToParse,
                "$lookup requires either 'pipeline' or both 'localField' and 'foreignField' to be "
                "specified",
                !localField.empty() && !foreignField.empty());
        uassert(ErrorCodes::FailedToParse,
                "$lookup with a 'let' argument must also specify 'pipeline'",
                !hasLet);

        return new DocumentSourceLookUp(std::move(fromNs),
                                        std::move(as),
                                        std::move(localField),
                                        std::move(foreignField),
                                        pExpCtx);
    }
}
}
