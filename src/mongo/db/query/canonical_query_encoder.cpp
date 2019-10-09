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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/canonical_query_encoder.h"

#include <boost/iterator/transform_iterator.hpp>

#include "mongo/base/simple_string_data_comparator.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/query/projection.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

// Delimiters for cache key encoding.
const char kEncodeChildrenBegin = '[';
const char kEncodeChildrenEnd = ']';
const char kEncodeChildrenSeparator = ',';
const char kEncodeCollationSection = '#';
const char kEncodeProjectionSection = '|';
const char kEncodeProjectionRequirementSeparator = '-';
const char kEncodeRegexFlagsSeparator = '/';
const char kEncodeSortSection = '~';

/**
 * Encode user-provided string. Cache key delimiters seen in the
 * user string are escaped with a backslash.
 */
void encodeUserString(StringData s, StringBuilder* keyBuilder) {
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        switch (c) {
            case kEncodeChildrenBegin:
            case kEncodeChildrenEnd:
            case kEncodeChildrenSeparator:
            case kEncodeCollationSection:
            case kEncodeProjectionSection:
            case kEncodeProjectionRequirementSeparator:
            case kEncodeRegexFlagsSeparator:
            case kEncodeSortSection:
            case '\\':
                *keyBuilder << '\\';
            // Fall through to default case.
            default:
                *keyBuilder << c;
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

        case MatchExpression::INTERNAL_EXPR_EQ:
            return "ee";

        case MatchExpression::INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX:
            return "internalSchemaAllElemMatchFromIndex";

        case MatchExpression::INTERNAL_SCHEMA_ALLOWED_PROPERTIES:
            return "internalSchemaAllowedProperties";

        case MatchExpression::INTERNAL_SCHEMA_BIN_DATA_ENCRYPTED_TYPE:
            return "internalSchemaBinDataEncryptedType";

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
void encodeGeoMatchExpression(const GeoMatchExpression* tree, StringBuilder* keyBuilder) {
    const GeoExpression& geoQuery = tree->getGeoExpression();

    // Type of geo query.
    switch (geoQuery.getPred()) {
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
    *keyBuilder << geoQuery.getGeometry().getDebugType();

    // CRS (flat or spherical)
    if (FLAT == geoQuery.getGeometry().getNativeCRS()) {
        *keyBuilder << "fl";
    } else if (SPHERE == geoQuery.getGeometry().getNativeCRS()) {
        *keyBuilder << "sp";
    } else if (STRICT_SPHERE == geoQuery.getGeometry().getNativeCRS()) {
        *keyBuilder << "ss";
    } else {
        error() << "unknown CRS type " << (int)geoQuery.getGeometry().getNativeCRS()
                << " in geometry of type " << geoQuery.getGeometry().getDebugType();
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
            error() << "unknown CRS type " << (int)nearQuery.centroid->crs
                    << " in point geometry for near query";
            MONGO_UNREACHABLE;
            break;
    }
}

template <class T>
char encodeEnum(T val) {
    static_assert(static_cast<int>(T::kMax) <= 9,
                  "enum has too many values to encode as a value between '0' and '9'. You must "
                  "change the encoding scheme");
    invariant(val <= T::kMax);

    return static_cast<char>(val) + '0';
}

void encodeCollation(const CollatorInterface* collation, StringBuilder* keyBuilder) {
    if (!collation) {
        return;
    }

    const CollationSpec& spec = collation->getSpec();

    *keyBuilder << kEncodeCollationSection;
    *keyBuilder << spec.localeID;
    *keyBuilder << spec.caseLevel;

    // Ensure that we can encode this value with a single ascii byte '0' through '9'.
    *keyBuilder << encodeEnum(spec.caseFirst);
    *keyBuilder << encodeEnum(spec.strength);
    *keyBuilder << spec.numericOrdering;

    *keyBuilder << encodeEnum(spec.alternate);
    *keyBuilder << encodeEnum(spec.maxVariable);
    *keyBuilder << spec.normalization;
    *keyBuilder << spec.backwards;

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
    const auto transformFunc = [](const auto& regex) { return regex.get(); };
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

    // GEO and GEO_NEAR require additional encoding.
    if (MatchExpression::GEO == tree->matchType()) {
        encodeGeoMatchExpression(static_cast<const GeoMatchExpression*>(tree), keyBuilder);
    } else if (MatchExpression::GEO_NEAR == tree->matchType()) {
        encodeGeoNearMatchExpression(static_cast<const GeoNearMatchExpression*>(tree), keyBuilder);
    }

    // We encode regular expression flags such that different options produce different shapes.
    if (MatchExpression::REGEX == tree->matchType()) {
        encodeRegexFlagsForMatch({static_cast<const RegexMatchExpression*>(tree)}, keyBuilder);
    } else if (MatchExpression::MATCH_IN == tree->matchType()) {
        const auto* inMatch = static_cast<const InMatchExpression*>(tree);
        if (!inMatch->getRegexes().empty()) {
            // Append '_re' to distinguish an $in without regexes from an $in with regexes.
            encodeUserString("_re"_sd, keyBuilder);
            encodeRegexFlagsForMatch(inMatch->getRegexes(), keyBuilder);
        }
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
 * Encodes sort order into cache key.
 * Sort order is normalized because it provided by
 * QueryRequest.
 */
void encodeKeyForSort(const BSONObj& sortObj, StringBuilder* keyBuilder) {
    if (sortObj.isEmpty()) {
        return;
    }

    *keyBuilder << kEncodeSortSection;

    BSONObjIterator it(sortObj);
    while (it.more()) {
        BSONElement elt = it.next();
        // $meta text score
        if (QueryRequest::isTextScoreMeta(elt)) {
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
    if (!proj || proj->requiresDocument()) {
        // Don't encode anything for the projection section to indicate the entire document is
        // required.
        return;
    }

    std::vector<std::string> requiredFields = proj->getRequiredFields();
    invariant(!requiredFields.empty());

    // Keep track of whether we appended the character marking the beginning of the projection
    // section. We may not have to if all of the fields in the projection are $-prefixed.
    bool appendedStart = false;

    // Encode the fields required by the projection in order.
    std::sort(requiredFields.begin(), requiredFields.end());
    for (auto&& requiredField : requiredFields) {
        invariant(!requiredField.empty());

        // Internal callers (e.g, from mongos) may add "$sortKey" to the projection. This is not
        // part of the user query, and therefore are not considered part of the cache key.
        if (requiredField == "$sortKey") {
            continue;
        }

        const bool isFirst = !appendedStart;

        if (isFirst) {
            *keyBuilder << kEncodeProjectionSection;
            appendedStart = true;
        } else {
            *keyBuilder << kEncodeProjectionRequirementSeparator;
        }
        encodeUserString(requiredField, keyBuilder);
    }
}
}  // namespace

namespace canonical_query_encoder {

CanonicalQuery::QueryShapeString encode(const CanonicalQuery& cq) {
    StringBuilder keyBuilder;
    encodeKeyForMatch(cq.root(), &keyBuilder);
    encodeKeyForSort(cq.getQueryRequest().getSort(), &keyBuilder);
    encodeKeyForProj(cq.getProj(), &keyBuilder);
    encodeCollation(cq.getCollator(), &keyBuilder);

    return keyBuilder.str();
}

uint32_t computeHash(StringData key) {
    return SimpleStringDataComparator::kInstance.hash(key);
}
}  // namespace canonical_query_encoder
}  // namespace mongo
