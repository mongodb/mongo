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


#include "mongo/db/query/canonical_query_encoder.h"

#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/geo/geometry_container.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_path.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/matcher/expression_text_noop.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/expression_where.h"
#include "mongo/db/matcher/expression_where_noop.h"
#include "mongo/db/matcher/schema/expression_internal_schema_allowed_properties.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/analyze_regex.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_knob_configuration.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/decorable.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include <s2cellid.h>

#include <boost/iterator/iterator_facade.hpp>
#include <boost/iterator/transform_iterator.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

bool isQueryNegatingEqualToNull(const mongo::MatchExpression* tree) {
    // If the query predicate is null, do not reuse the plan since empty arrays ([]) are
    // encoded as 'null' in the index. Thus we cannot safely invert the index bounds since 'null'
    // has special comparison semantics.
    if (tree->matchType() != MatchExpression::NOT) {
        return false;
    }

    const MatchExpression* child = tree->getChild(0);
    switch (child->matchType()) {
        case MatchExpression::EQ:
        case MatchExpression::GTE:
        case MatchExpression::LTE:
            return static_cast<const ComparisonMatchExpression*>(child)->getData().type() ==
                BSONType::null;

        default:
            return false;
    }
}

namespace {

/**
 * AppendChar provides the compiler with a type for a "appendChar(...)" member function.
 */
template <class BuilderType>
using AppendChar = decltype(std::declval<BuilderType>().appendChar(std::declval<char>()));

/**
 * hasAppendChar is a template variable indicating whether such a void-returning member function
 * exists for a 'BuilderType'.
 */
template <typename BuilderType>
inline constexpr auto hasAppendChar = stdx::is_detected_exact_v<void, AppendChar, BuilderType>;

/**
 * Encode user-provided string. Cache key delimiters seen in the user string are escaped with a
 * backslash.
 */
template <class BuilderType>
void encodeUserString(StringData s, BuilderType* builder) {
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        switch (c) {
            case kEncodeChildrenBegin:
            case kEncodeChildrenEnd:
            case kEncodeChildrenSeparator:
            case kEncodeSectionDelimiter:
            case kEncodeProjectionRequirementSeparator:
            case kEncodeRegexFlagsSeparator:
            case kEncodeParamMarker:
            case kEncodeConstantLiteralMarker:
            case '\\':
                if constexpr (hasAppendChar<BuilderType>) {
                    builder->appendChar('\\');
                } else {
                    *builder << '\\';
                }
                [[fallthrough]];
            default:
                if constexpr (hasAppendChar<BuilderType>) {
                    builder->appendChar(c);
                } else {
                    *builder << c;
                }
        }
    }
}

/**
 * String encoding of MatchExpression::MatchType.
 */
const char* encodeMatchType(MatchExpression::MatchType mt) {
    switch (mt) {
        case MatchExpression::AND:
            return "an";

        case MatchExpression::OR:
            return "or";

        case MatchExpression::NOR:
            return "nr";

        case MatchExpression::NOT:
            return "nt";

        case MatchExpression::ELEM_MATCH_OBJECT:
            return "eo";

        case MatchExpression::ELEM_MATCH_VALUE:
            return "ev";

        case MatchExpression::SIZE:
            return "sz";

        case MatchExpression::LTE:
            return "le";

        case MatchExpression::LT:
            return "lt";

        case MatchExpression::EQ:
            return "eq";

        case MatchExpression::GT:
            return "gt";

        case MatchExpression::GTE:
            return "ge";

        case MatchExpression::REGEX:
            return "re";

        case MatchExpression::MOD:
            return "mo";

        case MatchExpression::EXISTS:
            return "ex";

        case MatchExpression::MATCH_IN:
            return "in";

        case MatchExpression::TYPE_OPERATOR:
            return "ty";

        case MatchExpression::GEO:
            return "go";

        case MatchExpression::WHERE:
            return "wh";

        case MatchExpression::ALWAYS_FALSE:
            return "af";

        case MatchExpression::ALWAYS_TRUE:
            return "at";

        case MatchExpression::GEO_NEAR:
            return "gn";

        case MatchExpression::TEXT:
            return "te";

        case MatchExpression::BITS_ALL_SET:
            return "ls";

        case MatchExpression::BITS_ALL_CLEAR:
            return "lc";

        case MatchExpression::BITS_ANY_SET:
            return "ys";

        case MatchExpression::BITS_ANY_CLEAR:
            return "yc";

        case MatchExpression::EXPRESSION:
            return "xp";

        case MatchExpression::INTERNAL_BUCKET_GEO_WITHIN:
            return "internalBucketGeoWithin";

        case MatchExpression::INTERNAL_EXPR_EQ:
            return "eeq";

        case MatchExpression::INTERNAL_EXPR_GT:
            return "egt";

        case MatchExpression::INTERNAL_EXPR_GTE:
            return "ege";

        case MatchExpression::INTERNAL_EXPR_LT:
            return "elt";

        case MatchExpression::INTERNAL_EXPR_LTE:
            return "ele";

        case MatchExpression::INTERNAL_EQ_HASHED_KEY:
            return "ieh";

        case MatchExpression::INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX:
            return "internalSchemaAllElemMatchFromIndex";

        case MatchExpression::INTERNAL_SCHEMA_ALLOWED_PROPERTIES:
            return "internalSchemaAllowedProperties";

        case MatchExpression::INTERNAL_SCHEMA_BIN_DATA_ENCRYPTED_TYPE:
            return "internalSchemaBinDataEncryptedType";

        case MatchExpression::INTERNAL_SCHEMA_BIN_DATA_FLE2_ENCRYPTED_TYPE:
            return "internalSchemaBinDataFLE2EncryptedType";

        case MatchExpression::INTERNAL_SCHEMA_BIN_DATA_SUBTYPE:
            return "internalSchemaBinDataSubType";

        case MatchExpression::INTERNAL_SCHEMA_COND:
            return "internalSchemaCond";

        case MatchExpression::INTERNAL_SCHEMA_EQ:
            return "internalSchemaEq";

        case MatchExpression::INTERNAL_SCHEMA_FMOD:
            return "internalSchemaFmod";

        case MatchExpression::INTERNAL_SCHEMA_MIN_ITEMS:
            return "internalSchemaMinItems";

        case MatchExpression::INTERNAL_SCHEMA_MAX_ITEMS:
            return "internalSchemaMaxItems";

        case MatchExpression::INTERNAL_SCHEMA_UNIQUE_ITEMS:
            return "internalSchemaUniqueItems";

        case MatchExpression::INTERNAL_SCHEMA_XOR:
            return "internalSchemaXor";

        case MatchExpression::INTERNAL_SCHEMA_OBJECT_MATCH:
            return "internalSchemaObjectMatch";

        case MatchExpression::INTERNAL_SCHEMA_ROOT_DOC_EQ:
            return "internalSchemaRootDocEq";

        case MatchExpression::INTERNAL_SCHEMA_MIN_LENGTH:
            return "internalSchemaMinLength";

        case MatchExpression::INTERNAL_SCHEMA_MAX_LENGTH:
            return "internalSchemaMaxLength";

        case MatchExpression::INTERNAL_SCHEMA_MIN_PROPERTIES:
            return "internalSchemaMinProperties";

        case MatchExpression::INTERNAL_SCHEMA_MAX_PROPERTIES:
            return "internalSchemaMaxProperties";

        case MatchExpression::INTERNAL_SCHEMA_MATCH_ARRAY_INDEX:
            return "internalSchemaMatchArrayIndex";

        case MatchExpression::INTERNAL_SCHEMA_TYPE:
            return "internalSchemaType";

        default:
            MONGO_UNREACHABLE;
    }
}

