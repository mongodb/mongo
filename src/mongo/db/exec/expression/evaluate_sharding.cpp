/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/expression/evaluate_sharding.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/catalog_cache/shard_cannot_refresh_due_to_locks_held_exception.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/index/2d_common.h"
#include "mongo/db/index/2d_key_generator.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/index/expression_keys_private.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/index/s2_key_generator.h"
#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/index_key_validate.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"

namespace mongo {

namespace exec::expression {

Value evaluate(const ExpressionInternalOwningShard& expr,
               const Document& root,
               Variables* variables) {
    uassert(6868600,
            "$_internalOwningShard is currently not supported on mongos",
            !serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer));

    auto expCtx = expr.getExpressionContext();

    Value input = expr.getChildren()[0]->evaluate(root, variables);
    if (input.nullish()) {
        return Value(BSONNULL);
    }

    const auto& opName = expr.getOpName();
    uassert(9567000,
            str::stream() << opName << " supports an object as its argument",
            input.getType() == BSONType::object);

    // Retrieve the values from the incoming document.
    Value nsUnchecked = input["ns"_sd];
    Value shardVersionUnchecked = input["shardVersion"_sd];
    Value shardKeyValUnchecked = input["shardKeyVal"_sd];

    uassert(9567001,
            str::stream() << opName << " requires 'ns' argument to be an string",
            nsUnchecked.getType() == BSONType::string);
    NamespaceString ns(NamespaceStringUtil::deserialize(expCtx->getNamespaceString().tenantId(),
                                                        nsUnchecked.getStringData(),
                                                        expCtx->getSerializationContext()));

    uassert(9567002,
            str::stream() << opName << " requires 'shardVersion' argument to be an object",
            shardVersionUnchecked.getType() == BSONType::object);
    const auto shardVersionObj = shardVersionUnchecked.getDocument().toBson();
    const auto shardVersion = ShardVersion::parse(BSON("" << shardVersionObj).firstElement());

    uassert(9567003,
            str::stream() << opName << " requires 'shardKeyVal' argument to be an object",
            shardKeyValUnchecked.getType() == BSONType::object);
    const auto shardKeyVal = shardKeyValUnchecked.getDocument().toBson();

    // Get the 'chunkManager' from the catalog cache.
    auto opCtx = expCtx->getOperationContext();
    const auto catalogCache = Grid::get(opCtx)->catalogCache();
    uassert(6868602,
            "$_internalOwningShard expression only makes sense in sharded environment",
            catalogCache);

    // Setting 'allowLocks' to true when evaluating on mongod, as otherwise an invariant is thrown.
    // We can safely set it to true as there is no risk of deadlock, because the code still throws
    // when a refresh would actually need to take place.
    // We will always refresh the cached routing tables if the ShardVersion is older than the
    // version the caller believes it has, so it is okay of the cached routing tables are stale
    // here.
    const auto cri =
        uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, ns, true /* allowLocks */));

    // Advances the version in the cache if the chunk manager is not yet available or its version is
    // stale.
    if (!cri.hasRoutingTable() ||
        (cri.getCollectionVersion().placementVersion() <=> shardVersion.placementVersion()) ==
            std::partial_ordering::less) {
        catalogCache->onStaleCollectionVersion(ns, boost::none /* wanted */);

        uasserted(ShardCannotRefreshDueToLocksHeldInfo(ns),
                  str::stream()
                      << "Routing information for collection " << ns.toStringForErrorMsg()
                      << " is currently stale and needs to be refreshed from the config server");
    }

    // Retrieve the shard id for the given shard key value.
    std::set<ShardId> shardIds;
    cri.getChunkManager().getShardIdsForRange(shardKeyVal, shardKeyVal, &shardIds);
    uassert(6868601, "The value should belong to exactly one ShardId", shardIds.size() == 1);
    const auto shardId = *(shardIds.begin());
    return Value(shardId.toString());
}

