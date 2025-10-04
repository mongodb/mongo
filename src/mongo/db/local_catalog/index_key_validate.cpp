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

#include "mongo/db/local_catalog/index_key_validate.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/index/wildcard_validation.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/ttl/ttl_collection_cache.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/represent_as.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/numeric/conversion/converter_policies.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex


namespace mongo {
namespace index_key_validate {

std::function<void(std::map<StringData, std::set<IndexType>>&)> filterAllowedIndexFieldNames;

using IndexVersion = IndexDescriptor::IndexVersion;

std::map<StringData, std::set<IndexType>> kAllowedFieldNames = {
    {IndexDescriptor::k2dIndexBitsFieldName, {IndexType::INDEX_2D}},
    {IndexDescriptor::k2dIndexMaxFieldName, {IndexType::INDEX_2D}},
    {IndexDescriptor::k2dIndexMinFieldName, {IndexType::INDEX_2D}},
    {IndexDescriptor::k2dsphereCoarsestIndexedLevel, {IndexType::INDEX_2DSPHERE}},
    {IndexDescriptor::k2dsphereFinestIndexedLevel, {IndexType::INDEX_2DSPHERE}},
    {IndexDescriptor::k2dsphereVersionFieldName,
     {IndexType::INDEX_2DSPHERE, IndexType::INDEX_2DSPHERE_BUCKET}},
    {IndexDescriptor::kBackgroundFieldName, {}},
    {IndexDescriptor::kCollationFieldName, {}},
    {IndexDescriptor::kDefaultLanguageFieldName, {}},
    {IndexDescriptor::kDropDuplicatesFieldName, {}},
    {IndexDescriptor::kExpireAfterSecondsFieldName, {}},
    {IndexDescriptor::kHiddenFieldName, {}},
    {IndexDescriptor::kIndexNameFieldName, {}},
    {IndexDescriptor::kIndexVersionFieldName, {}},
    {IndexDescriptor::kKeyPatternFieldName, {}},
    {IndexDescriptor::kLanguageOverrideFieldName, {}},
    // TODO(SERVER-100328): remove after 9.0 is branched.
    {IndexDescriptor::kNamespaceFieldName, {}},
    {IndexDescriptor::kPartialFilterExprFieldName, {}},
    {IndexDescriptor::kWildcardProjectionFieldName, {IndexType::INDEX_WILDCARD}},
    {IndexDescriptor::kSparseFieldName, {}},
    {IndexDescriptor::kStorageEngineFieldName, {}},
    {IndexDescriptor::kTextVersionFieldName, {IndexType::INDEX_TEXT}},
    {IndexDescriptor::kUniqueFieldName, {}},
    {IndexDescriptor::kWeightsFieldName, {IndexType::INDEX_TEXT}},
    {IndexDescriptor::kOriginalSpecFieldName, {}},
    {IndexDescriptor::kPrepareUniqueFieldName, {}},
    // Index creation under legacy writeMode can result in an index spec with an _id field.
    {"_id", {}},
    // TODO SERVER-76108: Field names are not validated to match index type. This was used for the
    // removed 'geoHaystack' index type, but users could have set it for other index types as well.
    // We need to keep allowing it until FCV upgrade is implemented to clean this up.
    {"bucketSize"_sd, {}}};

// Initialised by a GlobalInitializerRegisterer.
std::map<StringData, std::set<IndexType>> kNonDeprecatedAllowedFieldNames = {};

namespace {
// When the skipIndexCreateFieldNameValidation failpoint is enabled, validation for index field
// names will be disabled. This will allow for creation of indexes with invalid field names in their
// specification.
MONGO_FAIL_POINT_DEFINE(skipIndexCreateFieldNameValidation);

// When the skipTTLIndexExpireAfterSecondsValidation failpoint is enabled,
// validation for TTL index 'expireAfterSeconds' will be disabled in certain codepaths.
MONGO_FAIL_POINT_DEFINE(skipTTLIndexExpireAfterSecondsValidation);

static const std::set<StringData> allowedIdIndexFieldNames = {
    IndexDescriptor::kCollationFieldName,
    IndexDescriptor::kIndexNameFieldName,
    IndexDescriptor::kIndexVersionFieldName,
    IndexDescriptor::kKeyPatternFieldName,
    // TODO(SERVER-100328): remove after 9.0 is branched.
    IndexDescriptor::kNamespaceFieldName,
    // Index creation under legacy writeMode can result in an index spec with an _id field.
    "_id"};

static const std::set<StringData> allowedClusteredIndexFieldNames = {
    ClusteredIndexSpec::kNameFieldName,
    ClusteredIndexSpec::kUniqueFieldName,
    ClusteredIndexSpec::kVFieldName,
    ClusteredIndexSpec::kKeyFieldName,
    // These are for indexSpec creation only.
    IndexDescriptor::kClusteredFieldName,
    IndexDescriptor::kExpireAfterSecondsFieldName,
};

/**
 * Returns Status::OK() if indexes of version 'indexVersion' are allowed to be created, and
 * returns ErrorCodes::CannotCreateIndex otherwise.
 */
Status isIndexVersionAllowedForCreation(IndexVersion indexVersion, const BSONObj& indexSpec) {
    switch (indexVersion) {
        case IndexVersion::kV1:
        case IndexVersion::kV2:
            return Status::OK();
    }
    return {ErrorCodes::CannotCreateIndex,
            str::stream() << "Invalid index specification " << indexSpec
                          << "; cannot create an index with v=" << static_cast<int>(indexVersion)};
}

BSONObj buildRepairedIndexSpec(
    const NamespaceString& ns,
    const BSONObj& indexSpec,
    const std::map<StringData, std::set<IndexType>>& allowedFieldNames,
    std::function<void(const BSONElement&, BSONObjBuilder*)> indexSpecHandleFn) {
    const auto key = indexSpec.getObjectField(IndexDescriptor::kKeyPatternFieldName);
    const auto indexName = IndexNames::nameToType(IndexNames::findPluginName(key));
    BSONObjBuilder builder;
    for (const auto& indexSpecElem : indexSpec) {
        StringData fieldName = indexSpecElem.fieldNameStringData();
        auto it = allowedFieldNames.find(fieldName);
        if (it != allowedFieldNames.end() &&
            (it->second.empty() || it->second.count(indexName) != 0)) {
            indexSpecHandleFn(indexSpecElem, &builder);
        } else {
            LOGV2_WARNING(23878,
                          "Removing unknown field from index spec",
                          "namespace"_attr = redact(toStringForLogging(ns)),
                          "fieldName"_attr = redact(fieldName),
                          "indexSpec"_attr = redact(indexSpec));
        }
    }
    return builder.obj();
}
}  // namespace

Status validateKeyPattern(const BSONObj& key, IndexDescriptor::IndexVersion indexVersion) {
    const ErrorCodes::Error code = ErrorCodes::CannotCreateIndex;

    if (key.objsize() > 2048)
        return Status(code, "Index key pattern too large.");

    if (key.isEmpty())
        return Status(code, "Index keys cannot be empty.");

    auto pluginName = IndexNames::findPluginName(key);
    if (pluginName.size()) {
        if (pluginName == IndexNames::GEO_HAYSTACK)
            return {
                ErrorCodes::CannotCreateIndex,
                str::stream() << "GeoHaystack indexes cannot be created in version 5.0 and above"};
        if (!IndexNames::isKnownName(pluginName))
            return Status(code, str::stream() << "Unknown index plugin '" << pluginName << '\'');
    }

    if (pluginName == IndexNames::WILDCARD) {
        auto status = validateWildcardIndex(key);
        if (!status.isOK()) {
            return status;
        }
    }

    BSONObjIterator it(key);
    while (it.more()) {
        BSONElement keyElement = it.next();

        switch (indexVersion) {
            case IndexVersion::kV1: {
                if (keyElement.type() == BSONType::object || keyElement.type() == BSONType::array) {
                    return {code,
                            str::stream() << "Values in index key pattern cannot be of type "
                                          << typeName(keyElement.type()) << " for index version v:"
                                          << static_cast<int>(indexVersion)};
                }

                break;
            }
            case IndexVersion::kV2: {
                if (keyElement.isNumber()) {
                    double value = keyElement.number();
                    if (std::isnan(value)) {
                        return {code, "Values in the index key pattern cannot be NaN."};
                    } else if (value == 0.0) {
                        return {code, "Values in the index key pattern cannot be 0."};
                    }
                } else if (keyElement.type() != BSONType::string) {
                    return {code,
                            str::stream()
                                << "Values in v:2 index key pattern cannot be of type "
                                << typeName(keyElement.type())
                                << ". Only numbers > 0, numbers < 0, and strings are allowed."};
                }

                break;
            }
            default:
                MONGO_UNREACHABLE;
        }

        if (keyElement.type() == BSONType::string && pluginName != keyElement.str()) {
            return Status(code, "Can't use more than one index plugin for a single index.");
        } else if (keyElement.type() == BSONType::string &&
                   keyElement.str() == IndexNames::WILDCARD) {
            return Status(code,
                          str::stream() << "The key pattern value for an '" << IndexNames::WILDCARD
                                        << "' index must be a non-zero number, not a string.");
        }

        // Ensure that the fields on which we are building the index are valid: a field must not
        // begin with a '$' unless it is part of a wildcard, DBRef or text index, and a field path
        // cannot contain an empty field. If a field cannot be created or updated, it should not be
        // indexable.

        FieldRef keyField(keyElement.fieldName());

        const size_t numParts = keyField.numParts();
        if (numParts == 0) {
            return Status(code, "Index keys cannot be an empty field.");
        }

        // "$**" is acceptable for a text or wildcard index.
        if ((keyElement.fieldNameStringData() == "$**") &&
            ((keyElement.isNumber()) || (keyElement.str() == IndexNames::TEXT)))
            continue;

        if ((keyElement.fieldNameStringData() == "_fts") && keyElement.str() != IndexNames::TEXT) {
            return Status(code, "Index key contains an illegal field name: '_fts'");
        }

        for (size_t i = 0; i != numParts; ++i) {
            const StringData part = keyField.getPart(i);

            // Check if the index key path contains an empty field.
            if (part.empty()) {
                return Status(code, "Index keys cannot contain an empty field.");
            }

            if (part[0] != '$')
                continue;

            // Check if the '$'-prefixed field is part of a DBRef: since we don't have the
            // necessary context to validate whether this is a proper DBRef, we allow index
            // creation on '$'-prefixed names that match those used in a DBRef.
            const bool mightBePartOfDbRef =
                (i != 0) && (part == "$db" || part == "$id" || part == "$ref");

            const bool isWildcard =
                (i == numParts - 1) && (part == "$**") && (pluginName == IndexNames::WILDCARD);

            if (!mightBePartOfDbRef && !isWildcard) {
                return Status(code,
                              "Index key contains an illegal field name: "
                              "field name starts with '$'.");
            }
        }
    }

    return Status::OK();
}

BSONObj removeUnknownFields(const NamespaceString& ns, const BSONObj& indexSpec) {
    auto appendIndexSpecFn = [](const BSONElement& indexSpecElem, BSONObjBuilder* builder) {
        builder->append(indexSpecElem);
    };
    return buildRepairedIndexSpec(ns, indexSpec, kAllowedFieldNames, appendIndexSpecFn);
}

BSONObj repairIndexSpec(const NamespaceString& ns,
                        const BSONObj& indexSpec,
                        const std::map<StringData, std::set<IndexType>>& allowedFieldNames) {
    auto fixIndexSpecFn = [&indexSpec, &ns](const BSONElement& indexSpecElem,
                                            BSONObjBuilder* builder) {
        StringData fieldName = indexSpecElem.fieldNameStringData();
        // The "background" field has been deprecated. Ignore its duplication here so it will be
        // repaired for new indexes in the future, and also be ignored while listing old indexes.
        if (IndexDescriptor::kBackgroundFieldName == fieldName && builder->hasField(fieldName)) {
            LOGV2_WARNING(8072000,
                          "Ignoring duplicated field from index spec",
                          "namespace"_attr = redact(toStringForLogging(ns)),
                          "fieldName"_attr = redact(fieldName),
                          "indexSpec"_attr = redact(indexSpec));
            return;
        }

        if ((IndexDescriptor::kBackgroundFieldName == fieldName ||
             IndexDescriptor::kUniqueFieldName == fieldName ||
             IndexDescriptor::kSparseFieldName == fieldName ||
             IndexDescriptor::kDropDuplicatesFieldName == fieldName ||
             IndexDescriptor::kPrepareUniqueFieldName == fieldName ||
             IndexDescriptor::kClusteredFieldName == fieldName) &&
            !indexSpecElem.isNumber() && !indexSpecElem.isBoolean() && indexSpecElem.trueValue()) {
            LOGV2_WARNING(6444400,
                          "Fixing boolean field from index spec",
                          "namespace"_attr = redact(toStringForLogging(ns)),
                          "fieldName"_attr = redact(fieldName),
                          "indexSpec"_attr = redact(indexSpec));
            builder->appendBool(fieldName, true);
        } else if (IndexDescriptor::kExpireAfterSecondsFieldName == fieldName &&
                   !validateExpireAfterSeconds(indexSpecElem,
                                               ValidateExpireAfterSecondsMode::kSecondaryTTLIndex)
                        .isOK()) {
            LOGV2_WARNING(6835900,
                          "Fixing expire field from TTL index spec",
                          "namespace"_attr = redact(toStringForLogging(ns)),
                          "fieldName"_attr = redact(fieldName),
                          "indexSpec"_attr = redact(indexSpec));
            builder->appendNumber(fieldName,
                                  durationCount<Seconds>(kExpireAfterSecondsForInactiveTTLIndex));
        } else if (IndexDescriptor::k2dIndexBitsFieldName == fieldName) {
            // The bits index option might've been stored as a double in the catalog which is
            // incorrect considering this field represent the number of precision bits of a 2d
            // index. This can cause migrations to fail when comparing indexes from source and
            // destination shards, due to the usage of listIndexes, which makes an internal coercion
            // to int when parsing the indexes options.
            builder->appendNumber(fieldName, indexSpecElem.safeNumberInt());
        } else {
            builder->append(indexSpecElem);
        }
    };

    return buildRepairedIndexSpec(ns, indexSpec, allowedFieldNames, fixIndexSpecFn);
}

StatusWith<BSONObj> validateIndexSpec(
    OperationContext* opCtx,
    const BSONObj& indexSpec,
    const std::map<StringData, std::set<IndexType>>& allowedFieldNames) {
    bool hasKeyPatternField = false;
    bool hasIndexNameField = false;
    bool hasNamespaceField = false;
    bool isTTLIndexWithInvalidExpireAfterSeconds = false;
    bool isTTLIndexWithNonIntExpireAfterSeconds = false;
    bool hasVersionField = false;
    bool hasCollationField = false;
    bool hasWeightsField = false;
    bool hasOriginalSpecField = false;
    bool unique = false;
    bool prepareUnique = false;
    auto clusteredField = indexSpec[IndexDescriptor::kClusteredFieldName];
    bool apiStrict = opCtx && APIParameters::get(opCtx).getAPIStrict().value_or(false);

    auto fieldNamesValidStatus = validateIndexSpecFieldNames(indexSpec, allowedFieldNames);
    if (!fieldNamesValidStatus.isOK()) {
        return fieldNamesValidStatus;
    }

    boost::optional<IndexVersion> resolvedIndexVersion;
    std::string indexType;

    for (auto&& indexSpecElem : indexSpec) {
        auto indexSpecElemFieldName = indexSpecElem.fieldNameStringData();
        if (IndexDescriptor::kKeyPatternFieldName == indexSpecElemFieldName) {
            if (indexSpecElem.type() != BSONType::object) {
                return {ErrorCodes::TypeMismatch,
                        str::stream()
                            << "The field '" << IndexDescriptor::kKeyPatternFieldName
                            << "' must be an object, but got " << typeName(indexSpecElem.type())};
            }
            auto keyPattern = indexSpecElem.Obj();
            std::vector<StringData> keys;
            for (auto&& keyElem : keyPattern) {
                auto keyElemFieldName = keyElem.fieldNameStringData();
                if (std::find(keys.begin(), keys.end(), keyElemFieldName) != keys.end()) {
                    return {ErrorCodes::BadValue,
                            str::stream() << "The field '" << keyElemFieldName
                                          << "' appears multiple times in the index key pattern "
                                          << keyPattern};
                }
                keys.push_back(keyElemFieldName);
            }

            indexType = IndexNames::findPluginName(keyPattern);
            if (apiStrict && indexType == IndexNames::TEXT) {
                return {ErrorCodes::APIStrictError,
                        str::stream()
                            << indexType << " indexes cannot be created with apiStrict: true"};
            }

            // Here we always validate the key pattern according to the most recent rules, in order
            // to enforce that all new indexes have well-formed key patterns.
            Status keyPatternValidateStatus =
                validateKeyPattern(keyPattern, IndexConfig::kLatestIndexVersion);
            if (!keyPatternValidateStatus.isOK()) {
                return keyPatternValidateStatus;
            }

            for (const auto& keyElement : keyPattern) {
                if (keyElement.type() == BSONType::string && keyElement.str().empty()) {
                    return {ErrorCodes::CannotCreateIndex,
                            str::stream()
                                << "Values in the index key pattern cannot be empty strings"};
                }
                if (indexType == IndexNames::WILDCARD &&
                    keyElement.fieldNameStringData() == "$**" && keyPattern.nFields() > 1 &&
                    !indexSpec.hasField(IndexDescriptor::kWildcardProjectionFieldName)) {
                    return {ErrorCodes::CannotCreateIndex,
                            "Compound wildcard indexes on all fields must also specify "
                            "'wildcardProjection' option"};
                }
            }

            hasKeyPatternField = true;
        } else if (IndexDescriptor::kIndexNameFieldName == indexSpecElemFieldName) {
            if (indexSpecElem.type() != BSONType::string) {
                return {ErrorCodes::TypeMismatch,
                        str::stream()
                            << "The field '" << IndexDescriptor::kIndexNameFieldName
                            << "' must be a string, but got " << typeName(indexSpecElem.type())};
            }

            hasIndexNameField = true;
        } else if (IndexDescriptor::kHiddenFieldName == indexSpecElemFieldName) {
            if (indexSpecElem.type() != BSONType::boolean) {
                return {ErrorCodes::TypeMismatch,
                        str::stream()
                            << "The field '" << IndexDescriptor::kHiddenFieldName
                            << "' must be a bool, but got " << typeName(indexSpecElem.type())};
            }

        } else if (IndexDescriptor::kNamespaceFieldName == indexSpecElemFieldName) {
            // TODO(SERVER-100328): remove after 9.0 is branched.
            hasNamespaceField = true;
        } else if (IndexDescriptor::kIndexVersionFieldName == indexSpecElemFieldName) {
            if (!indexSpecElem.isNumber()) {
                return {ErrorCodes::TypeMismatch,
                        str::stream()
                            << "The field '" << IndexDescriptor::kIndexVersionFieldName
                            << "' must be a number, but got " << typeName(indexSpecElem.type())};
            }

            auto requestedIndexVersionAsInt = representAs<int>(indexSpecElem.number());
            if (!requestedIndexVersionAsInt) {
                return {ErrorCodes::BadValue,
                        str::stream()
                            << "Index version must be representable as a 32-bit integer, but got "
                            << indexSpecElem.toString(false, false)};
            }

            const IndexVersion requestedIndexVersion =
                static_cast<IndexVersion>(*requestedIndexVersionAsInt);
            auto creationAllowedStatus =
                isIndexVersionAllowedForCreation(requestedIndexVersion, indexSpec);
            if (!creationAllowedStatus.isOK()) {
                return creationAllowedStatus;
            }

            hasVersionField = true;
            resolvedIndexVersion = requestedIndexVersion;
        } else if (IndexDescriptor::kOriginalSpecFieldName == indexSpecElemFieldName) {
            if (indexSpecElem.type() != BSONType::object) {
                return {ErrorCodes::TypeMismatch,
                        str::stream()
                            << "The field '" << IndexDescriptor::kOriginalSpecFieldName
                            << "' must be an object, but got " << typeName(indexSpecElem.type())};
            }

            if (indexSpecElem.Obj().isEmpty()) {
                return {ErrorCodes::BadValue,
                        str::stream() << "The field '" << IndexDescriptor::kOriginalSpecFieldName
                                      << "' cannot be an empty object."};
            }

            hasOriginalSpecField = true;
        } else if (IndexDescriptor::kCollationFieldName == indexSpecElemFieldName) {
            if (indexSpecElem.type() != BSONType::object) {
                return {ErrorCodes::TypeMismatch,
                        str::stream()
                            << "The field '" << IndexDescriptor::kCollationFieldName
                            << "' must be an object, but got " << typeName(indexSpecElem.type())};
            }

            if (indexSpecElem.Obj().isEmpty()) {
                return {ErrorCodes::BadValue,
                        str::stream() << "The field '" << IndexDescriptor::kCollationFieldName
                                      << "' cannot be an empty object."};
            }

            hasCollationField = true;
        } else if (IndexDescriptor::kPartialFilterExprFieldName == indexSpecElemFieldName) {
            if (indexSpecElem.type() != BSONType::object) {
                return {ErrorCodes::TypeMismatch,
                        str::stream()
                            << "The field '" << IndexDescriptor::kPartialFilterExprFieldName
                            << "' must be an object, but got " << typeName(indexSpecElem.type())};
            }

            // Just use the simple collator, even though the index may have a separate collation
            // specified or may inherit the default collation from the collection. It's legal to
            // parse with the wrong collation, since the collation can be set on a MatchExpression
            // after the fact. Here, we don't bother checking the collation after the fact, since
            // this invocation of the parser is just for validity checking. It's also legal to parse
            // with an empty namespace string, because we are only doing validity checking and not
            // resolving the expression against a given namespace.
            auto expCtx =
                ExpressionContextBuilder{}.opCtx(opCtx).ns(NamespaceString::kEmpty).build();
            // Special match expression features (e.g. $jsonSchema, $expr, ...) are not allowed in a
            // partialFilterExpression on index creation.
            auto statusWithMatcher =
                MatchExpressionParser::parse(indexSpecElem.Obj(),
                                             std::move(expCtx),
                                             ExtensionsCallbackNoop(),
                                             MatchExpressionParser::kBanAllSpecialFeatures);
            if (!statusWithMatcher.isOK()) {
                return statusWithMatcher.getStatus();
            }
        } else if (IndexDescriptor::kWildcardProjectionFieldName == indexSpecElemFieldName) {
            const auto key = indexSpec.getObjectField(IndexDescriptor::kKeyPatternFieldName);
            if (IndexNames::findPluginName(key) != IndexNames::WILDCARD) {
                // For backwards compatibility, we will return BadValue for Wildcard indices.
                return {ErrorCodes::BadValue,
                        str::stream()
                            << "The field '" << indexSpecElemFieldName << "' is only allowed in '"
                            << IndexNames::WILDCARD << "' indexes"};
            }
            if (indexSpecElem.type() != BSONType::object) {
                return {ErrorCodes::TypeMismatch,
                        str::stream() << "The field '" << indexSpecElemFieldName
                                      << "' must be a non-empty object, but got "
                                      << typeName(indexSpecElem.type())};
            }
            if (!key.hasField("$**")) {
                return {ErrorCodes::FailedToParse,
                        str::stream()
                            << "The field '" << indexSpecElemFieldName << "' is only allowed when '"
                            << IndexDescriptor::kKeyPatternFieldName << "' is {\"$**\": ±1}"};
            }
            if (indexSpecElem.embeddedObject().isEmpty()) {
                return {ErrorCodes::FailedToParse,
                        str::stream() << "The '" << indexSpecElemFieldName
                                      << "' field can't be an empty object"};
            }
            try {
                if (key.nFields() > 1) {
                    auto validationStatus =
                        validateWildcardProjection(key, indexSpecElem.embeddedObject());
                    if (!validationStatus.isOK()) {
                        return validationStatus;
                    }
                }
                // We use createProjectionExecutor to parse and validate the path projection
                // spec. call here
                WildcardKeyGenerator::createProjectionExecutor(key, indexSpecElem.embeddedObject());

            } catch (const DBException& ex) {
                return ex.toStatus(str::stream()
                                   << "Failed to parse projection: " << indexSpecElemFieldName);
            }
        } else if (IndexDescriptor::kWeightsFieldName == indexSpecElemFieldName) {
            if (!indexSpecElem.isABSONObj() && indexSpecElem.type() != BSONType::string) {
                return {ErrorCodes::TypeMismatch,
                        str::stream()
                            << "The field '" << indexSpecElemFieldName
                            << "' must be an object, but got " << typeName(indexSpecElem.type())};
            }
            hasWeightsField = true;
        } else if ((IndexDescriptor::kBackgroundFieldName == indexSpecElemFieldName ||
                    IndexDescriptor::kUniqueFieldName == indexSpecElemFieldName ||
                    IndexDescriptor::kSparseFieldName == indexSpecElemFieldName ||
                    IndexDescriptor::kDropDuplicatesFieldName == indexSpecElemFieldName ||
                    IndexDescriptor::kPrepareUniqueFieldName == indexSpecElemFieldName ||
                    IndexDescriptor::kClusteredFieldName == indexSpecElemFieldName)) {
            if (!indexSpecElem.isNumber() && !indexSpecElem.isBoolean()) {
                return {ErrorCodes::TypeMismatch,
                        str::stream()
                            << "The field '" << indexSpecElemFieldName << " has value "
                            << indexSpecElem.toString() << ", which is not convertible to bool"};
            }
            if (IndexDescriptor::kUniqueFieldName == indexSpecElemFieldName) {
                unique = indexSpecElem.trueValue();
            }
            if (IndexDescriptor::kPrepareUniqueFieldName == indexSpecElemFieldName) {
                prepareUnique = indexSpecElem.trueValue();
            }
        } else if ((IndexDescriptor::kDefaultLanguageFieldName == indexSpecElemFieldName ||
                    IndexDescriptor::kLanguageOverrideFieldName == indexSpecElemFieldName) &&
                   indexSpecElem.type() != BSONType::string) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "The field '" << indexSpecElemFieldName
                                  << "' must be a string, but got "
                                  << typeName(indexSpecElem.type())};
        } else if ((IndexDescriptor::k2dsphereVersionFieldName == indexSpecElemFieldName ||
                    IndexDescriptor::kTextVersionFieldName == indexSpecElemFieldName ||
                    IndexDescriptor::k2dIndexBitsFieldName == indexSpecElemFieldName ||
                    IndexDescriptor::k2dIndexMinFieldName == indexSpecElemFieldName ||
                    IndexDescriptor::k2dIndexMaxFieldName == indexSpecElemFieldName ||
                    IndexDescriptor::k2dsphereCoarsestIndexedLevel == indexSpecElemFieldName ||
                    IndexDescriptor::k2dsphereFinestIndexedLevel == indexSpecElemFieldName ||
                    IndexDescriptor::kBucketSizeFieldName == indexSpecElemFieldName) &&
                   !indexSpecElem.isNumber()) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << "The field '" << indexSpecElemFieldName
                                  << "' must be a number, but got "
                                  << typeName(indexSpecElem.type())};
        } else if (IndexDescriptor::kExpireAfterSecondsFieldName == indexSpecElemFieldName) {
            auto swType = validateExpireAfterSeconds(
                indexSpecElem, ValidateExpireAfterSecondsMode::kSecondaryTTLIndex);
            if (!swType.isOK()) {
                isTTLIndexWithInvalidExpireAfterSeconds = true;
            } else if (extractExpireAfterSecondsType(swType) ==
                       TTLCollectionCache::Info::ExpireAfterSecondsType::kNonInt) {
                isTTLIndexWithNonIntExpireAfterSeconds = true;
            }
        } else {
            // We can assume field name is valid at this point. Validation of fieldname is handled
            // prior to this in validateIndexSpecFieldNames().
            continue;
        }
    }

    if (!resolvedIndexVersion) {
        resolvedIndexVersion = IndexDescriptor::getDefaultIndexVersion();
    }

    if (!hasKeyPatternField) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "The '" << IndexDescriptor::kKeyPatternFieldName
                              << "' field is a required property of an index specification"};
    }

    if (!hasIndexNameField) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "The '" << IndexDescriptor::kIndexNameFieldName
                              << "' field is a required property of an index specification"};
    }

    if (clusteredField) {
        if (!clusteredField.trueValue()) {
            // Disallow 'clustered' from taking value 'false'
            return {ErrorCodes::Error(6492800), "Value 'false' for field 'clustered' is invalid"};
        }

        if (!indexSpec.hasField(IndexDescriptor::kUniqueFieldName) ||
            indexSpec.getBoolField(IndexDescriptor::kUniqueFieldName) == false) {
            // Only require 'unique' if clustered is specified.
            return {ErrorCodes::CannotCreateIndex,
                    str::stream() << "The '" << IndexDescriptor::kUniqueFieldName
                                  << "' field is required when 'clustered' is specified"};
        }
    }

    if (hasCollationField && *resolvedIndexVersion < IndexVersion::kV2) {
        return {ErrorCodes::CannotCreateIndex,
                str::stream() << "Invalid index specification " << indexSpec
                              << "; cannot create an index with the '"
                              << IndexDescriptor::kCollationFieldName << "' option and "
                              << IndexDescriptor::kIndexVersionFieldName << "="
                              << static_cast<int>(*resolvedIndexVersion)};
    }

    if (indexType != IndexNames::TEXT && hasWeightsField) {
        return {ErrorCodes::CannotCreateIndex,
                str::stream() << "Invalid index specification " << indexSpec << "; the field '"
                              << IndexDescriptor::kWeightsFieldName
                              << "' can only be specified with text indexes"};
    }

    if (unique && prepareUnique) {
        return {ErrorCodes::CannotCreateIndex,
                str::stream() << "Invalid index specification " << indexSpec
                              << "; cannot create an index with the '"
                              << IndexDescriptor::kUniqueFieldName << "' option and the '"
                              << IndexDescriptor::kPrepareUniqueFieldName << "' option"};
    }

    BSONObj modifiedSpec = indexSpec;

    // Ignore any 'ns' field in the index spec because this field is dropped post-4.0. Don't remove
    // the field during repair, as repair may run on old data files (version 3.6 and 4.0) that
    // require the field to be present.
    // TODO(SERVER-100328): remove after 9.0 is branched.
    if (hasNamespaceField && !storageGlobalParams.repair) {
        modifiedSpec = modifiedSpec.removeField(IndexDescriptor::kNamespaceFieldName);
    }

    if (!skipTTLIndexExpireAfterSecondsValidation.shouldFail()) {
        if (isTTLIndexWithInvalidExpireAfterSeconds) {
            // We create a new index specification with the 'expireAfterSeconds' field set as
            // kExpireAfterSecondsForInactiveTTLIndex if the current value is invalid. A similar
            // treatment is done in repairIndexSpec(). This rewrites the 'expireAfterSeconds'
            // value to be compliant with the 'safeInt' IDL type for the listIndexes response.
            BSONObjBuilder builder;
            builder.appendNumber(IndexDescriptor::kExpireAfterSecondsFieldName,
                                 durationCount<Seconds>(kExpireAfterSecondsForInactiveTTLIndex));
            auto obj = builder.obj();
            modifiedSpec = modifiedSpec.addField(obj.firstElement());
        }

        if (isTTLIndexWithNonIntExpireAfterSeconds) {
            BSONObjBuilder builder;
            builder.appendNumber(
                IndexDescriptor::kExpireAfterSecondsFieldName,
                indexSpec[IndexDescriptor::kExpireAfterSecondsFieldName].safeNumberInt());
            auto obj = builder.obj();
            modifiedSpec = modifiedSpec.addField(obj.firstElement());
        }
    }

    if (!hasVersionField) {
        // We create a new index specification with the 'v' field set as 'defaultIndexVersion' if
        // the field was omitted.
        BSONObj versionObj = BSON(IndexDescriptor::kIndexVersionFieldName
                                  << static_cast<int>(*resolvedIndexVersion));
        modifiedSpec = modifiedSpec.addField(versionObj.firstElement());
    }

    if (hasOriginalSpecField) {
        StatusWith<BSONObj> modifiedOriginalSpec = validateIndexSpec(
            opCtx, indexSpec.getObjectField(IndexDescriptor::kOriginalSpecFieldName));
        if (!modifiedOriginalSpec.isOK()) {
            return modifiedOriginalSpec.getStatus();
        }

        BSONObj specToAdd =
            BSON(IndexDescriptor::kOriginalSpecFieldName << modifiedOriginalSpec.getValue());
        modifiedSpec = modifiedSpec.addField(specToAdd.firstElement());
    }

    return modifiedSpec;
}

