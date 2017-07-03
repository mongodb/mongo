/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/db/pipeline/document_source_graph_lookup.h"

#include "mongo/base/init.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_comparator.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using boost::intrusive_ptr;

namespace dps = ::mongo::dotted_path_support;

std::unique_ptr<LiteParsedDocumentSourceForeignCollections> DocumentSourceGraphLookUp::liteParse(
    const AggregationRequest& request, const BSONElement& spec) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "the $graphLookup stage specification must be an object, but found "
                          << typeName(spec.type()),
            spec.type() == BSONType::Object);

    auto specObj = spec.Obj();
    auto fromElement = specObj["from"];
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "missing 'from' option to $graphLookup stage specification: "
                          << specObj,
            fromElement);
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "'from' option to $graphLookup must be a string, but was type "
                          << typeName(specObj["from"].type()),
            fromElement.type() == BSONType::String);

    NamespaceString nss(request.getNamespaceString().db(), fromElement.valueStringData());
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "invalid $graphLookup namespace: " << nss.ns(),
            nss.isValid());

    PrivilegeVector privileges{
        Privilege(ResourcePattern::forExactNamespace(nss), ActionType::find)};

    return stdx::make_unique<LiteParsedDocumentSourceForeignCollections>(std::move(nss),
                                                                         std::move(privileges));
}

REGISTER_DOCUMENT_SOURCE(graphLookup,
                         DocumentSourceGraphLookUp::liteParse,
                         DocumentSourceGraphLookUp::createFromBson);

const char* DocumentSourceGraphLookUp::getSourceName() const {
    return "$graphLookup";
}

DocumentSource::GetNextResult DocumentSourceGraphLookUp::getNext() {
    pExpCtx->checkForInterrupt();

    if (_unwind) {
        return getNextUnwound();
    }

    // We aren't handling a $unwind, process the input document normally.
    auto input = pSource->getNext();
    if (!input.isAdvanced()) {
        return input;
    }

    _input = input.releaseDocument();

    performSearch();

    std::vector<Value> results;
    while (!_visited.empty()) {
        // Remove elements one at a time to avoid consuming more memory.
        auto it = _visited.begin();
        results.push_back(Value(it->second));
        _visited.erase(it);
    }

    MutableDocument output(*_input);
    output.setNestedField(_as, Value(std::move(results)));

    _visitedUsageBytes = 0;

    invariant(_visited.empty());

    return output.freeze();
}

DocumentSource::GetNextResult DocumentSourceGraphLookUp::getNextUnwound() {
    const boost::optional<FieldPath> indexPath((*_unwind)->indexPath());

    // If the unwind is not preserving empty arrays, we might have to process multiple inputs before
    // we get one that will produce an output.
    while (true) {
        if (_visited.empty()) {
            // No results are left for the current input, so we should move on to the next one and
            // perform a new search.

            auto input = pSource->getNext();
            if (!input.isAdvanced()) {
                return input;
            }

            _input = input.releaseDocument();
            performSearch();
            _visitedUsageBytes = 0;
            _outputIndex = 0;
        }
        MutableDocument unwound(*_input);

        if (_visited.empty()) {
            if ((*_unwind)->preserveNullAndEmptyArrays()) {
                // Since "preserveNullAndEmptyArrays" was specified, output a document even though
                // we had no result.
                unwound.setNestedField(_as, Value());
                if (indexPath) {
                    unwound.setNestedField(*indexPath, Value(BSONNULL));
                }
            } else {
                // $unwind would not output anything, since the '_as' field would not exist. We
                // should loop until we have something to return.
                continue;
            }
        } else {
            auto it = _visited.begin();
            unwound.setNestedField(_as, Value(it->second));
            if (indexPath) {
                unwound.setNestedField(*indexPath, Value(_outputIndex));
                ++_outputIndex;
            }
            _visited.erase(it);
        }

        return unwound.freeze();
    }
}

void DocumentSourceGraphLookUp::doDispose() {
    _cache.clear();
    _frontier.clear();
    _visited.clear();
}