/**
 * Encodes GEO match expression.
 * Encoding includes:
 * - type of geo query (within/intersect/near)
 * - geometry type
 * - CRS (flat or spherical)
 */
void encodeGeoMatchExpression(const GeometryContainer& geo,
                              GeoExpression::Predicate pred,
                              StringBuilder* keyBuilder) {
    // Type of geo query.
    switch (pred) {
        case GeoExpression::WITHIN:
            *keyBuilder << "wi";
            break;
        case GeoExpression::INTERSECT:
            *keyBuilder << "in";
            break;
        case GeoExpression::INVALID:
            *keyBuilder << "id";
            break;
    }

    // Geometry type.
    // Only one of the shared_ptrs in GeoContainer may be non-NULL.
    *keyBuilder << geo.getDebugType();

    // CRS (flat or spherical)
    if (FLAT == geo.getNativeCRS()) {
        *keyBuilder << "fl";
    } else if (SPHERE == geo.getNativeCRS()) {
        *keyBuilder << "sp";
    } else if (STRICT_SPHERE == geo.getNativeCRS()) {
        *keyBuilder << "ss";
    } else {
        LOGV2_ERROR(23849,
                    "Unknown CRS type in geometry",
                    "crsType"_attr = (int)geo.getNativeCRS(),
                    "geometryType"_attr = geo.getDebugType());
        MONGO_UNREACHABLE;
    }
}

/**
 * Encodes GEO_NEAR match expression.
 * Encode:
 * - isNearSphere
 * - CRS (flat or spherical)
 */
void encodeGeoNearMatchExpression(const GeoNearMatchExpression* tree, StringBuilder* keyBuilder) {
    const GeoNearExpression& nearQuery = tree->getData();

    // isNearSphere
    *keyBuilder << (nearQuery.isNearSphere ? "ns" : "nr");

    // CRS (flat or spherical or strict-winding spherical)
    switch (nearQuery.centroid->crs) {
        case FLAT:
            *keyBuilder << "fl";
            break;
        case SPHERE:
            *keyBuilder << "sp";
            break;
        case STRICT_SPHERE:
            *keyBuilder << "ss";
            break;
        case UNSET:
            LOGV2_ERROR(23850,
                        "Unknown CRS type in point geometry for near query",
                        "crsType"_attr = (int)nearQuery.centroid->crs);
            MONGO_UNREACHABLE;
            break;
    }
}

template <class T>
char encodeEnum(T val) {
    // Ensure val can be encoded as a digit between '0' and '9' inclusive.
    invariant(static_cast<int>(val) < 10);
    return static_cast<char>(val) + '0';
}

void encodeCollation(const CollatorInterface* collation, StringBuilder* keyBuilder) {
    *keyBuilder << kEncodeSectionDelimiter;
    if (!collation) {
        return;
    }

    const Collation& spec = collation->getSpec();

    *keyBuilder << spec.getLocale();
    *keyBuilder << spec.getCaseLevel();

    // Ensure that we can encode this value with a single ascii byte '0' through '9'.
    *keyBuilder << encodeEnum(spec.getCaseFirst());
    *keyBuilder << encodeEnum(spec.getStrength());
    *keyBuilder << spec.getNumericOrdering();

    *keyBuilder << encodeEnum(spec.getAlternate());
    *keyBuilder << encodeEnum(spec.getMaxVariable());
    *keyBuilder << spec.getNormalization();
    *keyBuilder << spec.getBackwards();

    // We do not encode 'spec.version' because query shape strings are never persisted, and need
    // not be stable between versions.
}

template <class RegexIterator>
void encodeRegexFlagsForMatch(RegexIterator first, RegexIterator last, StringBuilder* keyBuilder) {
    // We sort the flags, so that queries with the same regex flags in different orders will have
    // the same shape. We then add them to a set, so that identical flags across multiple regexes
    // will be deduplicated and the resulting set of unique flags will be ordered consistently.
    // Regex flags are not validated at parse-time, so we also ensure that only valid flags
    // contribute to the encoding.
    static const auto maxValidFlags = RegexMatchExpression::kValidRegexFlags.size();
    std::set<char> flags;
    for (auto it = first; it != last && flags.size() < maxValidFlags; ++it) {
        auto inserter = std::inserter(flags, flags.begin());
        std::copy_if((*it)->getFlags().begin(), (*it)->getFlags().end(), inserter, [](auto flag) {
            return RegexMatchExpression::kValidRegexFlags.count(flag);
        });
    }
    if (!flags.empty()) {
        *keyBuilder << kEncodeRegexFlagsSeparator;
        for (const auto& flag : flags) {
            invariant(RegexMatchExpression::kValidRegexFlags.count(flag));
            encodeUserString(StringData(&flag, 1), keyBuilder);
        }
        *keyBuilder << kEncodeRegexFlagsSeparator;
    }
}

