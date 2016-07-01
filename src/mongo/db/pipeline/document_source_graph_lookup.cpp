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

#include "document_source.h"

#include "mongo/base/init.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using boost::intrusive_ptr;
using std::unique_ptr;

namespace dps = ::mongo::dotted_path_support;

REGISTER_DOCUMENT_SOURCE(graphLookup, DocumentSourceGraphLookUp::createFromBson);

const char* DocumentSourceGraphLookUp::getSourceName() const {
    return "$graphLookup";
}

boost::optional<Document> DocumentSourceGraphLookUp::getNext() {
    pExpCtx->checkForInterrupt();

    uassert(
        40106, "from collection must have a unique _id index", _mongod->hasUniqueIdIndex(_from));

    if (_unwind) {
        return getNextUnwound();
    }

    // We aren't handling a $unwind, process the input document normally.
    if (!(_input = pSource->getNext())) {
        dispose();
        return boost::none;
    }

    performSearch();

    std::vector<Value> results;
    while (!_visited->empty()) {
        // Remove elements one at a time to avoid consuming more memory.
        auto it = _visited->begin();
        results.push_back(Value(it->second));
        _visited->erase(it);
    }

    MutableDocument output(*_input);
    output.setNestedField(_as, Value(std::move(results)));

    _visitedUsageBytes = 0;

    invariant(_visited->empty());

    return output.freeze();
}

boost::optional<Document> DocumentSourceGraphLookUp::getNextUnwound() {
    const boost::optional<FieldPath> indexPath((*_unwind)->indexPath());

    // If the unwind is not preserving empty arrays, we might have to process multiple inputs before
    // we get one that will produce an output.
    while (true) {
        if (_visited->empty()) {
            // No results are left for the current input, so we should move on to the next one and
            // perform a new search.
            if (!(_input = pSource->getNext())) {
                dispose();
                return boost::none;
            }

            performSearch();
            _visitedUsageBytes = 0;
            _outputIndex = 0;
        }
        MutableDocument unwound(*_input);

        if (_visited->empty()) {
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
            auto it = _visited->begin();
            unwound.setNestedField(_as, Value(it->second));
            if (indexPath) {
                unwound.setNestedField(*indexPath, Value(_outputIndex));
                ++_outputIndex;
            }
            _visited->erase(it);
        }

        return unwound.freeze();
    }
}

void DocumentSourceGraphLookUp::dispose() {
    _cache.clear();
    _frontier->clear();
    _visited->clear();
    pSource->dispose();
}

void DocumentSourceGraphLookUp::doBreadthFirstSearch() {
    long long depth = 0;
    bool shouldPerformAnotherQuery;
    do {
        shouldPerformAnotherQuery = false;

        // Check whether each key in the frontier exists in the cache or needs to be queried.
        BSONObjSet cached;
        auto query = constructQuery(&cached);

        ValueUnorderedSet queried = pExpCtx->getValueComparator().makeUnorderedValueSet();
        _frontier->swap(queried);
        _frontierUsageBytes = 0;

        // Process cached values, populating '_frontier' for the next iteration of search.
        while (!cached.empty()) {
            auto it = cached.begin();
            shouldPerformAnotherQuery =
                addToVisitedAndFrontier(*it, depth) || shouldPerformAnotherQuery;
            cached.erase(it);
            checkMemoryUsage();
        }

        if (query) {
            // Query for all keys that were in the frontier and not in the cache, populating
            // '_frontier' for the next iteration of search.
            unique_ptr<DBClientCursor> cursor = _mongod->directClient()->query(_from.ns(), *query);

            // Iterate the cursor.
            while (cursor->more()) {
                BSONObj result = cursor->nextSafe();
                shouldPerformAnotherQuery =
                    addToVisitedAndFrontier(result.getOwned(), depth) || shouldPerformAnotherQuery;
                addToCache(result, queried);
            }
            checkMemoryUsage();
        }

        ++depth;
    } while (shouldPerformAnotherQuery && depth < std::numeric_limits<long long>::max() &&
             (!_maxDepth || depth <= *_maxDepth));

    _frontier->clear();
    _frontierUsageBytes = 0;
}

namespace {

BSONObj addDepthFieldToObject(const std::string& field, long long depth, BSONObj object) {
    BSONObjBuilder bob;
    bob.appendElements(object);
    bob.append(field, depth);
    return bob.obj();
}

}  // namespace

bool DocumentSourceGraphLookUp::addToVisitedAndFrontier(BSONObj result, long long depth) {
    Value _id = Value(result.getField("_id"));

    if (_visited->find(_id) != _visited->end()) {
        // We've already seen this object, don't repeat any work.
        return false;
    }

    // We have not seen this node before. If '_depthField' was specified, add the field to the
    // object.
    BSONObj fullObject =
        _depthField ? addDepthFieldToObject(_depthField->fullPath(), depth, result) : result;

    // Add the object to our '_visited' list.
    (*_visited)[_id] = fullObject;

    // Update the size of '_visited' appropriately.
    _visitedUsageBytes += _id.getApproximateSize();
    _visitedUsageBytes += static_cast<size_t>(fullObject.objsize());

    // Add the 'connectFrom' field of 'result' into '_frontier'. If the 'connectFrom' field is an
    // array, we treat it as connecting to multiple values, so we must add each element to
    // '_frontier'.
    BSONElementSet recurseOnValues;
    dps::extractAllElementsAlongPath(result, _connectFromField.fullPath(), recurseOnValues);

    for (auto&& elem : recurseOnValues) {
        Value recurseOn = Value(elem);
        if (recurseOn.isArray()) {
            for (auto&& subElem : recurseOn.getArray()) {
                _frontier->insert(subElem);
                _frontierUsageBytes += subElem.getApproximateSize();
            }
        } else if (!recurseOn.missing()) {
            // Don't recurse on a missing value.
            _frontier->insert(recurseOn);
            _frontierUsageBytes += recurseOn.getApproximateSize();
        }
    }

    // We inserted into _visited, so return true.
    return true;
}

