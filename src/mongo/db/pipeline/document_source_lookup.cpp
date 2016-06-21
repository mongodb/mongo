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

#include "document_source.h"

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

REGISTER_DOCUMENT_SOURCE(lookup, DocumentSourceLookUp::createFromBson);

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

boost::optional<Document> DocumentSourceLookUp::getNext() {
    pExpCtx->checkForInterrupt();

    uassert(4567, "from collection cannot be sharded", !_mongod->isSharded(_fromNs));

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

    boost::optional<Document> input = pSource->getNext();
    if (!input)
        return {};

    // If we have not absorbed a $unwind, we cannot absorb a $match. If we have absorbed a $unwind,
    // '_handlingUnwind' would be set to true, and we would not have made it here.
    invariant(!_matchSrc);

    BSONObj query = queryForInput(*input, _localField, _foreignFieldFieldName, BSONObj());
    std::unique_ptr<DBClientCursor> cursor = _mongod->directClient()->query(_fromNs.ns(), query);

    std::vector<Value> results;
    int objsize = 0;
    while (cursor->more()) {
        BSONObj result = cursor->nextSafe();
        objsize += result.objsize();
        uassert(4568,
                str::stream() << "Total size of documents in " << _fromNs.coll() << " matching "
                              << query
                              << " exceeds maximum document size",
                objsize <= BSONObjMaxInternalSize);
        results.push_back(Value(result));
    }

    MutableDocument output(std::move(*input));
    output.setNestedField(_as, Value(std::move(results)));
    return output.freeze();
}

Pipeline::SourceContainer::iterator DocumentSourceLookUp::optimizeAt(
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

    auto nextMatch = dynamic_cast<DocumentSourceMatch*>((*std::next(itr)).get());

    if (!nextMatch) {
        return std::next(itr);
    }

    // Attempt to move part of the $match before ourselves, and internalize any predicates upon the
    // "_as" field.
    std::string outputPath = _as.fullPath();

    std::set<std::string> fields = {outputPath};
    if (_handlingUnwind && _unwindSrc->indexPath()) {
        fields.insert((*_unwindSrc->indexPath()).fullPath());
    }

    // Attempt to split the $match, putting the independent portion before ourselves.
    auto splitMatch = nextMatch->splitSourceBy(fields);

    // Remove the original match from the pipeline.
    container->erase(std::next(itr));

    auto independent = dynamic_cast<DocumentSourceMatch*>(splitMatch.first.get());
    auto dependent = dynamic_cast<DocumentSourceMatch*>(splitMatch.second.get());

    invariant(independent || dependent);

    auto locationOfNextPossibleOptimization = std::next(itr);
    if (independent) {
        // If the $match has an independent portion, insert it before ourselves. Keep track of where
        // the pipeline should check for the next possible optimization.
        container->insert(itr, std::move(independent));
        if (std::prev(itr) == container->begin()) {
            locationOfNextPossibleOptimization = std::prev(itr);
        } else {
            locationOfNextPossibleOptimization = std::prev(std::prev(itr));
        }
    }

    if (!dependent) {
        // Nothing left to do; the entire $match was moved before us.
        return locationOfNextPossibleOptimization;
    }

    // Part of the $match was dependent upon us; we must now determine if we need to split the
    // $match again to obtain a $match that is a predicate only upon the "_as" path.

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
        container->insert(std::next(itr), std::move(splitMatch.second));
        return locationOfNextPossibleOptimization;
    }

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

    expression::mapOver(dependent->getMatchExpression(), computeWhetherMatchOnAs);

    if (!isMatchOnlyOnAs) {
        // "dependent" is not wholly a predicate upon our "_as" field. We must put it back into the
        // pipeline as-is.
        container->insert(std::next(itr), std::move(splitMatch.second));
        return locationOfNextPossibleOptimization;
    }

    // We can internalize the entire $match.
    if (!_handlingMatch) {
        _matchSrc = dependent;
        _handlingMatch = true;
    } else {
        // We have already absorbed a $match. We need to join it with 'dependent'.
        _matchSrc->joinMatchWith(dependent);
    }
    return locationOfNextPossibleOptimization;
}

void DocumentSourceLookUp::dispose() {
    _cursor.reset();
    pSource->dispose();
}

