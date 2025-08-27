/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_densify.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep

#include <algorithm>
#include <iterator>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

using boost::intrusive_ptr;
using boost::optional;
using std::list;
using SortPatternPart = mongo::SortPattern::SortPatternPart;
using Full = mongo::RangeStatement::Full;
using DensifyValue = mongo::DensifyValue;

namespace mongo {

RangeStatement RangeStatement::parse(RangeSpec spec) {
    Value step = spec.getStep();
    ValueComparator comp = ValueComparator();
    uassert(5733401,
            "The step parameter in a range statement must be a strictly positive numeric value",
            step.numeric() && comp.evaluate(step > Value(0)));

    optional<TimeUnit> unit = [&]() {
        if (auto unit = spec.getUnit()) {
            uassert(6586400,
                    "The step parameter in a range statement must be a whole number when "
                    "densifying a date range",
                    step.integral64Bit());
            return optional<TimeUnit>(parseTimeUnit(unit.value()));
        } else {
            return optional<TimeUnit>(boost::none);
        }
    }();

    Bounds bounds = [&]() {
        BSONElement bounds = spec.getBounds().getElement();
        switch (bounds.type()) {
            case BSONType::array: {
                std::vector<BSONElement> array = bounds.Array();

                uassert(5733403,
                        "A bounding array in a range statement must have exactly two elements",
                        array.size() == 2);
                uassert(5733402,
                        "A bounding array must be an ascending array of either two dates or two "
                        "numbers",
                        comp.evaluate(Value(array[0]) <= Value(array[1])));
                if (array[0].isNumber()) {
                    uassert(5733409, "Numeric bounds may not have unit parameter", !unit);
                    uassert(5733406,
                            "A bounding array must contain either both dates or both numeric types",
                            array[1].isNumber());
                    // If these values are types of different sizes, output type may not be
                    // intuitive.
                    uassert(5876900,
                            "Upper bound, lower bound, and step must all have the same type",
                            array[0].type() == array[1].type() &&
                                array[0].type() == step.getType());
                    return Bounds(std::pair<Value, Value>(Value(array[0]), Value(array[1])));
                } else if (array[0].type() == BSONType::date) {
                    uassert(5733405,
                            "A bounding array must contain either both dates or both numeric types",
                            array[1].type() == BSONType::date);
                    uassert(5733410, "A bounding array of dates must specify a unit", unit);
                    return Bounds(std::pair<Date_t, Date_t>(array[0].date(), array[1].date()));
                } else {
                    uasserted(5946800, "Explicit bounds must be numeric or dates");
                }
                MONGO_UNREACHABLE_TASSERT(5946801);
            }
            case BSONType::string: {
                if (bounds.str() == kValFull)
                    return Bounds(Full());
                else if (bounds.str() == kValPartition)
                    return Bounds(Partition());
                else
                    uasserted(5946802,
                              str::stream() << "Bounds string must either be '" << kValFull
                                            << "' or '" << kValPartition << "'");
                MONGO_UNREACHABLE_TASSERT(5946803);
            }
            default:
                uasserted(5733404,
                          "The bounds in a range statement must be the string \'full\', "
                          "\'partition\', or an ascending array of two numbers or two dates");
        }
    }();

    RangeStatement range = RangeStatement(step, bounds, unit);
    return range;
}

REGISTER_DOCUMENT_SOURCE(densify,
                         LiteParsedDocumentSourceDefault::parse,
                         document_source_densify::createFromBson,
                         AllowedWithApiStrict::kAlways);

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalDensify,
                                  LiteParsedDocumentSourceDefault::parse,
                                  DocumentSourceInternalDensify::createFromBson,
                                  true);
ALLOCATE_DOCUMENT_SOURCE_ID(_internalDensify, DocumentSourceInternalDensify::id)

