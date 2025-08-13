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

#include "mongo/db/local_catalog/index_descriptor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/index_path_projection.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex


namespace mongo {

namespace {

/**
 * Returns wildcardProjection projection
 */
BSONObj createPathProjection(const BSONObj& infoObj) {
    if (const auto wildcardProjection = infoObj[IndexDescriptor::kWildcardProjectionFieldName]) {
        return wildcardProjection.Obj().getOwned();
    } else {
        return BSONObj();
    }
}

}  // namespace

using IndexVersion = IndexDescriptor::IndexVersion;

namespace {
std::map<StringData, BSONElement> populateOptionsMapForEqualityCheck(const BSONObj& spec) {
    std::map<StringData, BSONElement> optionsMap;

    // These index options are not considered for equality.
    static const StringDataSet kIndexOptionsNotConsideredForEqualityCheck{
        IndexDescriptor::kKeyPatternFieldName,  // checked specially
        // TODO(SERVER-100328): remove after 9.0 is branched.
        IndexDescriptor::kNamespaceFieldName,           // removed in 4.4
        IndexDescriptor::kIndexNameFieldName,           // checked separately
        IndexDescriptor::kIndexVersionFieldName,        // not considered for equivalence
        IndexDescriptor::kTextVersionFieldName,         // same as index version
        IndexDescriptor::k2dsphereVersionFieldName,     // same as index version
        IndexDescriptor::kBackgroundFieldName,          // this is a creation time option only
        IndexDescriptor::kDropDuplicatesFieldName,      // this is now ignored
        IndexDescriptor::kCollationFieldName,           // checked specially
        IndexDescriptor::kPartialFilterExprFieldName,   // checked specially
        IndexDescriptor::kUniqueFieldName,              // checked specially
        IndexDescriptor::kSparseFieldName,              // checked specially
        IndexDescriptor::kWildcardProjectionFieldName,  // checked specially
    };

    BSONObjIterator it(spec);
    while (it.more()) {
        const BSONElement e = it.next();

        StringData fieldName = e.fieldNameStringData();
        if (kIndexOptionsNotConsideredForEqualityCheck.count(fieldName) == 0) {
            optionsMap[fieldName] = e;
        }
    }

    return optionsMap;
}
}  // namespace

constexpr StringData IndexDescriptor::k2dIndexBitsFieldName;
constexpr StringData IndexDescriptor::k2dIndexMaxFieldName;
constexpr StringData IndexDescriptor::k2dIndexMinFieldName;
constexpr StringData IndexDescriptor::k2dsphereCoarsestIndexedLevel;
constexpr StringData IndexDescriptor::k2dsphereFinestIndexedLevel;
constexpr StringData IndexDescriptor::k2dsphereVersionFieldName;
constexpr StringData IndexDescriptor::kBackgroundFieldName;
constexpr StringData IndexDescriptor::kCollationFieldName;
constexpr StringData IndexDescriptor::kDefaultLanguageFieldName;
constexpr StringData IndexDescriptor::kDropDuplicatesFieldName;
constexpr StringData IndexDescriptor::kExpireAfterSecondsFieldName;
constexpr StringData IndexDescriptor::kIndexNameFieldName;
constexpr StringData IndexDescriptor::kIndexVersionFieldName;
constexpr StringData IndexDescriptor::kKeyPatternFieldName;
constexpr StringData IndexDescriptor::kLanguageOverrideFieldName;
// TODO(SERVER-100328): remove after 9.0 is branched.
constexpr StringData IndexDescriptor::kNamespaceFieldName;
constexpr StringData IndexDescriptor::kPartialFilterExprFieldName;
constexpr StringData IndexDescriptor::kWildcardProjectionFieldName;
constexpr StringData IndexDescriptor::kSparseFieldName;
constexpr StringData IndexDescriptor::kStorageEngineFieldName;
constexpr StringData IndexDescriptor::kTextVersionFieldName;
constexpr StringData IndexDescriptor::kUniqueFieldName;
constexpr StringData IndexDescriptor::kHiddenFieldName;
constexpr StringData IndexDescriptor::kWeightsFieldName;
constexpr StringData IndexDescriptor::kPrepareUniqueFieldName;

/**
 * Constructs an IndexDescriptor object. Arguments:
 *   accessMethodName - one of the 'IndexNames::XXX' constants from index_names.cpp
 *   infoObj          - options information
 */
IndexDescriptor::IndexDescriptor(const std::string& accessMethodName, BSONObj infoObj)
    : _shared(make_intrusive<SharedState>(accessMethodName, infoObj)) {}

IndexDescriptor::SharedState::SharedState(const std::string& accessMethodName, BSONObj infoObj)
    : _accessMethodName(accessMethodName),
      _indexType(IndexNames::nameToType(accessMethodName)),
      _infoObj(infoObj.getOwned()),
      _numFields(infoObj.getObjectField(IndexDescriptor::kKeyPatternFieldName).nFields()),
      _keyPattern(infoObj.getObjectField(IndexDescriptor::kKeyPatternFieldName).getOwned()),
      _projection(createPathProjection(infoObj)),
      _indexName(infoObj.getStringField(IndexDescriptor::kIndexNameFieldName)),
      _isIdIndex(isIdIndexPattern(_keyPattern)),
      _isHashedIdIndex(isHashedIdIndex(_keyPattern)),
      _sparse(infoObj[IndexDescriptor::kSparseFieldName].trueValue()),
      _unique(_isIdIndex || infoObj[kUniqueFieldName].trueValue()),
      _hidden(infoObj[kHiddenFieldName].trueValue()),
      _partial(!infoObj[kPartialFilterExprFieldName].eoo()),
      _ordering(_indexType == IndexType::INDEX_WILDCARD
                    ? WildcardAccessMethod::makeOrdering(_keyPattern)
                    : Ordering::make(_keyPattern)) {
    BSONElement e = _infoObj[IndexDescriptor::kIndexVersionFieldName];
    fassert(50942, e.isNumber());
    _version = static_cast<IndexVersion>(e.numberInt());

    if (BSONElement filterElement = _infoObj[kPartialFilterExprFieldName]) {
        invariant(filterElement.isABSONObj());
        _partialFilterExpression = filterElement.Obj().getOwned();
    }

    if (BSONElement collationElement = _infoObj[kCollationFieldName]) {
        invariant(collationElement.isABSONObj());
        _collation = collationElement.Obj().getOwned();
    }

    if (BSONElement prepareUniqueElement = _infoObj[kPrepareUniqueFieldName]) {
        _prepareUnique = prepareUniqueElement.trueValue();
    }


    _compressor = boost::none;

    // If there is a wildcardProjection, compute and store the normalized
    // version in '_normalizedProjection'.
    BSONElement wildcardProjection = infoObj[IndexDescriptor::kWildcardProjectionFieldName];
    if (wildcardProjection) {
        IndexPathProjection indexPathProjection =
            static_cast<IndexPathProjection>(WildcardKeyGenerator::createProjectionExecutor(
                BSON("$**" << 1), wildcardProjection.Obj()));
        _normalizedProjection = indexPathProjection.exec()->serializeTransformation().toBson();
    }
}

bool IndexDescriptor::isIndexVersionSupported(IndexVersion indexVersion) {
    switch (indexVersion) {
        case IndexVersion::kV1:
        case IndexVersion::kV2:
            return true;
    }
    return false;
}

IndexVersion IndexDescriptor::getDefaultIndexVersion() {
    return IndexVersion::kV2;
}

IndexDescriptor::Comparison IndexDescriptor::compareIndexOptions(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const IndexCatalogEntry* existingIndex) const {
    auto existingIndexDesc = existingIndex->descriptor();

    // We first check whether the key pattern is identical for both indexes.
    if (SimpleBSONObjComparator::kInstance.evaluate(keyPattern() !=
                                                    existingIndexDesc->keyPattern())) {
        return Comparison::kDifferent;
    }

    // If the candidate has a wildcardProjection, we must compare the
    // normalized versions, not the versions from the catalog which are kept as the user gave them
    // and thus may be semantically identical to but syntactically different from the normalized
    // form. There are no other types of index projections. Thus, if there is no projection, both
    // the original and normalized projections will be empty BSON objects, so we can still do the
    // comparison based on the normalized projection.
    static const UnorderedFieldsBSONObjComparator kUnorderedBSONCmp;
    if (kUnorderedBSONCmp.evaluate(_shared->_normalizedProjection !=
                                   existingIndexDesc->_shared->_normalizedProjection)) {
        return Comparison::kDifferent;
    }

    if (unique() != existingIndexDesc->unique()) {
        return Comparison::kDifferent;
    }

    if (isSparse() != existingIndexDesc->isSparse()) {
        return Comparison::kDifferent;
    }

    // Check whether both indexes have the same collation. If not, then they are not equivalent.
    auto collator = collation().isEmpty()
        ? nullptr
        : uassertStatusOK(
              CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation()));
    if (!CollatorInterface::collatorsMatch(collator.get(), existingIndex->getCollator())) {
        return Comparison::kDifferent;
    }