Status validateIdIndexSpec(const BSONObj& indexSpec) {
    bool isClusteredIndexSpec = indexSpec.hasField(IndexDescriptor::kClusteredFieldName);

    if (!isClusteredIndexSpec) {
        // Field names for a 'clustered' index spec have already been validated through
        // allowedClusteredIndexFieldNames.

        for (auto&& indexSpecElem : indexSpec) {
            auto indexSpecElemFieldName = indexSpecElem.fieldNameStringData();
            if (!allowedIdIndexFieldNames.count(indexSpecElemFieldName)) {
                return {ErrorCodes::InvalidIndexSpecificationOption,
                        str::stream()
                            << "The field '" << indexSpecElemFieldName
                            << "' is not valid for an _id index specification. Specification: "
                            << indexSpec};
            }
        }
    }

    auto keyPatternElem = indexSpec[IndexDescriptor::kKeyPatternFieldName];
    // validateIndexSpec() should have already verified that 'keyPatternElem' is an object.
    invariant(keyPatternElem.type() == BSONType::object);
    if (SimpleBSONObjComparator::kInstance.evaluate(keyPatternElem.Obj() != BSON("_id" << 1))) {
        return {ErrorCodes::BadValue,
                str::stream() << "The field '" << IndexDescriptor::kKeyPatternFieldName
                              << "' for an _id index must be {_id: 1}, but got "
                              << keyPatternElem.Obj()};
    }

    if (!indexSpec[IndexDescriptor::kHiddenFieldName].eoo()) {
        return Status(ErrorCodes::BadValue, "can't hide _id index");
    }

    return Status::OK();
}