// Helper overload to prepare a vector of unique_ptrs for the heavy-lifting function above.
void encodeRegexFlagsForMatch(const std::vector<std::unique_ptr<RegexMatchExpression>>& regexes,
                              StringBuilder* keyBuilder) {
    const auto transformFunc = [](const auto& regex) {
        return regex.get();
    };
    encodeRegexFlagsForMatch(boost::make_transform_iterator(regexes.begin(), transformFunc),
                             boost::make_transform_iterator(regexes.end(), transformFunc),
                             keyBuilder);
}
// Helper that passes a range covering the entire source set into the heavy-lifting function above.
void encodeRegexFlagsForMatch(const std::vector<const RegexMatchExpression*>& regexes,
                              StringBuilder* keyBuilder) {
    encodeRegexFlagsForMatch(regexes.begin(), regexes.end(), keyBuilder);
}

/**
 * Traverses expression tree pre-order.
 * Appends an encoding of each node's match type and path name
 * to the output stream.
 */
void encodeKeyForMatch(const MatchExpression* tree, StringBuilder* keyBuilder) {
    invariant(keyBuilder);

    // Encode match type and path.
    *keyBuilder << encodeMatchType(tree->matchType());

    encodeUserString(tree->path(), keyBuilder);

    switch (tree->matchType()) {
        // Geo operators require additional encoding.
        case MatchExpression::GEO: {
            auto geoTree = static_cast<const GeoMatchExpression*>(tree);
            encodeGeoMatchExpression(geoTree->getGeoExpression().getGeometry(),
                                     geoTree->getGeoExpression().getPred(),
                                     keyBuilder);
            break;
        }
        case MatchExpression::GEO_NEAR:
            encodeGeoNearMatchExpression(static_cast<const GeoNearMatchExpression*>(tree),
                                         keyBuilder);
            break;
        case MatchExpression::INTERNAL_BUCKET_GEO_WITHIN: {
            auto geoTree = static_cast<const InternalBucketGeoWithinMatchExpression*>(tree);
            encodeGeoMatchExpression(geoTree->getGeoContainer(), GeoExpression::WITHIN, keyBuilder);
            break;
        }
        case MatchExpression::REGEX:
            // We encode regular expression flags such that different options produce different
            // shapes.
            encodeRegexFlagsForMatch({static_cast<const RegexMatchExpression*>(tree)}, keyBuilder);
            break;
        case MatchExpression::MATCH_IN: {
            const auto* inMatch = static_cast<const InMatchExpression*>(tree);
            if (!inMatch->getRegexes().empty()) {
                // Append '_re' to distinguish an $in without regexes from an $in with regexes.
                encodeUserString("_re"_sd, keyBuilder);
                encodeRegexFlagsForMatch(inMatch->getRegexes(), keyBuilder);
            }
            break;
        }
        case MatchExpression::AND:
        case MatchExpression::OR:
        case MatchExpression::ELEM_MATCH_OBJECT:
        case MatchExpression::ELEM_MATCH_VALUE:
        case MatchExpression::SIZE:
        case MatchExpression::EQ:
        case MatchExpression::LTE:
        case MatchExpression::LT:
        case MatchExpression::GT:
        case MatchExpression::GTE:
        case MatchExpression::MOD:
        case MatchExpression::EXISTS:
        case MatchExpression::BITS_ALL_SET:
        case MatchExpression::BITS_ALL_CLEAR:
        case MatchExpression::BITS_ANY_SET:
        case MatchExpression::BITS_ANY_CLEAR:
        case MatchExpression::NOT:
        case MatchExpression::NOR:
        case MatchExpression::TYPE_OPERATOR:
        case MatchExpression::WHERE:
        case MatchExpression::EXPRESSION:
        case MatchExpression::ALWAYS_FALSE:
        case MatchExpression::ALWAYS_TRUE:
        case MatchExpression::TEXT:
        case MatchExpression::INTERNAL_2D_POINT_IN_ANNULUS:
        case MatchExpression::INTERNAL_EXPR_EQ:
        case MatchExpression::INTERNAL_EXPR_GT:
        case MatchExpression::INTERNAL_EXPR_GTE:
        case MatchExpression::INTERNAL_EXPR_LT:
        case MatchExpression::INTERNAL_EXPR_LTE:
        case MatchExpression::INTERNAL_EQ_HASHED_KEY:
        case MatchExpression::INTERNAL_SCHEMA_ALLOWED_PROPERTIES:
        case MatchExpression::INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX:
        case MatchExpression::INTERNAL_SCHEMA_BIN_DATA_ENCRYPTED_TYPE:
        case MatchExpression::INTERNAL_SCHEMA_BIN_DATA_FLE2_ENCRYPTED_TYPE:
        case MatchExpression::INTERNAL_SCHEMA_BIN_DATA_SUBTYPE:
        case MatchExpression::INTERNAL_SCHEMA_COND:
        case MatchExpression::INTERNAL_SCHEMA_EQ:
        case MatchExpression::INTERNAL_SCHEMA_FMOD:
        case MatchExpression::INTERNAL_SCHEMA_MATCH_ARRAY_INDEX:
        case MatchExpression::INTERNAL_SCHEMA_MAX_ITEMS:
        case MatchExpression::INTERNAL_SCHEMA_MAX_LENGTH:
        case MatchExpression::INTERNAL_SCHEMA_MAX_PROPERTIES:
        case MatchExpression::INTERNAL_SCHEMA_MIN_ITEMS:
        case MatchExpression::INTERNAL_SCHEMA_MIN_LENGTH:
        case MatchExpression::INTERNAL_SCHEMA_MIN_PROPERTIES:
        case MatchExpression::INTERNAL_SCHEMA_OBJECT_MATCH:
        case MatchExpression::INTERNAL_SCHEMA_ROOT_DOC_EQ:
        case MatchExpression::INTERNAL_SCHEMA_TYPE:
        case MatchExpression::INTERNAL_SCHEMA_UNIQUE_ITEMS:
        case MatchExpression::INTERNAL_SCHEMA_XOR:
            break;
    }

    // If the query predicate is minKey or maxKey it can't use the same plan as other GT/LT
    // queries. If the index is multikey and the query involves one of these values, it needs
    // to use INEXACT_FETCH and the bounds can't be inverted. Therefore these need their own
    // shape.
    if (tree->isGTMinKey())
        *keyBuilder << "min";
    else if (tree->isLTMaxKey())
        *keyBuilder << "max";

    // If the query predicate involves comparison to null, do not reuse the plan since empty arrays
    // ([]) are encoded as null in the index. Thus we cannot safely invert the index bounds.
    if (isQueryNegatingEqualToNull(tree)) {
        *keyBuilder << "not_eq_null";
    }

    // Traverse child nodes.
    // Enclose children in [].
    if (tree->numChildren() > 0) {
        *keyBuilder << kEncodeChildrenBegin;
    }
    // Use comma to separate children encoding.
    for (size_t i = 0; i < tree->numChildren(); ++i) {
        if (i > 0) {
            *keyBuilder << kEncodeChildrenSeparator;
        }
        encodeKeyForMatch(tree->getChild(i), keyBuilder);
    }

    if (tree->numChildren() > 0) {
        *keyBuilder << kEncodeChildrenEnd;
    }
}