    // If we have a partialFilterExpression and the existingIndex doesn't, or vice-versa, then the
    // two indexes are not equivalent. We therefore return Comparison::kDifferent immediately.
    if (isPartial() != existingIndexDesc->isPartial()) {
        return Comparison::kDifferent;
    }
    // Compare 'partialFilterExpression' in each descriptor to see if they are equivalent. We use
    // the collator that we parsed earlier to create the filter's ExpressionContext, although we
    // don't currently consider collation when comparing string predicates for filter equivalence.
    // For instance, under a case-sensitive collation, the predicates {a: "blah"} and {a: "BLAH"}
    // would match the same set of documents, but these are not currently considered equivalent.
    // TODO SERVER-47664: take collation into account while comparing string predicates.
    if (existingIndex->getFilterExpression()) {
        auto expCtx =
            ExpressionContextBuilder{}.opCtx(opCtx).collator(std::move(collator)).ns(ns).build();
        auto filter = MatchExpressionParser::parseAndNormalize(partialFilterExpression(), expCtx);
        if (!filter->equivalent(existingIndex->getFilterExpression())) {
            return Comparison::kDifferent;
        }
    }

    // If we are here, then the two descriptors match on all option fields that uniquely distinguish
    // an index, and so the return value will be at least Comparison::kEquivalent. We now proceed to
    // compare the rest of the options to see if we should return Comparison::kIdentical instead.