/**
 * Top-level index spec field names for a "clustered" index are specified here.
 */
Status validateClusteredSpecFieldNames(const BSONObj& indexSpec) {
    for (auto&& indexSpecElem : indexSpec) {
        auto indexSpecElemFieldName = indexSpecElem.fieldNameStringData();
        if (!allowedClusteredIndexFieldNames.count(indexSpecElemFieldName)) {
            return {ErrorCodes::InvalidIndexSpecificationOption,
                    str::stream()
                        << "The field '" << indexSpecElemFieldName
                        << "' is not valid for a clustered index specification. Specification: "
                        << indexSpec};
        }
    }
    return Status::OK();
}

/**
 * Top-level index spec field names are validated here. When adding a new field with a document as
 * value, is the the sub-module's responsibility to ensure that the content is valid and that only
 * expected fields are present at creation time
 */
Status validateIndexSpecFieldNames(
    const BSONObj& indexSpec, const std::map<StringData, std::set<IndexType>>& allowedFieldNames) {
    if (MONGO_unlikely(skipIndexCreateFieldNameValidation.shouldFail())) {
        return Status::OK();
    }

    if (indexSpec.hasField(IndexDescriptor::kClusteredFieldName)) {
        return validateClusteredSpecFieldNames(indexSpec);
    }

    for (auto&& indexSpecElem : indexSpec) {
        auto indexSpecElemFieldName = indexSpecElem.fieldNameStringData();
        if (!allowedFieldNames.count(indexSpecElemFieldName)) {
            return {ErrorCodes::InvalidIndexSpecificationOption,
                    str::stream() << "The field '" << indexSpecElemFieldName
                                  << "' is not valid for an index specification. Specification: "
                                  << indexSpec};
        }
    }

    return Status::OK();
}