/**
 * Encodes sort order into cache key. Sort order is normalized because it provided by
 * FindCommandRequest.
 */
void encodeKeyForSort(const BSONObj& sortObj, StringBuilder* keyBuilder) {
    *keyBuilder << kEncodeSectionDelimiter;
    if (sortObj.isEmpty()) {
        return;
    }

    BSONObjIterator it(sortObj);
    while (it.more()) {
        BSONElement elt = it.next();
        // $meta text score
        if (query_request_helper::isTextScoreMeta(elt)) {
            *keyBuilder << "t";
        }
        // Ascending
        else if (elt.numberInt() == 1) {
            *keyBuilder << "a";
        }
        // Descending
        else {
            *keyBuilder << "d";
        }
        encodeUserString(elt.fieldName(), keyBuilder);

        // Sort argument separator
        if (it.more()) {
            *keyBuilder << ",";
        }
    }
}

/**
 * Encodes projection AST into a cache key.
 *
 * For projections which have a finite set of required fields (inclusion-only projections), encodes
 * those field names in order.
 *
 * For projections which require the entire document (exclusion projections, projections with
 * expressions), the projection section is empty.
 */
void encodeKeyForProj(const projection_ast::Projection* proj, StringBuilder* keyBuilder) {
    *keyBuilder << kEncodeSectionDelimiter;
    if (!proj || proj->requiresDocument()) {
        // Don't encode anything for the projection section to indicate the entire document is
        // required.
        return;
    }

    const auto& requiredFields = proj->getRequiredFields();

    // If the only requirement is that $sortKey be included with some value, we just act as if the
    // entire document is needed.
    if (requiredFields.size() == 1 && *requiredFields.begin() == "$sortKey") {
        return;
    }

    // Encode the fields required by the projection in order.
    bool isFirst = true;
    for (auto&& requiredField : requiredFields) {
        invariant(!requiredField.empty());

        // Internal callers (e.g, from mongos) may add "$sortKey" to the projection. This is not
        // part of the user query, and therefore are not considered part of the cache key.
        if (requiredField == "$sortKey") {
            continue;
        }

        if (!isFirst) {
            *keyBuilder << kEncodeProjectionRequirementSeparator;
        }
        encodeUserString(requiredField, keyBuilder);
        isFirst = false;
    }
}

/**
 * Encodes relevant CanonicalDistinct properties into cache key, notably distinct key and sort
 * requirement. Projection spec is not encoded because it depends on the distinct key and filter
 * which are already encoded.
 */
void encodeKeyForDistinct(const boost::optional<CanonicalDistinct>& distinct,
                          StringBuilder* keyBuilder,
                          bool isShardFilteringDistinctScanEnabled) {
    // Ensure the delimiter is always included in the encoded plan cache key, regardless of the
    // feature gate state. This avoids inconsistencies in plan cache keys across different FCVs
    // during upgrades or downgrades.
    *keyBuilder << kEncodeSectionDelimiter;
    if (!isShardFilteringDistinctScanEnabled || !distinct.has_value()) {
        return;
    }
    encodeUserString(distinct->getKey(), keyBuilder);
    *keyBuilder << distinct->isDistinctScanDirectionFlipped();
    if (distinct->getSortRequirement()) {
        const auto& sortPattern = distinct->getSortRequirement().get();
        auto delimiter = "";
        for (const auto& part : sortPattern) {
            if (part.fieldPath) {
                *keyBuilder << delimiter << (part.isAscending ? "a" : "d");
                encodeUserString(part.fieldPath->fullPath(), keyBuilder);
                delimiter = ",";
            }
        }
    }
}

void encodeKeyForPipelineStage(DocumentSource* docSource,
                               std::vector<Value>& serializedArray,
                               BufBuilder* bufBuilder) {
    bufBuilder->appendChar(kEncodeSectionDelimiter);
    serializedArray.clear();
    docSource->serializeToArray(serializedArray);

    for (const auto& value : serializedArray) {
        tassert(6443201,
                "Expected pipeline stage to serialize to objects",
                value.getType() == BSONType::object);
        const BSONObj bson = value.getDocument().toBson();
        bufBuilder->appendBuf(bson.objdata(), bson.objsize());
    }
}

/**
 * Approximate the number of documents to be processed into a small, medium or large category. Best
 * plans for limit: 10 and limit: 1000 may be different. This allows us to cache different plans for
 * different cases without unbounded growth of plan cache for each skip and limit value.
 */
char getLimitSkipCategory(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                          boost::optional<int64_t> skip,
                          boost::optional<int64_t> limit) {
    if (limit.value_or(0) == 1 && !skip) {
        return '1';
    }

    size_t limitSkipSum;
    bool hasOverflowed = overflow::add(static_cast<size_t>(skip.value_or(0)),
                                       static_cast<size_t>(limit.value_or(0)),
                                       &limitSkipSum);
    if (hasOverflowed) {
        return 'l';
    }
    size_t planEvaluationMaxResults =
        expCtx->getQueryKnobConfiguration().getPlanEvaluationMaxResultsForOp();
    if (limitSkipSum < planEvaluationMaxResults) {
        return 's';
    } else if (limitSkipSum < 10 * planEvaluationMaxResults) {
        return 'm';
    } else {
        return 'l';
    }
}

