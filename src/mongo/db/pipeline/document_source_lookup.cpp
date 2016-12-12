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
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using boost::intrusive_ptr;
using std::vector;

DocumentSourceLookUp::DocumentSourceLookUp(NamespaceString fromNs,
                                           std::string as,
                                           std::string localField,
                                           std::string foreignField,
                                           const boost::intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSourceNeedsMongod(pExpCtx),
      _fromNs(std::move(fromNs)),
      _as(std::move(as)),
      _localField(std::move(localField)),
      _foreignField(foreignField),
      _foreignFieldFieldName(std::move(foreignField)) {}

std::unique_ptr<LiteParsedDocumentSourceOneForeignCollection> DocumentSourceLookUp::liteParse(
    const AggregationRequest& request, const BSONElement& spec) {
    uassert(40319,
            str::stream() << "the $lookup stage specification must be an object, but found "
                          << typeName(spec.type()),
            spec.type() == BSONType::Object);

    auto specObj = spec.Obj();
    auto fromElement = specObj["from"];
    uassert(40320,
            str::stream() << "missing 'from' option to $lookup stage specification: " << specObj,
            fromElement);
    uassert(40321,
            str::stream() << "'from' option to $lookup must be a string, but was type "
                          << typeName(specObj["from"].type()),
            fromElement.type() == BSONType::String);

    NamespaceString nss(request.getNamespaceString().db(), fromElement.valueStringData());
    uassert(40322, str::stream() << "invalid $lookup namespace: " << nss.ns(), nss.isValid());
    return stdx::make_unique<LiteParsedDocumentSourceOneForeignCollection>(std::move(nss));
}

REGISTER_DOCUMENT_SOURCE(lookup,
                         DocumentSourceLookUp::liteParse,
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
BSONObj buildEqualityOrQuery(const std::string& fieldName, const vector<Value>& values) {
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

    uassert(4567, "from collection cannot be sharded", !_mongod->isSharded(_fromExpCtx->ns));

    if (!_additionalFilter && _matchSrc) {
        // We have internalized a $match, but have not yet computed the descended $match that should
        // be applied to our queries.
        _additionalFilter = DocumentSourceMatch::descendMatchOnPath(
                                _matchSrc->getMatchExpression(), _as.fullPath(), pExpCtx)
                                ->getQuery();
    }

    if (_handlingUnwind) {
        return unwindResult();
    }

    auto nextInput = pSource->getNext();
    if (!nextInput.isAdvanced()) {
        return nextInput;
    }
    auto inputDoc = nextInput.releaseDocument();

    // If we have not absorbed a $unwind, we cannot absorb a $match. If we have absorbed a $unwind,
    // '_handlingUnwind' would be set to true, and we would not have made it here.
    invariant(!_matchSrc);

    auto matchStage =
        makeMatchStageFromInput(inputDoc, _localField, _foreignFieldFieldName, BSONObj());
    // We've already allocated space for the trailing $match stage in '_fromPipeline'.
    _fromPipeline.back() = matchStage;
    auto pipeline = uassertStatusOK(_mongod->makePipeline(_fromPipeline, _fromExpCtx));

    std::vector<Value> results;
    int objsize = 0;
    while (auto result = pipeline->getNext()) {
        objsize += result->getApproximateSize();
        uassert(4568,
                str::stream() << "Total size of documents in " << _fromNs.coll() << " matching "
                              << matchStage
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
    return {GetModPathsReturn::Type::kFiniteSet, std::move(modifiedPaths)};
}

Pipeline::SourceContainer::iterator DocumentSourceLookUp::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    auto nextUnwind = dynamic_cast<DocumentSourceUnwind*>((*std::next(itr)).get());

    // If we are not already handling an $unwind stage internally, we can combine with the
    // following $unwind stage.
    if (nextUnwind && !_handlingUnwind && nextUnwind->getUnwindPath() == _as.fullPath()) {
        _unwindSrc = std::move(nextUnwind);
        _handlingUnwind = true;
        container->erase(std::next(itr));
        return itr;
    }

    // Attempt to internalize any predicates of a $match upon the "_as" field.
    auto nextMatch = dynamic_cast<DocumentSourceMatch*>((*std::next(itr)).get());

    if (!nextMatch) {
        return std::next(itr);
    }

    if (!_handlingUnwind || _unwindSrc->indexPath() || _unwindSrc->preserveNullAndEmptyArrays()) {
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
    if (!_handlingMatch) {
        _matchSrc = nextMatch;
        _handlingMatch = true;
    } else {
        // We have already absorbed a $match. We need to join it with 'dependent'.
        _matchSrc->joinMatchWith(nextMatch);
    }

    // Remove the original $match. There may be further optimization between this $lookup and the
    // new neighbor, so we return an iterator pointing to ourself.
    container->erase(std::next(itr));
    return itr;
}

void DocumentSourceLookUp::dispose() {
    _pipeline.reset();
    pSource->dispose();
}

BSONObj DocumentSourceLookUp::makeMatchStageFromInput(const Document& input,
                                                      const FieldPath& localFieldPath,
                                                      const std::string& foreignFieldName,
                                                      const BSONObj& additionalFilter) {
    Value localFieldVal = input.getNestedField(localFieldPath);

    // Missing values are treated as null.
    if (localFieldVal.missing()) {
        localFieldVal = Value(BSONNULL);
    }

    // We construct a query of one of the following forms, depending on the contents of
    // 'localFieldVal'.
    //
    //   {$and: [{<foreignFieldName>: {$eq: <localFieldVal>}}, <additionalFilter>]}
    //     if 'localFieldVal' isn't an array value.
    //
    //   {$and: [{<foreignFieldName>: {$in: [<value>, <value>, ...]}}, <additionalFilter>]}
    //     if 'localFieldVal' is an array value but doesn't contain any elements that are regular
    //     expressions.
    //
    //   {$and: [{$or: [{<foreignFieldName>: {$eq: <value>}},
    //                  {<foreignFieldName>: {$eq: <value>}}, ...]},
    //           <additionalFilter>]}
    //     if 'localFieldVal' is an array value and it contains at least one element that is a
    //     regular expression.

    // We wrap the query in a $match so that it can be parsed into a DocumentSourceMatch when
    // constructing a pipeline to execute.
    BSONObjBuilder match;
    BSONObjBuilder query(match.subobjStart("$match"));

    BSONArrayBuilder andObj(query.subarrayStart("$and"));
    BSONObjBuilder joiningObj(andObj.subobjStart());

    if (localFieldVal.isArray()) {
        // A $lookup on an array value corresponds to finding documents in the foreign collection
        // that have a value of any of the elements in the array value, rather than finding
        // documents that have a value equal to the entire array value. These semantics are
        // automatically provided to us by using the $in query operator.
        const vector<Value>& localArray = localFieldVal.getArray();
        const bool containsRegex = std::any_of(
            localArray.begin(), localArray.end(), [](Value val) { return val.getType() == RegEx; });

        if (containsRegex) {
            // A regular expression inside the $in query operator will perform pattern matching on
            // any string values. Since we want regular expressions to only match other RegEx types,
            // we write the query as a $or of equality comparisons instead.
            BSONObj orQuery = buildEqualityOrQuery(foreignFieldName, localFieldVal.getArray());
            joiningObj.appendElements(orQuery);
        } else {
            // { <foreignFieldName> : { "$in" : <localFieldVal> } }
            BSONObjBuilder subObj(joiningObj.subobjStart(foreignFieldName));
            subObj << "$in" << localFieldVal;
            subObj.doneFast();
        }
    } else {
        // { <foreignFieldName> : { "$eq" : <localFieldVal> } }
        BSONObjBuilder subObj(joiningObj.subobjStart(foreignFieldName));
        subObj << "$eq" << localFieldVal;
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

        BSONObj filter = _additionalFilter.value_or(BSONObj());
        auto matchStage =
            makeMatchStageFromInput(*_input, _localField, _foreignFieldFieldName, filter);
        // We've already allocated space for the trailing $match stage in '_fromPipeline'.
        _fromPipeline.back() = matchStage;
        _pipeline = uassertStatusOK(_mongod->makePipeline(_fromPipeline, _fromExpCtx));

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

void DocumentSourceLookUp::serializeToArray(std::vector<Value>& array, bool explain) const {
    MutableDocument output(DOC(
        getSourceName() << DOC("from" << _fromNs.coll() << "as" << _as.fullPath() << "localField"
                                      << _localField.fullPath()
                                      << "foreignField"
                                      << _foreignField.fullPath())));
    if (explain) {
        if (_handlingUnwind) {
            const boost::optional<FieldPath> indexPath = _unwindSrc->indexPath();
            output[getSourceName()]["unwinding"] =
                Value(DOC("preserveNullAndEmptyArrays"
                          << _unwindSrc->preserveNullAndEmptyArrays()
                          << "includeArrayIndex"
                          << (indexPath ? Value(indexPath->fullPath()) : Value())));
        }

        if (_matchSrc) {
            // Our output does not have to be parseable, so include a "matching" field with the
            // descended match expression.
            output[getSourceName()]["matching"] =
                Value(DocumentSourceMatch::descendMatchOnPath(
                          _matchSrc->getMatchExpression(), _as.fullPath(), pExpCtx)
                          ->getQuery());
        }

        array.push_back(Value(output.freeze()));
    } else {
        array.push_back(Value(output.freeze()));

        if (_handlingUnwind) {
            _unwindSrc->serializeToArray(array);
        }

        if (_matchSrc) {
            // '_matchSrc' tracks the originally specified $match. We descend upon the $match in the
            // first call to getNext(), at which point we are confident that we no longer need to
            // serialize the $lookup again.
            _matchSrc->serializeToArray(array);
        }
    }
}

DocumentSource::GetDepsReturn DocumentSourceLookUp::getDependencies(DepsTracker* deps) const {
    deps->fields.insert(_localField.fullPath());
    return SEE_NEXT;
}

void DocumentSourceLookUp::doInjectExpressionContext() {
    auto it = pExpCtx->resolvedNamespaces.find(_fromNs.coll());
    invariant(it != pExpCtx->resolvedNamespaces.end());
    const auto& resolvedNamespace = it->second;
    _fromExpCtx = pExpCtx->copyWith(resolvedNamespace.ns);
    _fromPipeline = resolvedNamespace.pipeline;

    // We append an additional BSONObj to '_fromPipeline' as a placeholder for the $match stage
    // we'll eventually construct from the input document.
    _fromPipeline.reserve(_fromPipeline.size() + 1);
    _fromPipeline.push_back(BSONObj());
}

void DocumentSourceLookUp::doDetachFromOperationContext() {
    if (_pipeline) {
        // We have a pipeline we're going to be executing across multiple calls to getNext(), so we
        // use Pipeline::detachFromOperationContext() to take care of updating '_fromExpCtx->opCtx'.
        _pipeline->detachFromOperationContext();
        invariant(_fromExpCtx->opCtx == nullptr);
    } else {
        _fromExpCtx->opCtx = nullptr;
    }
}

void DocumentSourceLookUp::doReattachToOperationContext(OperationContext* opCtx) {
    if (_pipeline) {
        // We have a pipeline we're going to be executing across multiple calls to getNext(), so we
        // use Pipeline::reattachToOperationContext() to take care of updating '_fromExpCtx->opCtx'.
        _pipeline->reattachToOperationContext(opCtx);
        invariant(_fromExpCtx->opCtx == opCtx);
    } else {
        _fromExpCtx->opCtx = opCtx;
    }
}

intrusive_ptr<DocumentSource> DocumentSourceLookUp::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(4569, "the $lookup specification must be an Object", elem.type() == Object);

    NamespaceString fromNs;
    std::string as;
    std::string localField;
    std::string foreignField;

    for (auto&& argument : elem.Obj()) {
        uassert(4570,
                str::stream() << "arguments to $lookup must be strings, " << argument << " is type "
                              << argument.type(),
                argument.type() == String);
        const auto argName = argument.fieldNameStringData();

        if (argName == "from") {
            fromNs = NamespaceString(pExpCtx->ns.db().toString() + '.' + argument.String());
        } else if (argName == "as") {
            as = argument.String();
        } else if (argName == "localField") {
            localField = argument.String();
        } else if (argName == "foreignField") {
            foreignField = argument.String();
        } else {
            uasserted(4571,
                      str::stream() << "unknown argument to $lookup: " << argument.fieldName());
        }
    }

    uassert(4572,
            "need to specify fields from, as, localField, and foreignField for a $lookup",
            !fromNs.ns().empty() && !as.empty() && !localField.empty() && !foreignField.empty());

    return new DocumentSourceLookUp(
        std::move(fromNs), std::move(as), std::move(localField), std::move(foreignField), pExpCtx);
}
}