void DocumentSourceGraphLookUp::doBreadthFirstSearch() {
    long long depth = 0;
    bool shouldPerformAnotherQuery;
    do {
        shouldPerformAnotherQuery = false;

        // Check whether each key in the frontier exists in the cache or needs to be queried.
        auto cached = pExpCtx->getDocumentComparator().makeUnorderedDocumentSet();
        auto matchStage = makeMatchStageFromFrontier(&cached);

        ValueUnorderedSet queried = pExpCtx->getValueComparator().makeUnorderedValueSet();
        _frontier.swap(queried);
        _frontierUsageBytes = 0;

        // Process cached values, populating '_frontier' for the next iteration of search.
        while (!cached.empty()) {
            auto doc = *cached.begin();
            cached.erase(cached.begin());
            shouldPerformAnotherQuery =
                addToVisitedAndFrontier(std::move(doc), depth) || shouldPerformAnotherQuery;
            checkMemoryUsage();
        }

        if (matchStage) {
            // Query for all keys that were in the frontier and not in the cache, populating
            // '_frontier' for the next iteration of search.

            // We've already allocated space for the trailing $match stage in '_fromPipeline'.
            _fromPipeline.back() = *matchStage;
            auto pipeline = uassertStatusOK(_mongod->makePipeline(_fromPipeline, _fromExpCtx));
            while (auto next = pipeline->getNext()) {
                uassert(40271,
                        str::stream()
                            << "Documents in the '"
                            << _from.ns()
                            << "' namespace must contain an _id for de-duplication in $graphLookup",
                        !(*next)["_id"].missing());

                shouldPerformAnotherQuery =
                    addToVisitedAndFrontier(*next, depth) || shouldPerformAnotherQuery;
                addToCache(std::move(*next), queried);
            }
            checkMemoryUsage();
        }

        ++depth;
    } while (shouldPerformAnotherQuery && depth < std::numeric_limits<long long>::max() &&
             (!_maxDepth || depth <= *_maxDepth));

    _frontier.clear();
    _frontierUsageBytes = 0;
}

bool DocumentSourceGraphLookUp::addToVisitedAndFrontier(Document result, long long depth) {
    auto id = result.getField("_id");

    if (_visited.find(id) != _visited.end()) {
        // We've already seen this object, don't repeat any work.
        return false;
    }

    // We have not seen this node before. If '_depthField' was specified, add the field to the
    // object.
    if (_depthField) {
        MutableDocument mutableDoc(std::move(result));
        mutableDoc.setNestedField(*_depthField, Value(depth));
        result = mutableDoc.freeze();
    }

    // Add the 'connectFromField' of 'result' into '_frontier'. If the 'connectFromField' is an
    // array, we treat it as connecting to multiple values, so we must add each element to
    // '_frontier'.
    document_path_support::visitAllValuesAtPath(
        result, _connectFromField, [this](const Value& nextFrontierValue) {
            _frontier.insert(nextFrontierValue);
            _frontierUsageBytes += nextFrontierValue.getApproximateSize();
        });

    // Add the object to our '_visited' list and update the size of '_visited' appropriately.
    _visitedUsageBytes += id.getApproximateSize();
    _visitedUsageBytes += result.getApproximateSize();

    _visited[id] = std::move(result);

    // We inserted into _visited, so return true.
    return true;
}

void DocumentSourceGraphLookUp::addToCache(const Document& result,
                                           const ValueUnorderedSet& queried) {
    document_path_support::visitAllValuesAtPath(
        result, _connectToField, [this, &queried, &result](const Value& connectToValue) {
            // It is possible that 'connectToValue' is a single value, but was not queried for. For
            // instance, with a connectToField of "a.b" and a document with the structure:
            // {a: [{b: 1}, {b: 0}]}, this document will be retrieved by querying for "{b: 1}", but
            // the outer for loop will split this into two separate connectToValues. {b: 0} was not
            // queried for, and thus, we cannot cache under it.
            if (queried.find(connectToValue) != queried.end()) {
                _cache.insert(connectToValue, result);
            }
        });
}