void encodeLimitSkip(const CanonicalQuery& cq, BufBuilder* bufBuilder) {
    boost::optional<int64_t> skip = cq.getFindCommandRequest().getSkip();
    boost::optional<int64_t> limit = cq.getFindCommandRequest().getLimit();
    if (!limit && !skip) {
        return;
    }
    if (cq.shouldParameterizeLimitSkip()) {
        bufBuilder->appendChar('p');
        bufBuilder->appendChar(skip ? 1 : 0);
        bufBuilder->appendChar(limit ? 1 : 0);
        bufBuilder->appendChar(getLimitSkipCategory(cq.getExpCtx(), skip, limit));
    } else {
        bufBuilder->appendChar('c');
        bufBuilder->appendNum(skip.value_or(0));
        bufBuilder->appendNum(limit.value_or(0));
    }
}

void encodeFindCommandRequest(const CanonicalQuery& cq, BufBuilder* bufBuilder) {
    encodeLimitSkip(cq, bufBuilder);

    // Encode a OptionalBool value - 'n' if the value is not specified, 't' for true, and 'f' for
    // false.
    auto encodeOptionalBool = [bufBuilder](const mongo::OptionalBool& val) {
        if (!val.has_value()) {
            bufBuilder->appendChar('n');
        } else if (val) {
            bufBuilder->appendChar('t');
        } else {
            bufBuilder->appendChar('f');
        }
    };
    const auto& findCommand = cq.getFindCommandRequest();
    encodeOptionalBool(findCommand.getAllowDiskUse());
    encodeOptionalBool(findCommand.getReturnKey());
    encodeOptionalBool(findCommand.getRequestResumeToken());
    encodeOptionalBool(findCommand.getTailable());

    // Encode a BSON object by its raw data.
    auto encodeBSONObj = [&bufBuilder](const BSONObj& obj) {
        bufBuilder->appendBuf(obj.objdata(), obj.objsize());
    };
    encodeBSONObj(findCommand.getResumeAfter());

    encodeBSONObj(findCommand.getStartAt());

    // Read concern "available" results in SBE plans that do not perform shard filtering, so it must
    // be encoded differently from other read concerns.
    bool isAvailableReadConcern{false};
    if (const auto& readConcern = findCommand.getReadConcern()) {
        isAvailableReadConcern =
            readConcern->getLevel() == repl::ReadConcernLevel::kAvailableReadConcern;
    }
    bufBuilder->appendChar(isAvailableReadConcern ? 't' : 'f');
}

/**
 * A visitor intended for use in combination with the corresponding walker class below to encode a
 * 'MatchExpression' into the SBE plan cache key.
 *
 * Handles potentially parameterized queries, in which case parameter markers are encoded into the
 * cache key in place of the actual constant values.
 */
class MatchExpressionSbePlanCacheKeySerializationVisitor final
    : public MatchExpressionConstVisitor {
public:
    explicit MatchExpressionSbePlanCacheKeySerializationVisitor(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, BufBuilder* builder, bool hasSort)
        : _expCtx(expCtx), _builder(builder), _hasSort(hasSort) {}

    void visit(const BitsAllClearMatchExpression* expr) final {
        encodeBitTestExpression(expr);
    }
    void visit(const BitsAllSetMatchExpression* expr) final {
        encodeBitTestExpression(expr);
    }
    void visit(const BitsAnyClearMatchExpression* expr) final {
        encodeBitTestExpression(expr);
    }
    void visit(const BitsAnySetMatchExpression* expr) final {
        encodeBitTestExpression(expr);
    }

    void visit(const ExistsMatchExpression* expr) final {
        encodeRhs(expr);
    }

    void visit(const ExprMatchExpression* expr) final {
        encodeFull(expr);
    }

    void visit(const EqualityMatchExpression* expr) final {
        encodeSingleParamPathNode(expr);
    }
    void visit(const GTEMatchExpression* expr) final {
        encodeSingleParamPathNode(expr);
    }
    void visit(const GTMatchExpression* expr) final {
        encodeSingleParamPathNode(expr);
    }
    void visit(const LTEMatchExpression* expr) final {
        encodeSingleParamPathNode(expr);
    }
    void visit(const LTMatchExpression* expr) final {
        encodeSingleParamPathNode(expr);
    }

    void visit(const InMatchExpression* expr) final {
        encodeSingleParamPathNode(expr);

        // Encode the number of unique $in values as part of the plan cache key. If the query is
        // optimized by exploding for sort, the number of unique elements in $in determines how many
        // merge branches we get in the query plan. If there is no sort, this is not necessary.
        if (expr->getInputParamId() && _hasSort) {
            size_t maxScansToExplode =
                _expCtx->getQueryKnobConfiguration().getMaxScansToExplodeForOp();
            // Assume that $in has n elements.
            // If n is less than or equal to maxScansToExplode, then it is possible that explode for
            // sort optimization will be used, so we need to add n to plan cache key.
            // If n is greater than maxScansToExplode, then we can't explode it for sort. So we can
            // use the same value of (maxScansToExplode + 1) for all queries, so they can share a
            // plan cache entry.
            _builder->appendNum(std::min(maxScansToExplode + 1, expr->getEqualities().size()));
        }
    }

    void visit(const ModMatchExpression* expr) final {
        auto divisorParam = expr->getDivisorInputParamId();
        auto remainderParam = expr->getRemainderInputParamId();
        if (divisorParam) {
            tassert(6512902,
                    "$mod expression should have divisor and remainder params",
                    remainderParam);

            encodeParamMarker(*divisorParam);
            encodeParamMarker(*remainderParam);
        } else {
            tassert(6579300,
                    "If divisor param is not set in $mod expression reminder param must be unset "
                    "as well",
                    !remainderParam);
            encodeFull(expr);
        }
    }

    void visit(const RegexMatchExpression* expr) final {
        auto sourceRegexParam = expr->getSourceRegexInputParamId();
        auto compiledRegexParam = expr->getCompiledRegexInputParamId();
        if (sourceRegexParam) {
            tassert(6512904,
                    "regex expression should have source and compiled params",
                    compiledRegexParam);

            encodeParamMarker(*sourceRegexParam);
            encodeParamMarker(*compiledRegexParam);

            // Encode a discriminator so that a "simple" regex which is exactly convertible into
            // index bounds has a different shape from a non-simple regex.
            //
            // We don't actually need to know the contents of the prefix string, so we ignore the
            // first member of the pair.
            auto [_, isExact] = analyze_regex::getRegexPrefixMatch(expr->getString().c_str(),
                                                                   expr->getFlags().c_str());
            _builder->appendChar(kEncodeBoundsTightnessDiscriminator);
            _builder->appendChar(static_cast<char>(isExact));
        } else {
            tassert(6579301,
                    "If source param is not set in $regex expression compiled param must be unset "
                    "as well",
                    !compiledRegexParam);
            encodeFull(expr);
        }
    }

    void visit(const SizeMatchExpression* expr) final {
        encodeSingleParamPathNode(expr);
    }

    void visit(const TextMatchExpression* expr) final {
        encodeFull(expr);
    }
    void visit(const TextNoOpMatchExpression* expr) final {
        encodeFull(expr);
    }

    void visit(const TypeMatchExpression* expr) final {
        encodeSingleParamPathNode(expr);
    }

    void visit(const WhereMatchExpression* expr) final {
        encodeSingleParamNode(expr);
    }
    void visit(const WhereNoOpMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142109);
    }

    /**
     * Nothing needs to be encoded for these nodes beyond their type, their path (if they have one),
     * and their children.
     */
    void visit(const AlwaysFalseMatchExpression* expr) final {}
    void visit(const AlwaysTrueMatchExpression* expr) final {}
    void visit(const AndMatchExpression* expr) final {}
    void visit(const ElemMatchValueMatchExpression* matchExpr) final {}
    void visit(const ElemMatchObjectMatchExpression* matchExpr) final {}
    void visit(const NorMatchExpression* expr) final {}
    void visit(const NotMatchExpression* expr) final {}
    void visit(const OrMatchExpression* expr) final {}
    // The 'InternalExpr*' match expressions are generated internally from a $expr, so they do not
    // need to contribute anything else to the cache key.
    void visit(const InternalExprEqMatchExpression* expr) final {}
    void visit(const InternalExprGTEMatchExpression* expr) final {}
    void visit(const InternalExprGTMatchExpression* expr) final {}
    void visit(const InternalExprLTEMatchExpression* expr) final {}
    void visit(const InternalExprLTMatchExpression* expr) final {}


    /**
     * These node types are not yet supported in SBE.
     */
    void visit(const InternalEqHashedKey* expr) final {
        MONGO_UNREACHABLE_TASSERT(7281402);
    }
    void visit(const GeoMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142111);
    }
    void visit(const GeoNearMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142112);
    }
    void visit(const InternalBucketGeoWithinMatchExpression* expr) final {
        // This is only used for time-series collections, but SBE isn't yet used for querying
        // time-series collections.
        MONGO_UNREACHABLE_TASSERT(6142113);
    }
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142114);
    }
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142115);
    }
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142116);
    }
    void visit(const InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6364303);
    }
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142117);
    }
    void visit(const InternalSchemaCondMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142118);
    }
    void visit(const InternalSchemaEqMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142119);
    }
    void visit(const InternalSchemaFmodMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142120);
    }
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142121);
    }
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142122);
    }
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142123);
    }
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142124);
    }
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142125);
    }
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142126);
    }
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142127);
    }
    void visit(const InternalSchemaObjectMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142128);
    }
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142129);
    }
    void visit(const InternalSchemaTypeExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142130);
    }
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142131);
    }
    void visit(const InternalSchemaXorMatchExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142132);
    }
    // Used in the implementation of geoNear, which is not yet supported in SBE.
    void visit(const TwoDPtInAnnulusExpression* expr) final {
        MONGO_UNREACHABLE_TASSERT(6142133);
    }

