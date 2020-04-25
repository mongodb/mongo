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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/collation/collator_factory_interface.h"

#include <algorithm>

#include "mongo/bson/simple_bsonelement_comparator.h"

namespace mongo {

using IndexVersion = IndexDescriptor::IndexVersion;

namespace {
void populateOptionsMap(std::map<StringData, BSONElement>& theMap, const BSONObj& spec) {
    BSONObjIterator it(spec);
    while (it.more()) {
        const BSONElement e = it.next();

        StringData fieldName = e.fieldNameStringData();
        if (fieldName == IndexDescriptor::kKeyPatternFieldName ||  // checked specially
            fieldName == IndexDescriptor::kNamespaceFieldName ||   // removed in 4.4
            fieldName == IndexDescriptor::kIndexNameFieldName ||   // checked separately
            fieldName ==
                IndexDescriptor::kIndexVersionFieldName ||  // not considered for equivalence
            fieldName == IndexDescriptor::kTextVersionFieldName ||      // same as index version
            fieldName == IndexDescriptor::k2dsphereVersionFieldName ||  // same as index version
            fieldName ==
                IndexDescriptor::kBackgroundFieldName ||  // this is a creation time option only
            fieldName == IndexDescriptor::kDropDuplicatesFieldName ||  // this is now ignored
            fieldName == IndexDescriptor::kHiddenFieldName ||     // not considered for equivalence
            fieldName == IndexDescriptor::kCollationFieldName ||  // checked specially
            fieldName == IndexDescriptor::kPartialFilterExprFieldName  // checked specially
        ) {
            continue;
        }
        theMap[fieldName] = e;
    }
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

IndexDescriptor::IndexDescriptor(Collection* collection,
                                 const std::string& accessMethodName,
                                 BSONObj infoObj)
    : _collection(collection),
      _accessMethodName(accessMethodName),
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
      _partial(!infoObj[kPartialFilterExprFieldName].eoo()),
      _cachedEntry(nullptr) {
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

std::set<IndexVersion> IndexDescriptor::getSupportedIndexVersions() {
    return {IndexVersion::kV1, IndexVersion::kV2};
}

Status IndexDescriptor::isIndexVersionAllowedForCreation(
    IndexVersion indexVersion,
    const ServerGlobalParams::FeatureCompatibility& featureCompatibility,
    const BSONObj& indexSpec) {
    switch (indexVersion) {
        case IndexVersion::kV1:
        case IndexVersion::kV2:
            return Status::OK();
    }
    return {ErrorCodes::CannotCreateIndex,
            str::stream() << "Invalid index specification " << indexSpec
                          << "; cannot create an index with v=" << static_cast<int>(indexVersion)};
}

IndexVersion IndexDescriptor::getDefaultIndexVersion() {
    return IndexVersion::kV2;
}

bool IndexDescriptor::isMultikey() const {
    return _collection->getIndexCatalog()->isMultikey(this);
}

MultikeyPaths IndexDescriptor::getMultikeyPaths(OperationContext* opCtx) const {
    return _collection->getIndexCatalog()->getMultikeyPaths(opCtx, this);
}

const IndexCatalog* IndexDescriptor::getIndexCatalog() const {
    return _collection->getIndexCatalog();
}

const NamespaceString& IndexDescriptor::parentNS() const {
    return _collection->ns();
}

IndexDescriptor::Comparison IndexDescriptor::compareIndexOptions(
    OperationContext* opCtx, const IndexCatalogEntry* other) const {
    // We first check whether the key pattern is identical for both indexes.
    if (SimpleBSONObjComparator::kInstance.evaluate(keyPattern() !=
                                                    other->descriptor()->keyPattern())) {
        return Comparison::kDifferent;
    }

    // Check whether both indexes have the same collation. If not, then they are not equivalent.
    auto collator = collation().isEmpty()
        ? nullptr
        : uassertStatusOK(
              CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation()));
    if (!CollatorInterface::collatorsMatch(collator.get(), other->getCollator())) {
        return Comparison::kDifferent;
    }

    // The partialFilterExpression is only part of the index signature if FCV has been set to 4.6.
    // TODO SERVER-47766: remove these FCV checks after we branch for 4.7.
    auto isFCV46 = serverGlobalParams.featureCompatibility.isVersion(
        ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo46);

    // If we have a partial filter expression and the other index doesn't, or vice-versa, then the
    // two indexes are not equivalent. We therefore return Comparison::kDifferent immediately.
    if (isFCV46 && isPartial() != other->descriptor()->isPartial()) {
        return Comparison::kDifferent;
    }
    // Compare 'partialFilterExpression' in each descriptor to see if they are equivalent. We use
    // the collator that we parsed earlier to create the filter's ExpressionContext, although we
    // don't currently consider collation when comparing string predicates for filter equivalence.
    // For instance, under a case-sensitive collation, the predicates {a: "blah"} and {a: "BLAH"}
    // would match the same set of documents, but these are not currently considered equivalent.
    // TODO SERVER-47664: take collation into account while comparing string predicates.
    if (isFCV46 && other->getFilterExpression()) {
        auto expCtx =
            make_intrusive<ExpressionContext>(opCtx, std::move(collator), _collection->ns());
        auto filter = MatchExpressionParser::parseAndNormalize(partialFilterExpression(), expCtx);
        if (!filter->equivalent(other->getFilterExpression())) {
            return Comparison::kDifferent;
        }
    }

    // If we are here, then the two descriptors match on all option fields that uniquely distinguish
    // an index, and so the return value will be at least Comparison::kEquivalent. We now proceed to
    // compare the rest of the options to see if we should return Comparison::kIdentical instead.

    std::map<StringData, BSONElement> existingOptionsMap;
    populateOptionsMap(existingOptionsMap, infoObj());

    std::map<StringData, BSONElement> newOptionsMap;
    populateOptionsMap(newOptionsMap, other->descriptor()->infoObj());

    // If the FCV has not been upgraded to 4.6, add partialFilterExpression to the options map. It
    // does not contribute to the index signature, but can determine whether or not the candidate
    // index is identical to the existing index.
    if (!isFCV46) {
        existingOptionsMap[IndexDescriptor::kPartialFilterExprFieldName] =
            other->descriptor()->infoObj()[IndexDescriptor::kPartialFilterExprFieldName];
        newOptionsMap[IndexDescriptor::kPartialFilterExprFieldName] =
            infoObj()[IndexDescriptor::kPartialFilterExprFieldName];
    }

    const bool optsIdentical = existingOptionsMap.size() == newOptionsMap.size() &&
        std::equal(existingOptionsMap.begin(),
                   existingOptionsMap.end(),
                   newOptionsMap.begin(),
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
