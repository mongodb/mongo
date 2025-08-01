/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/util/str.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace {

static const StringDataSet kValidMetaSorts{"textScore"_sd,
                                           "randVal"_sd,
                                           "geoNearDistance"_sd,
                                           "searchScore"_sd,
                                           "vectorSearchScore"_sd,
                                           "score"_sd};

boost::intrusive_ptr<ExpressionMeta> parseMetaExpression(
    const BSONObj& metaDoc, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    BSONElement metaElem = metaDoc.firstElement();
    // This restriction is due to needing to figure out sort direction. We assume for scored results
    // that users want the most relevant results first.
    uassert(17312,
            "$meta is the only expression supported by $sort right now",
            metaElem.fieldNameStringData() == "$meta");

    uassert(ErrorCodes::FailedToParse,
            "Cannot have additional keys in a $meta sort specification",
            metaDoc.nFields() == 1);

    const auto metaName = metaElem.valueStringDataSafe();

    if (metaName == "searchScore"_sd || metaName == "vectorSearchScore"_sd ||
        metaName == "score"_sd) {
        expCtx->ignoreFeatureInParserOrRejectAndThrow(
            "Sorting by searchScore, vectorSearchScore, or score",
            feature_flags::gFeatureFlagRankFusionFull);
    }
    uassert(31138,
            str::stream() << "Illegal $meta sort: " << metaElem,
            kValidMetaSorts.contains(metaName));

    VariablesParseState vps = expCtx->variablesParseState;
    return static_cast<ExpressionMeta*>(ExpressionMeta::parse(expCtx.get(), metaElem, vps).get());
}
}  // namespace

SortPattern::SortPattern(const BSONObj& obj,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    for (auto&& keyField : obj) {
        auto fieldName = keyField.fieldNameStringData();

        SortPatternPart patternPart;

        if (keyField.type() == BSONType::object) {
            patternPart.expression = parseMetaExpression(keyField.Obj(), expCtx);

            // If sorting by any metadata, sort highest scores first. Note this is weird for
            // geoNearDistance, but it makes sense for every other meta field. If sorting by
            // randVal, order doesn't matter, so just always use descending.
            // One day we can support a syntax to customize this order if we find the motivation.
            patternPart.isAscending = false;

            _sortPattern.push_back(std::move(patternPart));
            continue;
        }

        uassert(15974,
                str::stream() << "Illegal key in $sort specification: " << keyField,
                keyField.isNumber());

        const auto direction = keyField.safeNumberLong();

        uassert(15975,
                "$sort key ordering must be 1 (for ascending) or -1 (for descending)",
                ((direction == 1) || (direction == -1)));

        patternPart.fieldPath = FieldPath{fieldName};
        patternPart.isAscending = (direction > 0);

        const auto [_, inserted] = _paths.insert(patternPart.fieldPath->fullPath());
        uassert(7472500,
                str::stream() << "$sort key must not contain duplicate keys (duplicate: '"
                              << patternPart.fieldPath->fullPath() << "')",
                inserted);

        _sortPattern.push_back(std::move(patternPart));
    }
}

QueryMetadataBitSet SortPattern::metadataDeps(
    DepsTracker::MetadataDependencyValidation availableMetadata) const {
    DepsTracker depsTracker{availableMetadata};
    for (auto&& part : _sortPattern) {
        if (part.expression) {
            expression::addDependencies(part.expression.get(), &depsTracker);
        }
    }

    return depsTracker.metadataDeps();
}

Document SortPattern::serialize(SortKeySerialization serializationMode,
                                const SerializationOptions& options) const {
    MutableDocument keyObj;
    const size_t n = _sortPattern.size();
    for (size_t i = 0; i < n; ++i) {
        if (_sortPattern[i].fieldPath) {
            keyObj.setField(options.serializeFieldPath(*_sortPattern[i].fieldPath),
                            Value(_sortPattern[i].isAscending ? 1 : -1));
        } else {
            // Sorting by an expression, use a made up field name.
            auto computedFieldName = std::string(str::stream() << "$computed" << i);
            switch (serializationMode) {
                case SortKeySerialization::kForExplain:
                case SortKeySerialization::kForPipelineSerialization: {
                    const bool isExplain = (serializationMode == SortKeySerialization::kForExplain);
                    auto opts = SerializationOptions{};
                    if (isExplain) {
                        opts.verbosity =
                            boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner);
                    }
                    keyObj[computedFieldName] = _sortPattern[i].expression->serialize(opts);
                    break;
                }
                case SortKeySerialization::kForSortKeyMerging: {
                    // We need to be able to tell which direction the sort is. Expression sorts are
                    // always descending.
                    keyObj[computedFieldName] = Value(-1);
                    break;
                }
            }
        }
    }
    return keyObj.freeze();
}

void SortPattern::addDependencies(DepsTracker* deps) const {
    for (auto&& keyPart : _sortPattern) {
        if (keyPart.expression) {
            expression::addDependencies(keyPart.expression.get(), deps);
        } else {
            deps->fields.insert(keyPart.fieldPath->fullPath());
        }
    }
}

bool SortPattern::isExtensionOf(const SortPattern& other) const {
    // If the other is longer, this cannot be an extension of it.
    if (_sortPattern.size() < other._sortPattern.size()) {
        return false;
    }
    // For each sortPatternPart in the other sort pattern, make sure we have it as well in order.
    for (unsigned int i = 0; i < other._sortPattern.size(); ++i) {
        if (_sortPattern[i] != other._sortPattern[i]) {
            return false;
        }
    }
    return true;
}

bool isSortOnSingleMetaField(const SortPattern& sortPattern,
                             QueryMetadataBitSet metadataToConsider) {
    // Exactly 1 expression in the sort pattern is needed.
    if (sortPattern.begin() == sortPattern.end() ||
        std::next(sortPattern.begin()) != sortPattern.end()) {
        // 0 parts, or more than 1 part.
        return false;
    }
    const auto& firstAndOnlyPart = *sortPattern.begin();
    if (auto* expr = firstAndOnlyPart.expression.get()) {
        if (auto metaExpr = dynamic_cast<ExpressionMeta*>(expr)) {
            if (metadataToConsider.none()) {
                // Any metadata field.
                return true;
            }
            for (std::size_t i = 1; i < DocumentMetadataFields::kNumFields; ++i) {
                if (metadataToConsider[i] &&
                    metaExpr->getMetaType() == static_cast<DocumentMetadataFields::MetaType>(i)) {
                    return true;
                }
            }
            return false;
        }
    }
    return false;
}
}  // namespace mongo