namespace {

/**
 * The class IndexKeysObjectsGenerator is used to generate the index keys objects for the provided
 * document 'docObj' and the index descriptor 'indexDescriptor'. This class determines the index
 * type from the 'indexDescriptor', generates the corresponding key strings and then convert those
 * key strings to the index keys objects. An index keys object is of the following form:
 * {<field_name>: <index key>, ...}, which maps a field name to an index key.
 */
class IndexKeysObjectsGenerator {
public:
    IndexKeysObjectsGenerator(ExpressionContext* expCtx,
                              const BSONObj& docObj,
                              IndexDescriptor* indexDescriptor)
        : _docObj{docObj},
          _indexDescriptor{indexDescriptor},
          _fieldNames{indexDescriptor->getFieldNames()} {
        // Validate the key pattern to ensure that the field ordering is ascending.
        auto keyPattern = _indexDescriptor->keyPattern();
        for (auto& keyElem : keyPattern) {
            uassert(6868501,
                    str::stream() << "Index key pattern field ordering must be ascending. Field: "
                                  << keyElem.fieldNameStringData() << " is not ascending.",
                    !keyElem.isNumber() || keyElem.numberInt() == 1);
        }

        // Set the collator interface if the index descriptor has a collation.
        if (auto collation = indexDescriptor->collation(); !collation.isEmpty()) {
            auto collatorFactory =
                CollatorFactoryInterface::get(expCtx->getOperationContext()->getServiceContext());
            auto collatorFactoryResult = collatorFactory->makeFromBSON(collation);

            uassert(6868502,
                    str::stream() << "Malformed 'collation' document provided. Reason "
                                  << collatorFactoryResult.getStatus(),
                    collatorFactoryResult.isOK());
            _collatorInterface = std::move(collatorFactoryResult.getValue());
        }
    }

    /**
     * Returns the generated index keys objects for the provided index type. The returned value
     * 'Value' is an array of 'BSONObj' the contains the generated keys objects.
     */
    Value generateKeys() const {
        KeyStringSet keyStrings;

        // Generate the key strings based on the index type.
        switch (_indexDescriptor->getIndexType()) {
            case IndexType::INDEX_BTREE: {
                _generateBtreeIndexKeys(&keyStrings);
                break;
            }
            case IndexType::INDEX_HASHED: {
                _generateHashedIndexKeys(&keyStrings);
                break;
            }
            case IndexType::INDEX_2D: {
                _generate2DIndexKeys(&keyStrings);
                break;
            }
            case IndexType::INDEX_2DSPHERE:
            case IndexType::INDEX_2DSPHERE_BUCKET: {
                _generate2DSphereIndexKeys(&keyStrings);
                break;
            }
            case IndexType::INDEX_TEXT: {
                _generateTextIndexKeys(&keyStrings);
                break;
            }
            case IndexType::INDEX_WILDCARD: {
                _generateWildcardIndexKeys(&keyStrings);
                break;
            }
            default: {
                uasserted(6868503,
                          str::stream()
                              << "Unsupported index type: " << _indexDescriptor->getIndexType());
            }
        };

        return _generateReply(keyStrings);
    }

private:
    /**
     * Generates the key string for the 'btree' index type and adds them to the 'keyStrings'.
     */
    void _generateBtreeIndexKeys(KeyStringSet* keyStrings) const {
        std::vector<BSONElement> keysElements{_fieldNames.size(), BSONElement{}};
        auto bTreeKeyGenerator = std::make_unique<BtreeKeyGenerator>(
            _fieldNames, keysElements, _indexDescriptor->isSparse(), _keyStringVersion, _ordering);

        constexpr bool skipMultikey = false;
        const boost::optional<RecordId> recordId = boost::none;
        MultikeyPaths multikeyPaths;
        SharedBufferFragmentBuilder pooledBufferBuilder(BufBuilder::kDefaultInitSizeBytes);

        bTreeKeyGenerator->getKeys(pooledBufferBuilder,
                                   _docObj,
                                   skipMultikey,
                                   keyStrings,
                                   &multikeyPaths,
                                   _collatorInterface.get(),
                                   recordId);
    }