void validateOriginalSpecCollation(OperationContext* opCtx,
                                   BSONObj originalSpecCollation,
                                   BSONObj newIndexSpec,
                                   const CollatorInterface* defaultCollator) {
    auto indexSpecCollation = (newIndexSpec.hasField(IndexDescriptor::kCollationFieldName)
                                   ? newIndexSpec[IndexDescriptor::kCollationFieldName].Obj()
                                   : (defaultCollator ? defaultCollator->getSpec().toBSON()
                                                      : CollationSpec::kSimpleSpec));
    auto indexSpecCollator =
        CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(indexSpecCollation);
    auto originalSpecCollator = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                    ->makeFromBSON(originalSpecCollation);
    uassert(ErrorCodes::InvalidIndexSpecificationOption,
            str::stream() << "The collation of the originalSpec must be the same as the "
                             "collation of the outer index spec. originalSpec's collation: "
                          << originalSpecCollation
                          << ", indexSpec's collation: " << indexSpecCollation,
            CollatorInterface::collatorsMatch(indexSpecCollator.getValue().get(),
                                              originalSpecCollator.getValue().get()));
}

StatusWith<BSONObj> validateIndexSpecCollation(OperationContext* opCtx,
                                               const BSONObj& indexSpec,
                                               const CollatorInterface* defaultCollator,
                                               const boost::optional<BSONObj>& newIndexSpec) {
    if (auto collationElem = indexSpec[IndexDescriptor::kCollationFieldName]) {
        // validateIndexSpec() should have already verified that 'collationElem' is an object.
        invariant(collationElem.type() == BSONType::object);

        auto collator = CollatorFactoryInterface::get(opCtx->getServiceContext())
                            ->makeFromBSON(collationElem.Obj());
        if (!collator.isOK()) {
            return collator.getStatus();
        }

        if (collator.getValue()) {
            if (newIndexSpec)
                validateOriginalSpecCollation(
                    opCtx, collationElem.Obj(), *newIndexSpec, defaultCollator);
            // If the collator factory returned a non-null collator, then inject the entire
            // collation specification into the index specification. This is necessary to fill
            // in any options that the user omitted.
            BSONObjBuilder bob;

            for (auto&& indexSpecElem : indexSpec) {
                if (IndexDescriptor::kCollationFieldName != indexSpecElem.fieldNameStringData()) {
                    bob.append(indexSpecElem);
                }
            }
            bob.append(IndexDescriptor::kCollationFieldName,
                       collator.getValue().get()->getSpec().toBSON());

            return bob.obj();
        }

        if (newIndexSpec)
            validateOriginalSpecCollation(opCtx,
                                          defaultCollator ? defaultCollator->getSpec().toBSON()
                                                          : CollationSpec::kSimpleSpec,
                                          *newIndexSpec,
                                          defaultCollator);
        // If the collator factory returned a null collator (representing the "simple"
        // collation), then we simply omit the "collation" from the index specification.
        // This is desirable to make the representation for the "simple" collation
        // consistent between v=1 and v=2 indexes.
        return indexSpec.removeField(IndexDescriptor::kCollationFieldName);
    } else if (defaultCollator) {
        if (newIndexSpec)
            validateOriginalSpecCollation(
                opCtx, defaultCollator->getSpec().toBSON(), *newIndexSpec, defaultCollator);

        // validateIndexSpec() should have added the "v" field if it was not present and
        // verified that 'versionElem' is a number.
        auto versionElem = indexSpec[IndexDescriptor::kIndexVersionFieldName];
        invariant(versionElem.isNumber());

        if (IndexVersion::kV2 <= static_cast<IndexVersion>(versionElem.numberInt())) {
            // The user did not specify an explicit collation for this index and the collection
            // has a default collator. If we're building a v=2 index, then we should inherit the
            // collection default. However, if we're building a v=1 index, then we're implicitly
            // building an index that's using the "simple" collation.
            BSONObjBuilder bob;

            bob.appendElements(indexSpec);
            bob.append(IndexDescriptor::kCollationFieldName, defaultCollator->getSpec().toBSON());

            return bob.obj();
        }
    }

    if (newIndexSpec)
        validateOriginalSpecCollation(
            opCtx, CollationSpec::kSimpleSpec, *newIndexSpec, defaultCollator);
    return indexSpec;
}

