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


#include "mongo/db/index/s2_access_method.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/index/expression_keys_private.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/index/s2_key_generator.h"
#include "mongo/db/index_names.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/version_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cmath>
#include <string>
#include <utility>

#include <s2cellid.h>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex


namespace mongo {

static const string kIndexVersionFieldName("2dsphereIndexVersion");

S2AccessMethod::S2AccessMethod(IndexCatalogEntry* btreeState,
                               std::unique_ptr<SortedDataInterface> btree,
                               const std::string& indexName)
    : SortedDataIndexAccessMethod(btreeState, std::move(btree)) {
    const IndexDescriptor* descriptor = btreeState->descriptor();

    index2dsphere::initialize2dsphereParams(
        descriptor->infoObj(), btreeState->getCollator(), &_params);

    int geoFields = 0;

    // Categorize the fields we're indexing and make sure we have a geo field.
    BSONObjIterator i(descriptor->keyPattern());
    while (i.more()) {
        BSONElement e = i.next();
        if (e.type() == BSONType::string && indexName == e.String()) {
            ++geoFields;
        } else {
            // We check for numeric in 2d, so that's the check here
            uassert(16823,
                    (string) "Cannot use " + indexName +
                        " index with other special index types: " + e.toString(),
                    e.isNumber());
        }
    }

    uassert(16750,
            "Expect at least one geo field, spec=" + descriptor->keyPattern().toString(),
            geoFields >= 1);

    if (descriptor->isSetSparseByUser()) {
        LOGV2_WARNING(23742,
                      "Sparse option ignored for index spec",
                      "indexSpec"_attr = descriptor->keyPattern());
    }
}

std::string formatAllowedVersions(const std::set<long long>& allowedVersions) {
    std::string result;
    bool first = true;
    for (long long version : allowedVersions) {
        if (!first) {
            result += ",";
        }
        result += std::to_string(version);
        first = false;
    }
    return result;
}

StatusWith<BSONObj> cannotCreateIndexStatus(BSONElement indexVersionElt,
                                            const std::string& message,
                                            const std::string& expectedVersions = str::stream()
                                                << S2_INDEX_VERSION_1 << "," << S2_INDEX_VERSION_2
                                                << "," << S2_INDEX_VERSION_3 << ","
                                                << S2_INDEX_VERSION_4,
                                            const std::string& extraMessage = "") {
    return {ErrorCodes::CannotCreateIndex,
            str::stream() << message << " { " << kIndexVersionFieldName << " : " << indexVersionElt
                          << " }, only versions: [" << expectedVersions << "] are supported"
                          << extraMessage};
}

StatusWith<BSONObj> S2AccessMethod::_fixSpecHelper(
    const BSONObj& specObj, boost::optional<std::set<long long>> allowedVersions) {
    // If the spec object doesn't have field "2dsphereIndexVersion", add the default version
    // based on the feature flag.
    BSONElement indexVersionElt = specObj[kIndexVersionFieldName];
    long long defaultVersion = static_cast<long long>(index2dsphere::getDefaultS2IndexVersion());
    if (indexVersionElt.eoo()) {
        // Validate the default version against allowed versions if provided.
        if (allowedVersions && !allowedVersions->contains(defaultVersion)) {
            return {ErrorCodes::CannotCreateIndex,
                    str::stream() << "Default geo index version " << defaultVersion
                                  << " is not in the allowed set: ["
                                  << formatAllowedVersions(*allowedVersions) << "]"};
        }
        BSONObjBuilder bob;
        bob.appendElements(specObj);
        bob.append(kIndexVersionFieldName, defaultVersion);
        return bob.obj();
    }

    // Otherwise, validate the index version.
    if (!indexVersionElt.isNumber()) {
        return cannotCreateIndexStatus(indexVersionElt, "Invalid type for geo index version");
    }

    if (indexVersionElt.type() == BSONType::numberDouble &&
        !std::isnormal(indexVersionElt.numberDouble())) {
        return cannotCreateIndexStatus(indexVersionElt, "Invalid value for geo index version");
    }

    long long indexVersion = indexVersionElt.safeNumberLong();

    if (allowedVersions && !allowedVersions->contains(indexVersion)) {
        // If we have allowedVersions, validate that the index version is in the set.
        return cannotCreateIndexStatus(indexVersionElt,
                                       "unsupported geo index version",
                                       formatAllowedVersions(*allowedVersions),
                                       "");
    } else {
        // Index version must be either 1, 2, 3, or 4.
        switch (indexVersion) {
            case S2_INDEX_VERSION_1:
            case S2_INDEX_VERSION_2:
            case S2_INDEX_VERSION_3:
                break;
            case S2_INDEX_VERSION_4: {
                // Gate version 4 behind feature flag.
                if (index2dsphere::getDefaultS2IndexVersion() != S2_INDEX_VERSION_4) {
                    return Status(ErrorCodes::CannotCreateIndex,
                                  "2dsphereIndexVersion 4 requires feature flag "
                                  "'featureFlag2dsphereIndexVersion4' to be enabled");
                }
                break;
            }
            default:
                return cannotCreateIndexStatus(indexVersionElt, "unsupported geo index version");
        }
    }
    return specObj;
}

// static
StatusWith<BSONObj> S2AccessMethod::fixSpec(const BSONObj& specObj) {
    return S2AccessMethod::_fixSpecHelper(specObj);
}

// static
KeyStringSet S2AccessMethod::generateKeysForValidation(const BSONObj& indexSpec,
                                                       const CollatorInterface* collator,
                                                       const BSONObj& document,
                                                       Ordering ordering,
                                                       const boost::optional<RecordId>& recordId,
                                                       key_string::Version keyStringVersion) {
    S2IndexingParams params;
    index2dsphere::initialize2dsphereParams(indexSpec, collator, &params);
    // Force version 4 for validation comparison
    params.indexVersion = S2_INDEX_VERSION_4;

    SharedBufferFragmentBuilder pool(key_string::HeapBuilder::kHeapAllocatorDefaultBytes);
    KeyStringSet keys;
    MultikeyPaths multikeyPaths;

    BSONObj keyPattern = indexSpec.getObjectField("key");
    index2dsphere::getS2Keys(pool,
                             document,
                             keyPattern,
                             params,
                             &keys,
                             &multikeyPaths,
                             keyStringVersion,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             ordering,
                             recordId);

    return keys;
}

// static
bool S2AccessMethod::isVersion3(const BSONObj& indexSpec) {
    BSONElement versionElt = indexSpec["2dsphereIndexVersion"];
    return versionElt.isNumber() && versionElt.numberInt() == S2_INDEX_VERSION_3;
}

bool S2AccessMethod::shouldCheckMissingIndexEntryAlternative(OperationContext* opCtx,
                                                             const IndexCatalogEntry& entry) const {
    // Only proceed with expensive record lookup for version 3 indexes, which may need
    // to be checked for version 4 upgrade scenarios.
    return isVersion3(entry.descriptor()->infoObj());
}

boost::optional<std::pair<std::string, std::string>>
S2AccessMethod::checkMissingIndexEntryAlternative(OperationContext* opCtx,
                                                  const IndexCatalogEntry& entry,
                                                  const key_string::Value& missingKey,
                                                  const RecordId& recordId,
                                                  const BSONObj& document) const {
    // Only check for version 3 to version 4 upgrade scenarios.
    if (!isVersion3(entry.descriptor()->infoObj())) {
        return boost::none;
    }

    try {
        // Generate version 4 keys for this document.
        KeyStringSet keysV4 =
            generateKeysForValidation(entry.descriptor()->infoObj(),
                                      entry.getCollator(),
                                      document,
                                      getSortedDataInterface()->getOrdering(),
                                      recordId,
                                      getSortedDataInterface()->getKeyStringVersion());

        // Check if version 4 keys exist in the index. If they do, this indicates the
        // validation failure was caused by SERVER-84794 and the index should be
        // upgraded to v4.
        if (!keysV4.empty()) {
            auto sortedDataInterface = getSortedDataInterface();
            auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
            auto cursor = sortedDataInterface->newCursor(opCtx, ru);
            bool foundMatchingKey =
                std::any_of(keysV4.begin(), keysV4.end(), [&](const auto& keyV4) {
                    // seekForKeyString checks if the key exists in the index and
                    // returns the KeyStringEntry with the RecordId if found.
                    auto ksEntry = cursor->seekForKeyString(ru, keyV4.getView());
                    return ksEntry && ksEntry->loc == recordId;
                });

            if (foundMatchingKey) {
                const std::string indexName = entry.descriptor()->indexName();
                std::string errorMsg = "Index '" + indexName +
                    "' was created with 2dsphereIndexVersion 3, but validation indicates it should "
                    "be "
                    "rebuilt with version 4. Please drop and recreate this index with version 4.";
                std::string warningMsg =
                    "Index '" + indexName + "' needs to be rebuilt with 2dsphereIndexVersion 4.";
                return std::make_pair(errorMsg, warningMsg);
            }
        }
    } catch (...) {
        // If key generation fails with version 4, continue with normal error reporting.
    }

    return boost::none;
}

void S2AccessMethod::validateDocument(const CollectionPtr& collection,
                                      const BSONObj& obj,
                                      const BSONObj& keyPattern) const {
    ExpressionKeysPrivate::validateDocumentCommon(collection, obj, keyPattern);
}

void S2AccessMethod::doGetKeys(OperationContext* opCtx,
                               const CollectionPtr& collection,
                               const IndexCatalogEntry* entry,
                               SharedBufferFragmentBuilder& pooledBufferBuilder,
                               const BSONObj& obj,
                               GetKeysContext context,
                               KeyStringSet* keys,
                               KeyStringSet* multikeyMetadataKeys,
                               MultikeyPaths* multikeyPaths,
                               const boost::optional<RecordId>& id) const {
    index2dsphere::getS2Keys(pooledBufferBuilder,
                             obj,
                             entry->descriptor()->keyPattern(),
                             _params,
                             keys,
                             multikeyPaths,
                             getSortedDataInterface()->getKeyStringVersion(),
                             context,
                             getSortedDataInterface()->getOrdering(),
                             id);
}

}  // namespace mongo
