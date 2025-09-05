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

#include "mongo/db/pipeline/document_source_unwind.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <iterator>
#include <list>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;
using std::string;

DocumentSourceUnwind::DocumentSourceUnwind(const intrusive_ptr<ExpressionContext>& pExpCtx,
                                           const FieldPath& fieldPath,
                                           bool preserveNullAndEmptyArrays,
                                           const boost::optional<FieldPath>& indexPath,
                                           bool strict)
    : DocumentSource(kStageName, pExpCtx),
      _unwindPath(fieldPath),
      _preserveNullAndEmptyArrays(preserveNullAndEmptyArrays),
      _indexPath(indexPath),
      _strict(strict) {}

REGISTER_DOCUMENT_SOURCE(unwind,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceUnwind::createFromBson,
                         AllowedWithApiStrict::kAlways);
ALLOCATE_DOCUMENT_SOURCE_ID(unwind, DocumentSourceUnwind::id)

const char* DocumentSourceUnwind::getSourceName() const {
    return kStageName.data();
}

intrusive_ptr<DocumentSourceUnwind> DocumentSourceUnwind::create(
    const intrusive_ptr<ExpressionContext>& expCtx,
    const string& unwindPath,
    bool preserveNullAndEmptyArrays,
    const boost::optional<string>& indexPath,
    bool strict) {
    intrusive_ptr<DocumentSourceUnwind> source(
        new DocumentSourceUnwind(expCtx,
                                 FieldPath(unwindPath),
                                 preserveNullAndEmptyArrays,
                                 indexPath ? FieldPath(*indexPath) : boost::optional<FieldPath>(),
                                 strict));
    return source;
}

DocumentSource::GetModPathsReturn DocumentSourceUnwind::getModifiedPaths() const {
    OrderedPathSet modifiedFields{getUnwindPath()};
    if (indexPath()) {
        modifiedFields.insert(indexPath()->fullPath());
    }
    return {GetModPathsReturn::Type::kFiniteSet, std::move(modifiedFields), {}};
}

bool DocumentSourceUnwind::canPushSortBack(const DocumentSourceSort* sort) const {
    // If the sort has a limit, we should also check that _preserveNullAndEmptyArrays is true,
    // otherwise when we swap the limit and unwind, we could end up providing fewer results to the
    // user than expected.
    if (!sort->hasLimit() || preserveNullAndEmptyArrays()) {
        auto modifiedPaths = getModifiedPaths();

        // Checks if any of the $sort's paths depend on the unwind path (or vice versa).
        SortPattern sortKeyPattern = sort->getSortKeyPattern();
        bool sortDependsOnUnwind =
            std::any_of(sortKeyPattern.begin(), sortKeyPattern.end(), [&](auto& sortKey) {
                // If 'sortKey' is a $meta expression, we can do the swap.
                return sortKey.fieldPath && modifiedPaths.canModify(*sortKey.fieldPath);
            });
        return !sortDependsOnUnwind;
    }
    return false;
}

bool DocumentSourceUnwind::canPushLimitBack(const DocumentSourceLimit* limit) const {
    // If _smallestLimitPushedDown is boost::none, then we have not yet pushed a limit down. So no
    // matter what the limit is, we should duplicate and push down. Otherwise we should only push
    // the limit down if it is smaller than the smallest limit we have pushed down so far.
    return !_smallestLimitPushedDown || limit->getLimit() < _smallestLimitPushedDown.value();
}