    /**
     * Generates the key strings for the 'hashed' index type and adds them to the 'keyStrings'.
     */
    void _generateHashedIndexKeys(KeyStringSet* keyStrings) const {
        int hashVersion;
        BSONObj keyPattern;
        ExpressionParams::parseHashParams(_indexDescriptor->infoObj(), &hashVersion, &keyPattern);

        constexpr auto ignoreArraysAlongPath = false;
        const boost::optional<RecordId> recordId = boost::none;
        SharedBufferFragmentBuilder pooledBufferBuilder(BufBuilder::kDefaultInitSizeBytes);

        ExpressionKeysPrivate::getHashKeys(pooledBufferBuilder,
                                           _docObj,
                                           keyPattern,
                                           hashVersion,
                                           _indexDescriptor->isSparse(),
                                           _collatorInterface.get(),
                                           keyStrings,
                                           _keyStringVersion,
                                           _ordering,
                                           ignoreArraysAlongPath,
                                           recordId);
    }

    /*
     * Generates the key strings for the '2d' index type and adds them to the 'keyStrings'.
     */
    void _generate2DIndexKeys(KeyStringSet* keyStrings) const {
        TwoDIndexingParams twoDIndexingParams;
        index2d::parse2dParams(_indexDescriptor->infoObj(), &twoDIndexingParams);

        const boost::optional<RecordId> recordId = boost::none;
        SharedBufferFragmentBuilder pooledBufferBuilder(BufBuilder::kDefaultInitSizeBytes);

        index2d::get2DKeys(pooledBufferBuilder,
                           _docObj,
                           twoDIndexingParams,
                           keyStrings,
                           _keyStringVersion,
                           _ordering,
                           recordId);
    }

    /*
     * Generates the key strings for the '2dsphere' and the '2dsphere_bucket' index types and adds
     * them to the 'keyStrings'.
     */
    void _generate2DSphereIndexKeys(KeyStringSet* keyStrings) const {
        S2IndexingParams s2IndexingParams;
        index2dsphere::initialize2dsphereParams(
            _indexDescriptor->infoObj(), _collatorInterface.get(), &s2IndexingParams);

        MultikeyPaths multikeyPaths;
        const boost::optional<RecordId> recordId = boost::none;
        SharedBufferFragmentBuilder pooledBufferBuilder(BufBuilder::kDefaultInitSizeBytes);

        index2dsphere::getS2Keys(pooledBufferBuilder,
                                 _docObj,
                                 _indexDescriptor->keyPattern(),
                                 s2IndexingParams,
                                 keyStrings,
                                 &multikeyPaths,
                                 _keyStringVersion,
                                 SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                                 _ordering,
                                 recordId);
    }

    /**
     * Generates the key strings for the 'text' index type and adds them to the 'keyStrings'.
     */
    void _generateTextIndexKeys(KeyStringSet* keyStrings) const {
        fts::FTSSpec ftsSpec{_indexDescriptor->infoObj()};
        const boost::optional<RecordId> recordId = boost::none;
        SharedBufferFragmentBuilder pooledBufferBuilder(BufBuilder::kDefaultInitSizeBytes);

        ExpressionKeysPrivate::getFTSKeys(pooledBufferBuilder,
                                          _docObj,
                                          ftsSpec,
                                          keyStrings,
                                          _keyStringVersion,
                                          _ordering,
                                          recordId);
    }

    /**
     * Generates the key string for the 'wildcard' index type and adds them to the 'keyStrings'.
     */
    void _generateWildcardIndexKeys(KeyStringSet* keyStrings) const {
        WildcardKeyGenerator wildcardKeyGenerator{_indexDescriptor->keyPattern(),
                                                  _indexDescriptor->pathProjection(),
                                                  _collatorInterface.get(),
                                                  _keyStringVersion,
                                                  _ordering};

        KeyStringSet* multikeyMetadataKeys = nullptr;
        const boost::optional<RecordId> recordId = boost::none;
        SharedBufferFragmentBuilder pooledBufferBuilder(BufBuilder::kDefaultInitSizeBytes);

        wildcardKeyGenerator.generateKeys(
            pooledBufferBuilder, _docObj, keyStrings, multikeyMetadataKeys, recordId);
    }