private:
    /**
     * Encodes a 'PathMatchExpression' node of type T whose constant can be replaced with a single
     * parameter marker. If the parameter marker is not present, encodes the node's BSON constant
     * into the cache key.
     */
    template <typename T,
              typename = std::enable_if_t<std::is_convertible_v<T*, PathMatchExpression*>>>
    void encodeSingleParamPathNode(const T* expr) {
        if (expr->getInputParamId()) {
            encodeParamMarker(*expr->getInputParamId());
        } else {
            encodeRhs(expr);
        }
    }

    /**
     * Encodes a non-path 'MatchExpression' node of type T whose constant can be replaced with a
     * single parameter marker. If the parameter marker is not present, encodes the entire node into
     * the cache key.
     */
    template <typename T>
    void encodeSingleParamNode(const T* expr) {
        static_assert(!std::is_convertible_v<T*, PathMatchExpression*>);
        if (expr->getInputParamId()) {
            encodeParamMarker(*expr->getInputParamId());
        } else {
            encodeFull(expr);
        }
    }

    void encodeBitTestExpression(const BitTestMatchExpression* expr) {
        auto bitPositionsParam = expr->getBitPositionsParamId();
        auto bitMaskParam = expr->getBitMaskParamId();
        if (bitPositionsParam) {

            tassert(6512906,
                    "bit-test expression should have bit positions and bitmask params",
                    bitMaskParam);

            encodeParamMarker(*bitPositionsParam);
            encodeParamMarker(*bitMaskParam);
        } else {
            tassert(6579302,
                    "If positions param is not set in a bit-test expression bitmask param must be "
                    "unset as well",
                    !bitMaskParam);
            encodeFull(expr);
        }
    }

    /**
     * Adds a special parameter marker byte to the cache key, followed by a four byte integer for
     * the parameter id.
     */
    void encodeParamMarker(MatchExpression::InputParamId paramId) {
        _builder->appendChar(kEncodeParamMarker);
        _builder->appendNum(paramId);
    }

    /**
     * For path match expressions which can be written as {"some.path": {$operator: <RHS>}}, encodes
     * the right-hand side portion of the expression verbatim. Illegal to call if 'expr' has a
     * parameter marker.
     */
    void encodeRhs(const PathMatchExpression* expr) {
        // Call getSerializedRightHandSide() with 'inMatchExprSortAndDedupElements' set to false.
        SerializationOptions opts;
        opts.inMatchExprSortAndDedupElements = false;

        encodeHelper(expr->getSerializedRightHandSide(opts));
    }

    /**
     * Similar to 'encodeRhs()' above, but for non-path match expressions. In this case, rather than
     * encode just the right-hand side, we call 'serialize()' to get a serialized version of the
     * full expression, and encode the result into the plan cache key. Illegal to call if 'expr' has
     * a parameter marker.
     */
    void encodeFull(const MatchExpression* expr) {
        encodeHelper(expr->serialize());
    }

    void encodeHelper(const BSONObj& toEncode) {
        tassert(6142102, "expected object to encode to be non-empty", !toEncode.isEmpty());
        BSONObjIterator objIter{toEncode};
        BSONElement firstElem = objIter.next();
        tassert(6142103, "expected object to encode to have exactly one element", !objIter.more());
        encodeBsonValue(firstElem);
    }

    /**
     * Encodes a special byte to mark a constant, followed by a byte for the BSON type of 'elem',
     * followed by the bytes of the value part of 'elem' (for types that have such a value).
     *
     * Note that the element's field name is not encoded, just the type and value.
     */
    void encodeBsonValue(BSONElement elem) {
        _builder->appendChar(kEncodeConstantLiteralMarker);
        _builder->appendChar(stdx::to_underlying(elem.type()));
        _builder->appendBuf(elem.value(), elem.valuesize());
    }

    const boost::intrusive_ptr<ExpressionContext>& _expCtx;
    BufBuilder* const _builder;
    // Whether there is a sort absorbed by the Canonical query. Note: '_hasSort' is true only when
    // there is a sort that can be used for explode for sort optimization. For a $match stage,
    // '_hasSort' should be false since $match does not perform index selection. In cases where a
    // $sort is not absorbed by the canonical query '_hasSort' should be false since we only perform
    // explode for sort using the sort in canonical query.
    bool _hasSort;
};