boost::optional<BSONObj> DocumentSourceGraphLookUp::makeMatchStageFromFrontier(
    DocumentUnorderedSet* cached) {
    // Add any cached values to 'cached' and remove them from '_frontier'.
    for (auto it = _frontier.begin(); it != _frontier.end();) {
        if (auto entry = _cache[*it]) {
            cached->insert(entry->begin(), entry->end());
            size_t valueSize = it->getApproximateSize();
            it = _frontier.erase(it);

            // If the cached value increased in size while in the cache, we don't want to underflow
            // '_frontierUsageBytes'.
            invariant(valueSize <= _frontierUsageBytes);
            _frontierUsageBytes -= valueSize;
        } else {
            ++it;
        }
    }

    // Create a query of the form {$and: [_additionalFilter, {_connectToField: {$in: [...]}}]}.
    //
    // We wrap the query in a $match so that it can be parsed into a DocumentSourceMatch when
    // constructing a pipeline to execute.
    BSONObjBuilder match;
    {
        BSONObjBuilder query(match.subobjStart("$match"));
        {
            BSONArrayBuilder andObj(query.subarrayStart("$and"));
            if (_additionalFilter) {
                andObj << *_additionalFilter;
            }

            {
                BSONObjBuilder connectToObj(andObj.subobjStart());
                {
                    BSONObjBuilder subObj(connectToObj.subobjStart(_connectToField.fullPath()));
                    {
                        BSONArrayBuilder in(subObj.subarrayStart("$in"));
                        for (auto&& value : _frontier) {
                            in << value;
                        }
                    }
                }
            }
        }
    }

    return _frontier.empty() ? boost::none : boost::optional<BSONObj>(match.obj());
}

void DocumentSourceGraphLookUp::performSearch() {
    // Make sure _input is set before calling performSearch().
    invariant(_input);

    Value startingValue = _startWith->evaluate(*_input);

    // If _startWith evaluates to an array, treat each value as a separate starting point.
    if (startingValue.isArray()) {
        for (auto value : startingValue.getArray()) {
            _frontier.insert(value);
            _frontierUsageBytes += value.getApproximateSize();
        }
    } else {
        _frontier.insert(startingValue);
        _frontierUsageBytes += startingValue.getApproximateSize();
    }

    doBreadthFirstSearch();
}

DocumentSource::GetModPathsReturn DocumentSourceGraphLookUp::getModifiedPaths() const {
    std::set<std::string> modifiedPaths{_as.fullPath()};
    if (_unwind) {
        auto pathsModifiedByUnwind = _unwind.get()->getModifiedPaths();
        invariant(pathsModifiedByUnwind.type == GetModPathsReturn::Type::kFiniteSet);
        modifiedPaths.insert(pathsModifiedByUnwind.paths.begin(),
                             pathsModifiedByUnwind.paths.end());
    }
    return {GetModPathsReturn::Type::kFiniteSet, std::move(modifiedPaths), {}};
}

Pipeline::SourceContainer::iterator DocumentSourceGraphLookUp::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    // If we are not already handling an $unwind stage internally, we can combine with the following
    // $unwind stage.
    auto nextUnwind = dynamic_cast<DocumentSourceUnwind*>((*std::next(itr)).get());
    if (nextUnwind && !_unwind && nextUnwind->getUnwindPath() == _as.fullPath()) {
        _unwind = std::move(nextUnwind);
        container->erase(std::next(itr));
        return itr;
    }
    return std::next(itr);
}

BSONObjSet DocumentSourceGraphLookUp::getOutputSorts() {
    std::set<std::string> fields{_as.fullPath()};
    if (_depthField) {
        fields.insert(_depthField->fullPath());
    }
    if (_unwind && (*_unwind)->indexPath()) {
        fields.insert((*_unwind)->indexPath()->fullPath());
    }

    return DocumentSource::truncateSortSet(pSource->getOutputSorts(), fields);
}

void DocumentSourceGraphLookUp::checkMemoryUsage() {
    // TODO SERVER-23980: Implement spilling to disk if allowDiskUse is specified.
    uassert(40099,
            "$graphLookup reached maximum memory consumption",
            (_visitedUsageBytes + _frontierUsageBytes) < _maxMemoryUsageBytes);
    _cache.evictDownTo(_maxMemoryUsageBytes - _frontierUsageBytes - _visitedUsageBytes);
}