namespace document_source_densify {

list<intrusive_ptr<DocumentSource>> createFromBsonInternal(
    BSONElement elem,
    const intrusive_ptr<ExpressionContext>& expCtx,
    StringData stageName,
    bool isInternal) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << stageName << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);

    auto spec = DensifySpec::parse(elem.embeddedObject(), IDLParserContext(stageName));
    auto rangeStatement = RangeStatement::parse(spec.getRange());

    list<FieldPath> partitions;
    if (spec.getPartitionByFields()) {
        auto partitionFields = (*spec.getPartitionByFields());
        for (auto& partitionField : partitionFields) {
            // The densified field cannot be included in a partition field.
            uassert(8993000,
                    fmt::format("{} '{}' cannot include {} '{}' that is being densified.",
                                DocumentSourceInternalDensify::kPartitionByFieldsFieldName,
                                partitionField,
                                DocumentSourceInternalDensify::kFieldFieldName,
                                spec.getField()),
                    !partitionField.starts_with(spec.getField()));

            // A partition field cannot be included in the densified field.
            uassert(9554500,
                    fmt::format("{} '{}' that is being densified cannot include {} '{}'.",
                                DocumentSourceInternalDensify::kFieldFieldName,
                                spec.getField(),
                                DocumentSourceInternalDensify::kPartitionByFieldsFieldName,
                                partitionField),
                    !spec.getField().starts_with(partitionField));
            partitions.push_back(FieldPath(partitionField));
        }
    }

    FieldPath field = FieldPath(spec.getField());

    if (holds_alternative<RangeStatement::Partition>(rangeStatement.getBounds()) &&
        partitions.empty())
        uasserted(5733408,
                  "One cannot specify the bounds as 'partition' without specifying a non-empty "
                  "array of partitionByFields. You may have meant to specify 'full' bounds.");

    auto densifyStage = create(
        expCtx, std::move(partitions), std::move(field), std::move(rangeStatement), isInternal);
    return densifyStage;
}

list<intrusive_ptr<DocumentSource>> createFromBson(BSONElement elem,
                                                   const intrusive_ptr<ExpressionContext>& expCtx) {
    return createFromBsonInternal(elem, expCtx, kStageName, false);
}

SortPattern getSortPatternForDensify(RangeStatement rangeStatement,
                                     list<FieldPath> partitions,
                                     FieldPath field) {
    // Add partition fields to sort spec.
    std::vector<SortPatternPart> sortParts;
    // We do not add partitions to the sort spec if the range is "full".
    if (!holds_alternative<Full>(rangeStatement.getBounds())) {
        for (const auto& partition : partitions) {
            SortPatternPart part;
            part.fieldPath = partition.fullPath();
            sortParts.push_back(std::move(part));
        }
    }

    // Add field path to sort spec if it is not yet in the sort spec.
    const auto inserted = std::find_if(
        sortParts.begin(), sortParts.end(), [&field](const SortPatternPart& s) -> bool {
            return s.fieldPath->fullPath().compare(field.fullPath()) == 0;
        });
    if (inserted == sortParts.end()) {
        SortPatternPart part;
        part.fieldPath = field.fullPath();
        sortParts.push_back(std::move(part));
    }
    return SortPattern{std::move(sortParts)};
}

list<intrusive_ptr<DocumentSource>> create(const intrusive_ptr<ExpressionContext>& expCtx,
                                           list<FieldPath> partitions,
                                           FieldPath field,
                                           RangeStatement rangeStatement,
                                           bool isInternal) {
    list<intrusive_ptr<DocumentSource>> results;

    // If we're creating an internal stage then we must not desugar and produce a sort stage in
    // addition.
    if (!isInternal) {
        auto sortPattern = getSortPatternForDensify(rangeStatement, partitions, field);
        // Constructing resulting stages.
        results.push_back(DocumentSourceSort::create(expCtx, sortPattern));
    }

    // Constructing resulting stages.
    results.push_back(make_intrusive<DocumentSourceInternalDensify>(
        expCtx, std::move(field), std::move(partitions), std::move(rangeStatement)));

    return results;
}
}  // namespace document_source_densify

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalDensify::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto results = document_source_densify::createFromBsonInternal(elem, expCtx, kStageName, true);
    tassert(5733413,
            "When creating an $_internalDensify stage, only one stage should be returned",
            results.size() == 1);
    return results.front();
}