Status validateExpireAfterSeconds(std::int64_t expireAfterSeconds,
                                  ValidateExpireAfterSecondsMode mode) {
    if (expireAfterSeconds < 0) {
        return {ErrorCodes::InvalidOptions,
                str::stream() << "TTL index '" << IndexDescriptor::kExpireAfterSecondsFieldName
                              << "' option cannot be less than 0"};
    }

    if (mode == ValidateExpireAfterSecondsMode::kSecondaryTTLIndex) {
        // Relax epoch restriction on TTL indexes. This allows us to export and import existing
        // TTL indexes with large values or NaN for the 'expireAfterSeconds' field.
        // Additionally, the 'expireAfterSeconds' for TTL indexes is defined as safeInt (int32_t)
        // in the IDL for listIndexes and collMod. See list_indexes.idl and coll_mod.idl.
        if (expireAfterSeconds > std::numeric_limits<std::int32_t>::max()) {
            return {ErrorCodes::InvalidOptions,
                    str::stream() << "TTL index '" << IndexDescriptor::kExpireAfterSecondsFieldName
                                  << "' option cannot be greater than max int32_t ("
                                  << std::numeric_limits<std::int32_t>::max()
                                  << ") for secondary indexes, found " << expireAfterSeconds};
        }
    } else {
        // Clustered collections with TTL.
        // Note that 'expireAfterSeconds' is defined as safeInt64 in the IDL for the create and
        // collMod commands. See create.idl and coll_mod.idl.
        //
        // There are two cases where we can encounter an issue here.
        // The first case is when we try to cast to milliseconds from seconds, which could cause an
        // overflow. The second case is where 'expireAfterSeconds' is larger than the current epoch
        // time. This isn't necessarily problematic for the general case, but for the specific case
        // of time series collections, we cluster the collection by an OID value, where the
        // timestamp portion is only a 32-bit unsigned integer offset of seconds since the epoch.
        // Rather than special case for timeseries, we apply the same constraints for all clustered
        // TTL indexes to maintain legacy behavior.
        if (expireAfterSeconds > std::numeric_limits<std::int64_t>::max() / 1000) {
            return {ErrorCodes::InvalidOptions,
                    str::stream() << "TTL index '" << IndexDescriptor::kExpireAfterSecondsFieldName
                                  << "' option cannot overflow int64_t when cast as milliseconds, "
                                  << "found " << expireAfterSeconds};
        }
        auto expireAfterMillis = duration_cast<Milliseconds>(Seconds(expireAfterSeconds));
        if (expireAfterMillis > Date_t::now().toDurationSinceEpoch()) {
            return {ErrorCodes::InvalidOptions,
                    str::stream() << "TTL index '" << IndexDescriptor::kExpireAfterSecondsFieldName
                                  << "' option cannot exceed time since last epoch ("
                                  << duration_cast<Seconds>(
                                         Milliseconds(Date_t::now().toDurationSinceEpoch()))
                                  << ") found " << expireAfterSeconds};
        }
    }
    return Status::OK();
}