BSONObj DocumentSourceLookUp::queryForInput(const Document& input,
                                            const FieldPath& localFieldPath,
                                            const std::string& foreignFieldName,
                                            const BSONObj& additionalFilter) {
    Value localFieldVal = input.getNestedField(localFieldPath);

    // Missing values are treated as null.
    if (localFieldVal.missing()) {
        localFieldVal = Value(BSONNULL);
    }

    // We are constructing a query of one of the following forms:
    // {$and: [{<foreignFieldName>: {$eq: <localFieldVal>}}, <additionalFilter>]}
    // {$and: [{<foreignFieldName>: {$in: [<value>, <value>, ...]}}, <additionalFilter>]}
    // {$and: [{$or: [{<foreignFieldName>: {$eq: <value>}},
    //                {<foreignFieldName>: {$eq: <value>}}, ...]},
    //         <additionalFilter>]}

    BSONObjBuilder query;

    BSONArrayBuilder andObj(query.subarrayStart("$and"));
    BSONObjBuilder joiningObj(andObj.subobjStart());

    if (localFieldVal.isArray()) {
        // Assume an array value logically corresponds to many documents, rather than logically
        // corresponding to one document with an array value.
        const vector<Value>& localArray = localFieldVal.getArray();
        const bool containsRegex = std::any_of(
            localArray.begin(), localArray.end(), [](Value val) { return val.getType() == RegEx; });

        if (containsRegex) {
            // A regex inside of an $in will not be treated as an equality comparison, so use an
            // $or.
            BSONObj orQuery = buildEqualityOrQuery(foreignFieldName, localFieldVal.getArray());
            joiningObj.appendElements(orQuery);
        } else {
            // { _foreignFieldFieldName : { "$in" : localFieldValue } }
            BSONObjBuilder subObj(joiningObj.subobjStart(foreignFieldName));
            subObj << "$in" << localFieldVal;
            subObj.doneFast();
        }
    } else {
        // { _foreignFieldFieldName : { "$eq" : localFieldValue } }
        BSONObjBuilder subObj(joiningObj.subobjStart(foreignFieldName));
        subObj << "$eq" << localFieldVal;
        subObj.doneFast();
    }

    joiningObj.doneFast();

    BSONObjBuilder additionalFilterObj(andObj.subobjStart());
    additionalFilterObj.appendElements(additionalFilter);
    additionalFilterObj.doneFast();

    andObj.doneFast();

    return query.obj();
}

boost::optional<Document> DocumentSourceLookUp::unwindResult() {
    const boost::optional<FieldPath> indexPath(_unwindSrc->indexPath());

    // Loop until we get a document that has at least one match.
    // Note we may return early from this loop if our source stage is exhausted or if the unwind
    // source was asked to return empty arrays and we get a document without a match.
    while (!_cursor || !_cursor->more()) {
        _input = pSource->getNext();
        if (!_input)
            return {};

        BSONObj filter = _additionalFilter.value_or(BSONObj());
        _cursor = _mongod->directClient()->query(
            _fromNs.ns(),
            DocumentSourceLookUp::queryForInput(
                *_input, _localField, _foreignFieldFieldName, filter));
        _cursorIndex = 0;

        if (_unwindSrc->preserveNullAndEmptyArrays() && !_cursor->more()) {
            // There were no results for this cursor, but the $unwind was asked to preserve empty
            // arrays, so we should return a document without the array.
            MutableDocument output(std::move(*_input));
            // Note this will correctly objects in the prefix of '_as', to act as if we had created
            // an empty array and then removed it.
            output.setNestedField(_as, Value());
            if (indexPath) {
                output.setNestedField(*indexPath, Value(BSONNULL));
            }
            return output.freeze();
        }
    }
    invariant(_cursor->more() && bool(_input));
    auto nextVal = Value(_cursor->nextSafe());

    // Move input document into output if this is the last or only result, otherwise perform a copy.
    MutableDocument output(_cursor->more() ? *_input : std::move(*_input));
    output.setNestedField(_as, nextVal);

    if (indexPath) {
        output.setNestedField(*indexPath, Value(_cursorIndex));
    }

    _cursorIndex++;
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