DocumentSourceContainer::iterator DocumentSourceUnwind::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    tassert(5482200, "DocumentSourceUnwind: itr must point to this object", *itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    // If the following stage is $sort (on a different field), push before $unwind.
    auto next = std::next(itr);
    auto nextSort = dynamic_cast<DocumentSourceSort*>(next->get());
    if (nextSort && canPushSortBack(nextSort)) {
        // If this sort is a top-k sort, we should add a limit after the unwind so that we preserve
        // behavior and not provide more results than requested.
        if (nextSort->hasLimit()) {
            container->insert(
                std::next(next),
                DocumentSourceLimit::create(nextSort->getExpCtx(), nextSort->getLimit().value()));
        }
        std::swap(*itr, *next);
        return itr == container->begin() ? itr : std::prev(itr);
    }

    // If _preserveNullAndEmptyArrays is true, and unwind is followed by a limit, we can add a
    // duplicate limit before the unwind to prevent sources further down the pipeline from giving us
    // more than we need.
    auto nextLimit = dynamic_cast<DocumentSourceLimit*>(next->get());
    if (nextLimit && preserveNullAndEmptyArrays() && canPushLimitBack(nextLimit)) {
        _smallestLimitPushedDown = nextLimit->getLimit();
        auto newStageItr = container->insert(
            itr, DocumentSourceLimit::create(nextLimit->getExpCtx(), nextLimit->getLimit()));
        return newStageItr == container->begin() ? newStageItr : std::prev(newStageItr);
    }

    return std::next(itr);
}

Value DocumentSourceUnwind::serialize(const SerializationOptions& opts) const {
    return Value(
        DOC(getSourceName() << DOC(
                "path" << opts.serializeFieldPathWithPrefix(getUnwindPath())
                       << "preserveNullAndEmptyArrays"
                       << (preserveNullAndEmptyArrays() ? opts.serializeLiteral(true) : Value())
                       << "includeArrayIndex"
                       << (indexPath() ? Value(opts.serializeFieldPath(*indexPath())) : Value()))));
}

DepsTracker::State DocumentSourceUnwind::getDependencies(DepsTracker* deps) const {
    deps->fields.insert(getUnwindPath());
    return DepsTracker::State::SEE_NEXT;
}

intrusive_ptr<DocumentSource> DocumentSourceUnwind::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    // $unwind accepts either the legacy "{$unwind: '$path'}" syntax, or a nested document with
    // extra options.
    string prefixedPathString;
    bool preserveNullAndEmptyArrays = false;
    boost::optional<string> indexPath;
    if (elem.type() == BSONType::object) {
        for (auto&& subElem : elem.Obj()) {
            if (subElem.fieldNameStringData() == "path") {
                uassert(28808,
                        str::stream() << "expected a string as the path for $unwind stage, got "
                                      << typeName(subElem.type()),
                        subElem.type() == BSONType::string);
                prefixedPathString = subElem.str();
            } else if (subElem.fieldNameStringData() == "preserveNullAndEmptyArrays") {
                uassert(28809,
                        str::stream() << "expected a boolean for the preserveNullAndEmptyArrays "
                                         "option to $unwind stage, got "
                                      << typeName(subElem.type()),
                        subElem.type() == BSONType::boolean);
                preserveNullAndEmptyArrays = subElem.Bool();
            } else if (subElem.fieldNameStringData() == "includeArrayIndex") {
                uassert(28810,
                        str::stream() << "expected a non-empty string for the includeArrayIndex "
                                         " option to $unwind stage, got "
                                      << typeName(subElem.type()),
                        subElem.type() == BSONType::string && !subElem.String().empty());
                indexPath = subElem.String();
                uassert(28822,
                        str::stream() << "includeArrayIndex option to $unwind stage should not be "
                                         "prefixed with a '$': "
                                      << (*indexPath),
                        (*indexPath)[0] != '$');
            } else {
                uasserted(28811,
                          str::stream() << "unrecognized option to $unwind stage: "
                                        << subElem.fieldNameStringData());
            }
        }
    } else if (elem.type() == BSONType::string) {
        prefixedPathString = elem.str();
    } else {
        uasserted(
            15981,
            str::stream()
                << "expected either a string or an object as specification for $unwind stage, got "
                << typeName(elem.type()));
    }
    uassert(28812, "no path specified to $unwind stage", !prefixedPathString.empty());

    uassert(28818,
            str::stream() << "path option to $unwind stage should be prefixed with a '$': "
                          << prefixedPathString,
            prefixedPathString[0] == '$');
    string pathString(Expression::removeFieldPrefix(prefixedPathString));
    return DocumentSourceUnwind::create(pExpCtx, pathString, preserveNullAndEmptyArrays, indexPath);
}
}  // namespace mongo