StatusWith<TTLCollectionCache::Info::ExpireAfterSecondsType> validateExpireAfterSeconds(
    BSONElement expireAfterSeconds, ValidateExpireAfterSecondsMode mode) {
    if (!expireAfterSeconds.isNumber()) {
        return {ErrorCodes::CannotCreateIndex,
                str::stream() << "TTL index '" << IndexDescriptor::kExpireAfterSecondsFieldName
                              << "' option must be numeric, but received a type of '"
                              << typeName(expireAfterSeconds.type())};
    }

    if (expireAfterSeconds.isNaN()) {
        return {ErrorCodes::CannotCreateIndex,
                str::stream() << "TTL index '" << IndexDescriptor::kExpireAfterSecondsFieldName
                              << "' option must not be NaN"};
    }

    // Clustered indexes allow 64-bit integers for expireAfterSeconds, but secondary indexes only
    // allow 32-bit integers, so we check the range here for secondary indexes.
    if (mode == ValidateExpireAfterSecondsMode::kSecondaryTTLIndex &&
        expireAfterSeconds.safeNumberInt() != expireAfterSeconds.safeNumberLong()) {
        return {ErrorCodes::CannotCreateIndex,
                str::stream() << "TTL index '" << IndexDescriptor::kExpireAfterSecondsFieldName
                              << "' must be within the range of a 32-bit integer"};
    }

    if (auto status = validateExpireAfterSeconds(expireAfterSeconds.safeNumberLong(), mode);
        !status.isOK()) {
        return {ErrorCodes::CannotCreateIndex, str::stream() << status.reason()};
    }

    return expireAfterSeconds.type() == BSONType::numberInt
        ? TTLCollectionCache::Info::ExpireAfterSecondsType::kInt
        : TTLCollectionCache::Info::ExpireAfterSecondsType::kNonInt;
}