Value DocumentSourceInternalDensify::serialize(const SerializationOptions& opts) const {
    MutableDocument spec;
    spec[kFieldFieldName] = Value(opts.serializeFieldPath(_field));
    std::vector<Value> serializedPartitionByFields(_partitions.size());
    std::transform(_partitions.begin(),
                   _partitions.end(),
                   serializedPartitionByFields.begin(),
                   [&](FieldPath field) -> Value { return Value(opts.serializeFieldPath(field)); });
    spec[kPartitionByFieldsFieldName] = Value(std::move(serializedPartitionByFields));
    spec[kRangeFieldName] = _range.serialize(opts);
    MutableDocument out;
    out[getSourceName()] = Value(spec.freeze());

    return Value(out.freezeToValue());
}

DensifyValue DensifyValue::increment(const RangeStatement& range) const {
    return visit(OverloadedVisitor{[&](Value val) {
                                       return DensifyValue(uassertStatusOK(
                                           exec::expression::evaluateAdd(val, range.getStep())));
                                   },
                                   [&](Date_t date) {
                                       return DensifyValue(dateAdd(date,
                                                                   range.getUnit().value(),
                                                                   range.getStep().coerceToLong(),
                                                                   timezone()));
                                   }},
                 _value);
}

DensifyValue DensifyValue::decrement(const RangeStatement& range) const {
    return visit(
        OverloadedVisitor{
            [&](Value val) {
                return DensifyValue(
                    uassertStatusOK(exec::expression::evaluateSubtract(val, range.getStep())));
            },
            [&](Date_t date) {
                return DensifyValue(dateAdd(
                    date, range.getUnit().value(), -range.getStep().coerceToLong(), timezone()));
            }},
        _value);
}

DocumentSourceContainer::iterator DocumentSourceInternalDensify::combineSorts(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    if (std::next(itr) == container->end() || itr == container->begin()) {
        return container->end();
    }

    // We can only combine the sorts if we can guarantee the output order will maintain the
    // sort. Densify changes the sort order if partitions are present and range is type 'full'.
    if (_partitions.size() != 0 && holds_alternative<Full>(_range.getBounds())) {
        // We will not maintain sort order.
        return std::next(itr);
    }

    // If $densify was the first stage in the pipeline, there should be a preceding sort.
    tassert(6059802, "$_internalDensify did not have a preceding stage", itr != container->begin());
    // Get the spec of the preceding sort stage. Densify always has a preceding sort, unless
    // the preceding sort was already removed by an earlier stage.
    const auto preSortItr = std::prev(itr);
    const auto preSortStage = dynamic_cast<DocumentSourceSort*>((*preSortItr).get());
    if (!preSortStage || preSortStage->getLimit()) {
        return std::next(itr);
    }

    // Check that the preceding sort was actually generated by $densify, and not by combining the
    // generated sort with a sort earlier in the pipeline.
    auto densifySortPattern =
        document_source_densify::getSortPatternForDensify(_range, _partitions, _field);

    auto preDensifySortPattern = preSortStage->getSortKeyPattern();
    if (densifySortPattern != preDensifySortPattern) {
        return std::next(itr);
    }

    // Get the spec of the following sort stage, if it exists.
    const auto postSortItr = std::next(itr);
    const auto postSortStage = dynamic_cast<DocumentSourceSort*>((*postSortItr).get());
    if (!postSortStage || postSortStage->getLimit()) {
        // If there is not a following sort stage, we won't do any optimization. Return the next
        // stage in the pipeline.
        return std::next(itr);
    }
    auto postDensifySortPattern = postSortStage->getSortKeyPattern();

    // We can only combine the sorts if the sorts are compatible. $densify only preserves a sort on
    // the fields on which it operates, as any other fields will be missing in generated documents.
    if (!preDensifySortPattern.isExtensionOf(postDensifySortPattern)) {
        return std::next(itr);
    }

    // If the post sort is longer, we would have bailed earlier. Remove the sort after the $densify.
    container->erase(postSortItr);

    return std::prev(itr);
}

DocumentSourceContainer::iterator DocumentSourceInternalDensify::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    tassert(6059800, "Expected to optimize $densify stage", *itr == this);

    return combineSorts(itr, container);
}
}  // namespace mongo