/**
 * A tree walker which walks a 'MatchExpression' tree and encodes the corresponding portion of the
 * SBE plan cache key into 'builder'.
 *
 * Handles potentially parameterized queries, in which case parameter markers are encoded into the
 * cache key in place of the actual constant values.
 */
class MatchExpressionSbePlanCacheKeySerializationWalker {
public:
    explicit MatchExpressionSbePlanCacheKeySerializationWalker(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, BufBuilder* builder, bool hasSort)
        : _builder{builder}, _visitor{expCtx, _builder, hasSort} {
        invariant(_builder);
    }

    void preVisit(const MatchExpression* expr) {
        // Encode the type of the node as well as the path (if there is a non-empty path).
        _builder->appendCStr(encodeMatchType(expr->matchType()));
        encodeUserString(expr->path(), _builder);

        // The node encodes itself, and then its children.
        expr->acceptVisitor(&_visitor);

        if (expr->numChildren() > 0) {
            _builder->appendChar(kEncodeChildrenBegin);
        }
    }

    void inVisit(long count, const MatchExpression* expr) {
        _builder->appendChar(kEncodeChildrenSeparator);
    }

    void postVisit(const MatchExpression* expr) {
        if (expr->numChildren() > 0) {
            _builder->appendChar(kEncodeChildrenEnd);
        }
    }

private:
    BufBuilder* const _builder;
    MatchExpressionSbePlanCacheKeySerializationVisitor _visitor;
};

/**
 * Given a 'matchExpr' which may have parameter markers, encodes a key into 'builder' with the
 * following property: Two match expression trees which are identical after auto-parameterization
 * have the same key, otherwise the keys must differ.
 */
void encodeKeyForAutoParameterizedMatchSBE(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           MatchExpression* matchExpr,
                                           BufBuilder* builder,
                                           bool hasSort) {
    MatchExpressionSbePlanCacheKeySerializationWalker walker{expCtx, builder, hasSort};
    tree_walker::walk<true, MatchExpression>(matchExpr, &walker);
}

}  // namespace

namespace canonical_query_encoder {

/**
 * Encode the stages pushed down to SBE via CanonicalQuery::cqPipeline.
 * Also encodes pipelines that are eligible for the Bonsai plan cache.
 */
void encodePipeline(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                    const std::vector<boost::intrusive_ptr<DocumentSource>>& cqPipeline,
                    BufBuilder* bufBuilder) {
    bufBuilder->appendChar(kEncodeSectionDelimiter);
    std::vector<Value> serializedArray;
    for (auto& stage : cqPipeline) {
        auto documentSource = stage.get();
        if (auto matchStage = dynamic_cast<DocumentSourceMatch*>(documentSource)) {
            // Match expressions are parameterized so need to be encoded differently.
            encodeKeyForAutoParameterizedMatchSBE(
                expCtx,
                matchStage->getMatchExpression(),
                bufBuilder,
                // We do not use explode for sort optimization for a $match stage, since it is not
                // a part of index selection.
                false /*hasSort*/);
        } else if (!search_helpers::encodeSearchForSbeCache(expCtx, documentSource, bufBuilder)) {
            encodeKeyForPipelineStage(documentSource, serializedArray, bufBuilder);
        }
    }  // for each stage in 'cqPipeline'
}

CanonicalQuery::QueryShapeString encodePipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const std::vector<boost::intrusive_ptr<DocumentSource>>& pipelineStages) {
    static constexpr size_t bufferSize = 200;
    BufBuilder bufBuilder(bufferSize);

    canonical_query_encoder::encodePipeline(expCtx, pipelineStages, &bufBuilder);
    return base64::encode(StringData(bufBuilder.buf(), bufBuilder.len()));
}