TTLCollectionCache::Info::ExpireAfterSecondsType extractExpireAfterSecondsType(
    const StatusWith<TTLCollectionCache::Info::ExpireAfterSecondsType>& swType) {
    return swType.isOK() ? swType.getValue()
                         : TTLCollectionCache::Info::ExpireAfterSecondsType::kInvalid;
}

bool isIndexTTL(const BSONObj& indexSpec) {
    return indexSpec.hasField(IndexDescriptor::kExpireAfterSecondsFieldName);
}

Status validateIndexSpecTTL(const BSONObj& indexSpec) {
    if (!isIndexTTL(indexSpec)) {
        return Status::OK();
    }

    if (auto swType =
            validateExpireAfterSeconds(indexSpec[IndexDescriptor::kExpireAfterSecondsFieldName],
                                       ValidateExpireAfterSecondsMode::kSecondaryTTLIndex);
        !swType.isOK()) {
        return swType.getStatus().withContext(str::stream() << ". Index spec: " << indexSpec);
    }

    const BSONObj key = indexSpec["key"].Obj();
    if (key.nFields() != 1) {
        return {ErrorCodes::CannotCreateIndex,
                str::stream() << "TTL indexes are single-field indexes, compound indexes do "
                                 "not support TTL. Index spec: "
                              << indexSpec};
    }

    return Status::OK();
}

