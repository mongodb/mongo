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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/index/index_descriptor.h"

#include <algorithm>

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/server_options.h"

namespace mongo {

using IndexVersion = IndexDescriptor::IndexVersion;

namespace {
std::map<StringData, BSONElement> populateOptionsMapForEqualityCheck(const BSONObj& spec) {
    std::map<StringData, BSONElement> optionsMap;

    // These index options are not considered for equality.
    static const StringDataSet kIndexOptionsNotConsideredForEqualityCheck{
        IndexDescriptor::kKeyPatternFieldName,         // checked specially
        IndexDescriptor::kNamespaceFieldName,          // removed in 4.4
        IndexDescriptor::kIndexNameFieldName,          // checked separately
        IndexDescriptor::kIndexVersionFieldName,       // not considered for equivalence
        IndexDescriptor::kTextVersionFieldName,        // same as index version
        IndexDescriptor::k2dsphereVersionFieldName,    // same as index version
        IndexDescriptor::kBackgroundFieldName,         // this is a creation time option only
        IndexDescriptor::kDropDuplicatesFieldName,     // this is now ignored
        IndexDescriptor::kHiddenFieldName,             // not considered for equivalence
        IndexDescriptor::kCollationFieldName,          // checked specially
        IndexDescriptor::kPartialFilterExprFieldName,  // checked specially
        IndexDescriptor::kUniqueFieldName,             // checked specially
        IndexDescriptor::kSparseFieldName,             // checked specially
        IndexDescriptor::kPathProjectionFieldName,     // checked specially
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
constexpr StringData IndexDescriptor::kGeoHaystackBucketSize;
constexpr StringData IndexDescriptor::kIndexNameFieldName;
constexpr StringData IndexDescriptor::kIndexVersionFieldName;
constexpr StringData IndexDescriptor::kKeyPatternFieldName;
constexpr StringData IndexDescriptor::kLanguageOverrideFieldName;
constexpr StringData IndexDescriptor::kNamespaceFieldName;
constexpr StringData IndexDescriptor::kPartialFilterExprFieldName;
constexpr StringData IndexDescriptor::kPathProjectionFieldName;
constexpr StringData IndexDescriptor::kSparseFieldName;
constexpr StringData IndexDescriptor::kStorageEngineFieldName;
constexpr StringData IndexDescriptor::kTextVersionFieldName;
constexpr StringData IndexDescriptor::kUniqueFieldName;
constexpr StringData IndexDescriptor::kHiddenFieldName;
constexpr StringData IndexDescriptor::kWeightsFieldName;

IndexDescriptor::IndexDescriptor(const std::string& accessMethodName, BSONObj infoObj)
    : _accessMethodName(accessMethodName),
      _indexType(IndexNames::nameToType(accessMethodName)),
      _infoObj(infoObj.getOwned()),
      _numFields(infoObj.getObjectField(IndexDescriptor::kKeyPatternFieldName).nFields()),
      _keyPattern(infoObj.getObjectField(IndexDescriptor::kKeyPatternFieldName).getOwned()),
      _projection(infoObj.getObjectField(IndexDescriptor::kPathProjectionFieldName).getOwned()),
      _indexName(infoObj.getStringField(IndexDescriptor::kIndexNameFieldName)),
      _isIdIndex(isIdIndexPattern(_keyPattern)),
      _sparse(infoObj[IndexDescriptor::kSparseFieldName].trueValue()),
      _unique(_isIdIndex || infoObj[kUniqueFieldName].trueValue()),
      _hidden(infoObj[kHiddenFieldName].trueValue()),
      _partial(!infoObj[kPartialFilterExprFieldName].eoo()) {
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
    // The compareIndexOptions method can only be reliably called on a candidate index which is
    // being compared against an index that already exists in the catalog.
    tassert(4765900, "This object must be a candidate index", !getEntry());

    auto existingIndexDesc = existingIndex->descriptor();

    // We first check whether the key pattern is identical for both indexes.
    if (SimpleBSONObjComparator::kInstance.evaluate(keyPattern() !=
                                                    existingIndexDesc->keyPattern())) {
        return Comparison::kDifferent;
    }

    auto fcv = serverGlobalParams.featureCompatibility.getVersion();
    auto isFcvAtLeast50 = fcv >= ServerGlobalParams::FeatureCompatibility::Version::kVersion50;

    // TODO SERVER-47766: remove this FCV check when 5.0 becomes last-lts.
    if (isFcvAtLeast50) {
        // The 'wildcardProjection' field is a part of index signature if FCV has been set to 5.0+.
        // Compare 'wildcardProjection' in each index descriptor. Ignore field order when comparing.
        // The candidate index descriptor's _projection has already been normalized upstream. The
        // existing index's _normalizedProjection is populated when its associated IndexCatalogEntry
        // is created.
        static const UnorderedFieldsBSONObjComparator kUnorderedBSONCmp;
        if (kUnorderedBSONCmp.evaluate(_projection != existingIndexDesc->_normalizedProjection)) {
            return Comparison::kDifferent;
        }
    }

    auto isFcvAtLeast49 =
        isFcvAtLeast50 || fcv >= ServerGlobalParams::FeatureCompatibility::Version::kVersion49;

    // TODO SERVER-47766: remove these FCV checks when 5.0 becomes last-lts.
    if (isFcvAtLeast49) {
        // The 'unique' field is a part of index signature if FCV has been set to 4.9+.
        if (unique() != existingIndexDesc->unique()) {
            return Comparison::kDifferent;
        }

        // The 'sparse' field is a part of index signature if FCV has been set to 4.9+.
        if (isSparse() != existingIndexDesc->isSparse()) {
            return Comparison::kDifferent;
        }
    }

    // Check whether both indexes have the same collation. If not, then they are not equivalent.
    auto collator = collation().isEmpty()
        ? nullptr
        : uassertStatusOK(
              CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation()));
    if (!CollatorInterface::collatorsMatch(collator.get(), existingIndex->getCollator())) {
        return Comparison::kDifferent;
    }

    // The partialFilterExpression is only part of the index signature if FCV has been set to 4.7+.
    // TODO SERVER-47766: remove these FCV checks when 5.0 becomes last-lts.
    auto isFCVAtLeast47 =
        isFcvAtLeast49 || fcv >= ServerGlobalParams::FeatureCompatibility::Version::kVersion47;

    // If we have a partialFilterExpression and the existingIndex doesn't, or vice-versa, then the
    // two indexes are not equivalent. We therefore return Comparison::kDifferent immediately.
    if (isFCVAtLeast47 && isPartial() != existingIndexDesc->isPartial()) {
        return Comparison::kDifferent;
    }
    // Compare 'partialFilterExpression' in each descriptor to see if they are equivalent. We use
    // the collator that we parsed earlier to create the filter's ExpressionContext, although we
    // don't currently consider collation when comparing string predicates for filter equivalence.
    // For instance, under a case-sensitive collation, the predicates {a: "blah"} and {a: "BLAH"}
    // would match the same set of documents, but these are not currently considered equivalent.
    // TODO SERVER-47664: take collation into account while comparing string predicates.
    if (isFCVAtLeast47 && existingIndex->getFilterExpression()) {
        auto expCtx = make_intrusive<ExpressionContext>(opCtx, std::move(collator), ns);
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

    // If the FCV has not been upgraded to 5.0+, add wildcardProjection to the options map. They do
    // not contribute to the index signature, but can determine whether or not the candidate index
    // is identical to the existing index.
    if (!isFcvAtLeast50) {
        thisOptionsMap[IndexDescriptor::kPathProjectionFieldName] =
            infoObj()[IndexDescriptor::kPathProjectionFieldName];
        existingIndexOptionsMap[IndexDescriptor::kPathProjectionFieldName] =
            existingIndexDesc->infoObj()[IndexDescriptor::kPathProjectionFieldName];
    }

    // If the FCV has not been upgraded to 4.9+, add unique/sparse to the options map. They do not
    // contribute to the index signature, but can determine whether or not the candidate index is
    // identical to the existing index.
    if (!isFcvAtLeast49) {
        thisOptionsMap[IndexDescriptor::kUniqueFieldName] =
            infoObj()[IndexDescriptor::kUniqueFieldName];
        existingIndexOptionsMap[IndexDescriptor::kUniqueFieldName] =
            existingIndexDesc->infoObj()[IndexDescriptor::kUniqueFieldName];

        thisOptionsMap[IndexDescriptor::kSparseFieldName] =
            infoObj()[IndexDescriptor::kSparseFieldName];
        existingIndexOptionsMap[IndexDescriptor::kSparseFieldName] =
            existingIndexDesc->infoObj()[IndexDescriptor::kSparseFieldName];
    }

    // If the FCV has not been upgraded to 4.7+, add partialFilterExpression to the options map. It
    // does not contribute to the index signature, but can determine whether or not the candidate
    // index is identical to the existing index.
    if (!isFCVAtLeast47) {
        thisOptionsMap[IndexDescriptor::kPartialFilterExprFieldName] =
            infoObj()[IndexDescriptor::kPartialFilterExprFieldName];
        existingIndexOptionsMap[IndexDescriptor::kPartialFilterExprFieldName] =
            existingIndexDesc->infoObj()[IndexDescriptor::kPartialFilterExprFieldName];
    }

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

}  // namespace mongo