void DocumentSourceGraphLookUp::serializeToArray(
    std::vector<Value>& array, boost::optional<ExplainOptions::Verbosity> explain) const {
    // Serialize default options.
    MutableDocument spec(DOC("from" << _from.coll() << "as" << _as.fullPath() << "connectToField"
                                    << _connectToField.fullPath()
                                    << "connectFromField"
                                    << _connectFromField.fullPath()
                                    << "startWith"
                                    << _startWith->serialize(false)));

    // depthField is optional; serialize it if it was specified.
    if (_depthField) {
        spec["depthField"] = Value(_depthField->fullPath());
    }

    if (_maxDepth) {
        spec["maxDepth"] = Value(*_maxDepth);
    }

    if (_additionalFilter) {
        spec["restrictSearchWithMatch"] = Value(*_additionalFilter);
    }

    // If we are explaining, include an absorbed $unwind inside the $graphLookup specification.
    if (_unwind && explain) {
        const boost::optional<FieldPath> indexPath = (*_unwind)->indexPath();
        spec["unwinding"] = Value(DOC("preserveNullAndEmptyArrays"
                                      << (*_unwind)->preserveNullAndEmptyArrays()
                                      << "includeArrayIndex"
                                      << (indexPath ? Value((*indexPath).fullPath()) : Value())));
    }

    array.push_back(Value(DOC(getSourceName() << spec.freeze())));

    // If we are not explaining, the output of this method must be parseable, so serialize our
    // $unwind into a separate stage.
    if (_unwind && !explain) {
        (*_unwind)->serializeToArray(array);
    }
}

void DocumentSourceGraphLookUp::doDetachFromOperationContext() {
    _fromExpCtx->opCtx = nullptr;
}

void DocumentSourceGraphLookUp::doReattachToOperationContext(OperationContext* opCtx) {
    _fromExpCtx->opCtx = opCtx;
}

DocumentSourceGraphLookUp::DocumentSourceGraphLookUp(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    NamespaceString from,
    std::string as,
    std::string connectFromField,
    std::string connectToField,
    boost::intrusive_ptr<Expression> startWith,
    boost::optional<BSONObj> additionalFilter,
    boost::optional<FieldPath> depthField,
    boost::optional<long long> maxDepth,
    boost::optional<boost::intrusive_ptr<DocumentSourceUnwind>> unwindSrc)
    : DocumentSourceNeedsMongod(expCtx),
      _from(std::move(from)),
      _as(std::move(as)),
      _connectFromField(std::move(connectFromField)),
      _connectToField(std::move(connectToField)),
      _startWith(std::move(startWith)),
      _additionalFilter(additionalFilter),
      _depthField(depthField),
      _maxDepth(maxDepth),
      _frontier(pExpCtx->getValueComparator().makeUnorderedValueSet()),
      _visited(ValueComparator::kInstance.makeUnorderedValueMap<Document>()),
      _cache(pExpCtx->getValueComparator()),
      _unwind(unwindSrc) {
    const auto& resolvedNamespace = pExpCtx->getResolvedNamespace(_from);
    _fromExpCtx = pExpCtx->copyWith(resolvedNamespace.ns);
    _fromPipeline = resolvedNamespace.pipeline;

    // We append an additional BSONObj to '_fromPipeline' as a placeholder for the $match stage
    // we'll eventually construct from the input document.
    _fromPipeline.reserve(_fromPipeline.size() + 1);
    _fromPipeline.push_back(BSONObj());
}

intrusive_ptr<DocumentSourceGraphLookUp> DocumentSourceGraphLookUp::create(
    const intrusive_ptr<ExpressionContext>& expCtx,
    NamespaceString fromNs,
    std::string asField,
    std::string connectFromField,
    std::string connectToField,
    intrusive_ptr<Expression> startWith,
    boost::optional<BSONObj> additionalFilter,
    boost::optional<FieldPath> depthField,
    boost::optional<long long> maxDepth,
    boost::optional<boost::intrusive_ptr<DocumentSourceUnwind>> unwindSrc) {
    intrusive_ptr<DocumentSourceGraphLookUp> source(
        new DocumentSourceGraphLookUp(expCtx,
                                      std::move(fromNs),
                                      std::move(asField),
                                      std::move(connectFromField),
                                      std::move(connectToField),
                                      std::move(startWith),
                                      additionalFilter,
                                      depthField,
                                      maxDepth,
                                      unwindSrc));
    return source;
}