    auto thisOptionsMap = populateOptionsMapForEqualityCheck(infoObj());
    auto existingIndexOptionsMap = populateOptionsMapForEqualityCheck(existingIndexDesc->infoObj());

    const bool optsIdentical = thisOptionsMap.size() == existingIndexOptionsMap.size() &&
        std::equal(thisOptionsMap.begin(),
                   thisOptionsMap.end(),
                   existingIndexOptionsMap.begin(),
                   [](const std::pair<StringData, BSONElement>& lhs,
                      const std::pair<StringData, BSONElement>& rhs) {
                       return lhs.first == rhs.first &&
                           SimpleBSONElementComparator::kInstance.evaluate(lhs.second ==
                                                                           rhs.second);
                   });

    // If all non-identifying options also match, the descriptors are identical. Otherwise, we
    // consider them equivalent; two indexes with these options and the same key cannot coexist.
    return optsIdentical ? Comparison::kIdentical : Comparison::kEquivalent;
}

std::vector<const char*> IndexDescriptor::getFieldNames() const {
    constexpr auto kFTSTerm = "term"_sd;
    constexpr auto kFTSWeight = "weight"_sd;
    constexpr auto kFTSFieldName = "_fts"_sd;
    constexpr auto kFTSXFieldName = "_ftsx"_sd;

    std::vector<const char*> fieldNames;

    // This field is only applicable for the text index and if set to true, then the 'term' and the
    // 'weight' field names have already been added to the 'fieldNames'.
    bool hasSeenFtsOrFtsxFields = false;

    // Appends the 'term' and 'weight' fields to the 'fieldNames' vector if 'hasSeenFtsOrFtsxFields'
    // is set to false.
    auto maybeAppendFtsIndexField = [&]() {
        if (!hasSeenFtsOrFtsxFields) {
            fieldNames.push_back(kFTSTerm.data());
            fieldNames.push_back(kFTSWeight.data());
            hasSeenFtsOrFtsxFields = true;
        }
    };

    // Iterate over the key pattern and add the field names to the 'fieldNames' vector.
    BSONObjIterator keyPatternIter(_shared->_keyPattern);
    while (keyPatternIter.more()) {
        BSONElement KeyPatternElem = keyPatternIter.next();
        auto fieldName = KeyPatternElem.fieldNameStringData();

        // If the index type is text and the field name is either '_fts' or '_ftsx', then append the
        // index fields to the field names, otherwise add the field name from the key pattern.
        if ((_shared->_indexType == IndexType::INDEX_TEXT) &&
            (fieldName == kFTSFieldName || fieldName == kFTSXFieldName)) {
            maybeAppendFtsIndexField();
        } else {
            fieldNames.push_back(fieldName.data());
        }
    }

    // If the index type is text and the 'hasSeenFtsOrFtsxFields' is set to false, then append the
    // index fields.
    if (_shared->_indexType == IndexType::INDEX_TEXT) {
        maybeAppendFtsIndexField();
    }

    return fieldNames;
}

}  // namespace mongo