CanonicalQuery::QueryShapeString encodeClassic(const CanonicalQuery& cq) {
    StringBuilder keyBuilder;
    encodeKeyForMatch(cq.getPrimaryMatchExpression(), &keyBuilder);
    encodeKeyForSort(cq.getFindCommandRequest().getSort(), &keyBuilder);
    encodeKeyForProj(cq.getProj(), &keyBuilder);
    encodeCollation(cq.getCollator(), &keyBuilder);
    encodeKeyForDistinct(cq.getDistinct(),
                         &keyBuilder,
                         cq.getExpCtx()->isFeatureFlagShardFilteringDistinctScanEnabled());


    // The apiStrict flag can cause the query to see different set of indexes. For example, all
    // sparse indexes will be ignored with apiStrict is used.
    const bool apiStrict =
        cq.getOpCtx() && APIParameters::get(cq.getOpCtx()).getAPIStrict().value_or(false);
    keyBuilder << (apiStrict ? "t" : "f");


    // Encode a flag with three possible values:
    // 1 ('c'): The cache entry is intended to use the classic code path completely. In this case
    //            the entry stores 'works.'
    // 2 ('s'): The cache entry was planned with the classic runtime planners, and we intend
    //            to run it in SBE. In this case the entry stores 'reads.'
    // 3 ('o'): The cache entry is used for sub-planning using the classic SubPlanner
    //            and we intend to run it in SBE. In this case the entry stores 'works' but
    //            since replanning doesn't happen for queries generated from subplanning,
    //            the value has no meaning. TODO SERVER-90957: Update these entries to store a
    //            placeholder instead.
    //
    // The third case is treated specially because the classic sub-planner stores and reads
    // cache entries that cannot be executed with SBE. We use this extra flag to avoid the
    // possibility of the sub planner writing a cache entry for a branch of $or which is then used
    // by an independent query that doesn't subplan.
    //
    // For example, if the user runs an SBE-compatible query {$or: [{a:1,b:1}, {c:1,d:1}]} and a
    // cache entry is written for each branch, we do NOT want thos cache entries to be re-used for
    // a query {a:1,b:1} since the second query would expect the cache entry to store a "reads"
    // value that can be used with SBE's cache recovery path.
    //
    // By incorporating 'forSubplanner' we eliminate this possibility.

    if (cq.isSbeCompatible()) {
        if (cq.forSubPlanner()) {
            keyBuilder << "o";  // Case 3: 'o' for "OR."
        } else {
            keyBuilder << "s";  // Case 2: 's' for "SBE."
        }
    } else {
        keyBuilder << "c";  // Case 1: 'c' for "classic."
    }

    return keyBuilder.str();
}

std::string encodeSBE(const CanonicalQuery& cq, const bool requiresSbeCompatibility) {
    if (requiresSbeCompatibility) {
        tassert(6142104,
                "attempting to encode SBE plan cache key for SBE-incompatible query",
                cq.isSbeCompatible());
    }

    const auto& filter = cq.getQueryObj();
    const auto& proj = cq.getFindCommandRequest().getProjection();
    const auto& sort = cq.getFindCommandRequest().getSort();
    const auto& hint = cq.getFindCommandRequest().getHint();

    StringBuilder strBuilder;
    encodeKeyForSort(sort, &strBuilder);
    encodeCollation(cq.getCollator(), &strBuilder);
    auto strBuilderEncoded = strBuilder.stringData();

    // A constant for reserving buffer size. It should be large enough to reserve the space required
    // to encode various properties from the FindCommandRequest and query knobs.
    const int kBufferSizeConstant = 200;
    size_t bufSize = filter.objsize() + proj.objsize() + strBuilderEncoded.size() + hint.objsize() +
        kBufferSizeConstant;

    BufBuilder bufBuilder(bufSize);
    // Only encode parameter types in the MatchExpression if this key is being generated by
    // Bonsai.
    encodeKeyForAutoParameterizedMatchSBE(
        cq.getExpCtx(), cq.getPrimaryMatchExpression(), &bufBuilder, !sort.isEmpty());
    bufBuilder.appendBuf(proj.objdata(), proj.objsize());
    bufBuilder.appendStrBytes(strBuilderEncoded);
    bufBuilder.appendChar(kEncodeSectionDelimiter);
    if (!hint.isEmpty()) {
        bufBuilder.appendBuf(hint.objdata(), hint.objsize());
    }
    bufBuilder.appendChar(kEncodeSectionDelimiter);
    bufBuilder.appendChar(cq.getForceGenerateRecordId() ? 1 : 0);
    bufBuilder.appendChar(cq.isCountLike() ? 1 : 0);
    // The apiStrict flag can cause the query to see different set of indexes. For example, all
    // sparse indexes will be ignored with apiStrict is used.
    const bool apiStrict =
        cq.getOpCtx() && APIParameters::get(cq.getOpCtx()).getAPIStrict().value_or(false);
    bufBuilder.appendChar(apiStrict ? 1 : 0);

    // We can wind up with different query plans for aggregate commands if 'needsMerge' is set or
    // not. For instance, when 'needsMerge' is true, $group queries will produce partial aggregates
    // as output, and complete output otherwise.
    const bool needsMerge = cq.getExpCtx()->getNeedsMerge();
    bufBuilder.appendChar(needsMerge ? 1 : 0);

    encodeFindCommandRequest(cq, &bufBuilder);

    encodePipeline(cq.getExpCtx(), cq.cqPipeline(), &bufBuilder);

    return base64::encode(StringData(bufBuilder.buf(), bufBuilder.len()));
}

CanonicalQuery::PlanCacheCommandKey encodeForPlanCacheCommand(const CanonicalQuery& cq) {
    StringBuilder keyBuilder;
    encodeKeyForMatch(cq.getPrimaryMatchExpression(), &keyBuilder);
    encodeKeyForSort(cq.getFindCommandRequest().getSort(), &keyBuilder);
    encodeKeyForProj(cq.getProj(), &keyBuilder);

    // We only encode user-specified collation. Collation inherited from the collection should not
    // be encoded.
    if (!cq.getFindCommandRequest().getCollation().isEmpty()) {
        encodeCollation(cq.getCollator(), &keyBuilder);
    } else {
        keyBuilder << kEncodeSectionDelimiter;
    }

    return keyBuilder.str();
}

CanonicalQuery::PlanCacheCommandKey encodeForPlanCacheCommand(const Pipeline& pipeline) {
    static constexpr size_t bufferSize = 200;
    BufBuilder bufBuilder(bufferSize);

    std::vector<Value> serializedArray;
    for (auto& stage : pipeline.getSources()) {
        auto documentSource = stage.get();
        if (auto matchStage = dynamic_cast<DocumentSourceMatch*>(documentSource)) {
            StringBuilder keyBuilder;
            encodeKeyForMatch(matchStage->getMatchExpression(), &keyBuilder);
            bufBuilder.appendCStr(keyBuilder.stringData());
        } else if (!search_helpers::encodeSearchForSbeCache(
                       pipeline.getContext(), documentSource, &bufBuilder)) {
            encodeKeyForPipelineStage(documentSource, serializedArray, &bufBuilder);
        }
    }  // for each stage in 'pipeline'

    std::string key(bufBuilder.buf(), bufBuilder.len());
    return key;
}

uint32_t computeHash(StringData key) {
    size_t seed = 0;
    simpleStringDataComparator.hash_combine(seed, key);
    return seed;
}
}  // namespace canonical_query_encoder
}  // namespace mongo