bool isIndexAllowedInAPIVersion1(const IndexDescriptor& indexDesc) {
    const auto indexName = IndexNames::findPluginName(indexDesc.keyPattern());
    return indexName != IndexNames::TEXT && indexName != IndexNames::GEO_HAYSTACK &&
        !indexDesc.isSparse();
}

BSONObj parseAndValidateIndexSpecs(OperationContext* opCtx, const BSONObj& indexSpecObj) {
    constexpr auto k_id_ = IndexConstants::kIdIndexName;
    constexpr auto kStar = "*"_sd;

    BSONObj parsedIndexSpec = indexSpecObj;

    auto indexSpecStatus = index_key_validate::validateIndexSpec(opCtx, parsedIndexSpec);
    uassertStatusOK(indexSpecStatus.getStatus().withContext(
        str::stream() << "Error in specification " << parsedIndexSpec.toString()));

    auto indexSpec = indexSpecStatus.getValue();
    if (IndexDescriptor::isIdIndexPattern(indexSpec[IndexDescriptor::kKeyPatternFieldName].Obj())) {
        uassertStatusOK(index_key_validate::validateIdIndexSpec(indexSpec));
    } else {
        uassert(ErrorCodes::BadValue,
                str::stream() << "The index name '_id_' is reserved for the _id index, "
                                 "which must have key pattern {_id: 1}, found "
                              << indexSpec[IndexDescriptor::kKeyPatternFieldName],
                indexSpec[IndexDescriptor::kIndexNameFieldName].String() != k_id_);

        // An index named '*' cannot be dropped on its own, because a dropIndex oplog
        // entry with a '*' as an index name means "drop all indexes in this
        // collection".  We disallow creation of such indexes to avoid this conflict.
        uassert(ErrorCodes::BadValue,
                "The index name '*' is not valid.",
                indexSpec[IndexDescriptor::kIndexNameFieldName].String() != kStar);
    }

    return indexSpec;
}


GlobalInitializerRegisterer filterAllowedIndexFieldNamesInitializer(
    "FilterAllowedIndexFieldNames", [](InitializerContext*) {
        if (filterAllowedIndexFieldNames)
            filterAllowedIndexFieldNames(kAllowedFieldNames);
    });

GlobalInitializerRegisterer nonDeprecatedAllowedFieldNamesInitializer(
    "NonDeprecatedAllowedIndexFieldNames",
    [](InitializerContext*) {
        kNonDeprecatedAllowedFieldNames = kAllowedFieldNames;

        for (const auto& name : kDeprecatedFieldNames) {
            kNonDeprecatedAllowedFieldNames.erase(name);
        }
    },
    nullptr,
    {"FilterAllowedIndexFieldNames"});

}  // namespace index_key_validate
}  // namespace mongo