void DocumentSourceGraphLookUp::addToCache(const BSONObj& result,
                                           const ValueUnorderedSet& queried) {
    BSONElementSet cacheByValues;
    dps::extractAllElementsAlongPath(result, _connectToField.fullPath(), cacheByValues);

    for (auto&& elem : cacheByValues) {
        Value cacheBy(elem);
        if (cacheBy.isArray()) {
            for (auto&& val : cacheBy.getArray()) {
                if (queried.find(val) != queried.end()) {
                    _cache.insert(val.getOwned(), result.getOwned());
                }
            }
        } else if (!cacheBy.missing() && queried.find(cacheBy) != queried.end()) {
            // It is possible that 'cacheBy' is a single value, but was not queried for. For
            // instance, with a connectToField of "a.b" and a document with the structure:
            // {a: [{b: 1}, {b: 0}]}, this document will be retrieved by querying for "{b: 1}", but
            // the outer for loop will split this into two separate cacheByValues. {b: 0} was not
            // queried for, and thus, we cannot cache under it.
            _cache.insert(cacheBy.getOwned(), result.getOwned());
        }
    }
}

boost::optional<BSONObj> DocumentSourceGraphLookUp::constructQuery(BSONObjSet* cached) {
    // Add any cached values to 'cached' and remove them from '_frontier'.
    for (auto it = _frontier->begin(); it != _frontier->end();) {
        if (auto entry = _cache[*it]) {
            for (auto&& obj : *entry) {
                cached->insert(obj);
            }
            size_t valueSize = it->getApproximateSize();
            it = _frontier->erase(it);

            // If the cached value increased in size while in the cache, we don't want to underflow
            // '_frontierUsageBytes'.
            invariant(valueSize <= _frontierUsageBytes);
            _frontierUsageBytes -= valueSize;
        } else {
            it = std::next(it);
        }
    }

    // Create a query of the form {$and: [_additionalFilter, {_connectToField: {$in: [...]}}]}.
    BSONObjBuilder query;
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
                    for (auto&& value : *_frontier) {
                        in << value;
                    }
                }
            }
        }
    }

    return _frontier->empty() ? boost::none : boost::optional<BSONObj>(query.obj());
}

void DocumentSourceGraphLookUp::performSearch() {
    // Make sure _input is set before calling performSearch().
    invariant(_input);

    _variables->setRoot(*_input);
    Value startingValue = _startWith->evaluateInternal(_variables.get());
    _variables->clearRoot();

    // If _startWith evaluates to an array, treat each value as a separate starting point.
    if (startingValue.isArray()) {
        for (auto value : startingValue.getArray()) {
            _frontier->insert(value);
            _frontierUsageBytes += value.getApproximateSize();
        }
    } else {
        _frontier->insert(startingValue);
        _frontierUsageBytes += startingValue.getApproximateSize();
    }

    doBreadthFirstSearch();
}

Pipeline::SourceContainer::iterator DocumentSourceGraphLookUp::optimizeAt(
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

void DocumentSourceGraphLookUp::serializeToArray(std::vector<Value>& array, bool explain) const {
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

void DocumentSourceGraphLookUp::doInjectExpressionContext() {
    _frontier = pExpCtx->getValueComparator().makeUnorderedValueSet();
    _visited = pExpCtx->getValueComparator().makeUnorderedValueMap<BSONObj>();
}

DocumentSourceGraphLookUp::DocumentSourceGraphLookUp(
    NamespaceString from,
    std::string as,
    std::string connectFromField,
    std::string connectToField,
    boost::intrusive_ptr<Expression> startWith,
    boost::optional<BSONObj> additionalFilter,
    boost::optional<FieldPath> depthField,
    boost::optional<long long> maxDepth,
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSourceNeedsMongod(expCtx),
      _from(std::move(from)),
      _as(std::move(as)),
      _connectFromField(std::move(connectFromField)),
      _connectToField(std::move(connectToField)),
      _startWith(std::move(startWith)),
      _additionalFilter(additionalFilter),
      _depthField(depthField),
      _maxDepth(maxDepth) {}

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

    VariablesIdGenerator idGenerator;
    VariablesParseState vps(&idGenerator);

    for (auto&& argument : elem.Obj()) {
        const auto argName = argument.fieldNameStringData();

        if (argName == "startWith") {
            startWith = Expression::parseOperand(argument, vps);
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
        new DocumentSourceGraphLookUp(std::move(from),
                                      std::move(as),
                                      std::move(connectFromField),
                                      std::move(connectToField),
                                      std::move(startWith),
                                      additionalFilter,
                                      depthField,
                                      maxDepth,
                                      expCtx));

    newSource->_variables.reset(new Variables(idGenerator.getIdCount()));

    return std::move(newSource);
}
}  // namespace mongo