    /**
     * Returns a 'Value' that is a vector of 'BSONObj' that contains the index keys documents.
     */
    Value _generateReply(const KeyStringSet& keyStrings) const {
        // This helper accepts the key string 'keyString' and returns a 'BSONObj' that maps a field
        // names to its index key.
        auto buildObjectFromKeyString = [&](const auto& keyString) {
            auto keyStringObj = key_string::toBson(keyString, Ordering::make(BSONObj()));
            BSONObjBuilder keyObjectBuilder;

            switch (_indexDescriptor->getIndexType()) {
                //
                // A wild card key string is of the following format:
                // {'': <field name>, '': <key value>}.
                // The 'keyStringObj' itself contains the field name and the key. As such build a
                // new 'BSONObj' that directly maps a field name and its index key.
                //
                case IndexType::INDEX_WILDCARD: {
                    boost::optional<std::string> fieldName;
                    boost::optional<BSONElement> keyStringElem;

                    BSONObjIterator keyStringObjIter = BSONObjIterator(keyStringObj);
                    if (keyStringObjIter.more()) {
                        fieldName = keyStringObjIter.next().String();
                    }
                    if (keyStringObjIter.more()) {
                        keyStringElem = keyStringObjIter.next();
                    }

                    invariant(!keyStringObjIter.more());

                    if (fieldName && keyStringElem) {
                        keyObjectBuilder << *fieldName << *keyStringElem;
                    }
                    break;
                }

                //
                // For all other index types, each 'BSONElement' field of the 'keyStringObj' has a
                // one-to-one mapping with the elements of the '_fieldNames'. This one-to-one
                // property is utilized to builds a new 'BSONObj' that has keys from 'fieldNames'
                // and values from key string 'BSONElement'.
                //
                default: {
                    auto fieldNamesIter = _fieldNames.begin();
                    for (auto&& keyStringElem : keyStringObj) {
                        keyObjectBuilder << *fieldNamesIter << keyStringElem;
                        ++fieldNamesIter;
                    }

                    invariant(fieldNamesIter == _fieldNames.end());
                    break;
                }
            }

            return keyObjectBuilder.obj();
        };

        // Iterate through each key string and get a new 'BSONObj' that has field names and
        // corresponding keys embedded to it.
        std::vector<BSONObj> keysArrayBuilder;
        for (auto&& keyString : keyStrings) {
            auto keyStringObj = buildObjectFromKeyString(keyString);
            if (!keyStringObj.isEmpty()) {
                keysArrayBuilder.push_back(std::move(keyStringObj));
            }
        }

        return Value{keysArrayBuilder};
    }

    // The document for which the key strings should be generated.
    const BSONObj& _docObj;

    // The index descriptor to be used for generating the key strings.
    const IndexDescriptor* const _indexDescriptor;

    // Field names derived from the key pattern of the index descriptor.
    const std::vector<const char*> _fieldNames;

    // The collator interface initialized from the index descriptor.
    std::unique_ptr<CollatorInterface> _collatorInterface;

    // The key string version to be used for generating the key strings.
    const key_string::Version _keyStringVersion = key_string::Version::kLatestVersion;

    // The ordering to be used for generating the key strings.
    const Ordering _ordering = Ordering::allAscending();
};

}  // namespace

Value evaluate(const ExpressionInternalIndexKey& expr, const Document& root, Variables* variables) {
    uassert(6868510,
            str::stream() << expr.getOpName() << " is currently not supported on mongos",
            !serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer));

    auto docObj = expr.getDoc()->evaluate(root, variables).getDocument().toBson();
    auto specObj = expr.getSpec()->evaluate(root, variables).getDocument().toBson();

    // Parse and validate the index spec and then create the index descriptor object from it.
    auto indexSpec = index_key_validate::parseAndValidateIndexSpecs(
        expr.getExpressionContext()->getOperationContext(), specObj);
    BSONObj keyPattern = indexSpec.getObjectField(ExpressionInternalIndexKey::kIndexSpecKeyField);
    auto indexDescriptor =
        std::make_unique<IndexDescriptor>(IndexNames::findPluginName(keyPattern), indexSpec);

    IndexKeysObjectsGenerator indexKeysObjectsGenerator(
        expr.getExpressionContext(), docObj, indexDescriptor.get());

    return indexKeysObjectsGenerator.generateKeys();
}

}  // namespace exec::expression
}  // namespace mongo