intrusive_ptr<DocumentSource> DocumentSourceGraphLookUp::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    NamespaceString from;
    std::string as;
    boost::intrusive_ptr<Expression> startWith;
    std::string connectFromField;
    std::string connectToField;
    boost::optional<FieldPath> depthField;
    boost::optional<long long> maxDepth;
    boost::optional<BSONObj> additionalFilter;

    VariablesParseState vps = expCtx->variablesParseState;

    for (auto&& argument : elem.Obj()) {
        const auto argName = argument.fieldNameStringData();

        if (argName == "startWith") {
            startWith = Expression::parseOperand(expCtx, argument, vps);
            continue;
        } else if (argName == "maxDepth") {
            uassert(40100,
                    str::stream() << "maxDepth must be numeric, found type: "
                                  << typeName(argument.type()),
                    argument.isNumber());
            maxDepth = argument.safeNumberLong();
            uassert(40101,
                    str::stream() << "maxDepth requires a nonnegative argument, found: "
                                  << *maxDepth,
                    *maxDepth >= 0);
            uassert(40102,
                    str::stream() << "maxDepth could not be represented as a long long: "
                                  << *maxDepth,
                    *maxDepth == argument.number());
            continue;
        } else if (argName == "restrictSearchWithMatch") {
            uassert(40185,
                    str::stream() << "restrictSearchWithMatch must be an object, found "
                                  << typeName(argument.type()),
                    argument.type() == Object);

            // We don't need to keep ahold of the MatchExpression, but we do need to ensure that
            // the specified object is parseable.
            auto parsedMatchExpression = MatchExpressionParser::parse(
                argument.embeddedObject(), ExtensionsCallbackDisallowExtensions(), nullptr);

            uassert(40186,
                    str::stream()
                        << "Failed to parse 'restrictSearchWithMatch' option to $graphLookup: "
                        << parsedMatchExpression.getStatus().reason(),
                    parsedMatchExpression.isOK());

            uassert(40187,
                    str::stream()
                        << "Failed to parse 'restrictSearchWithMatch' option to $graphLookup: "
                        << "$near not supported.",
                    !QueryPlannerCommon::hasNode(parsedMatchExpression.getValue().get(),
                                                 MatchExpression::GEO_NEAR));

            additionalFilter = argument.embeddedObject().getOwned();
            continue;
        }

        if (argName == "from" || argName == "as" || argName == "connectFromField" ||
            argName == "depthField" || argName == "connectToField") {
            // All remaining arguments to $graphLookup are expected to be strings.
            uassert(40103,
                    str::stream() << "expected string as argument for " << argName << ", found: "
                                  << argument.toString(false, false),
                    argument.type() == String);
        }

        if (argName == "from") {
            from = NamespaceString(expCtx->ns.db().toString() + '.' + argument.String());
        } else if (argName == "as") {
            as = argument.String();
        } else if (argName == "connectFromField") {
            connectFromField = argument.String();
        } else if (argName == "connectToField") {
            connectToField = argument.String();
        } else if (argName == "depthField") {
            depthField = boost::optional<FieldPath>(FieldPath(argument.String()));
        } else {
            uasserted(40104,
                      str::stream() << "Unknown argument to $graphLookup: "
                                    << argument.fieldName());
        }
    }

    const bool isMissingRequiredField = from.ns().empty() || as.empty() || !startWith ||
        connectFromField.empty() || connectToField.empty();

    uassert(40105,
            str::stream() << "$graphLookup requires 'from', 'as', 'startWith', 'connectFromField', "
                          << "and 'connectToField' to be specified.",
            !isMissingRequiredField);

    intrusive_ptr<DocumentSourceGraphLookUp> newSource(
        new DocumentSourceGraphLookUp(expCtx,
                                      std::move(from),
                                      std::move(as),
                                      std::move(connectFromField),
                                      std::move(connectToField),
                                      std::move(startWith),
                                      additionalFilter,
                                      depthField,
                                      maxDepth,
                                      boost::none));

    return std::move(newSource);
}
}  // namespace mongo
