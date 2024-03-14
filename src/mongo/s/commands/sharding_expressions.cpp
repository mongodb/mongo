/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/s/commands/sharding_expressions.h"

#include <boost/container/flat_set.hpp>
#include <boost/container/vector.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/ordering.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index/2d_common.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/index/expression_keys_private.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/index_names.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_cannot_refresh_due_to_locks_held_exception.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/shared_buffer_fragment.h"
#include "mongo/util/str.h"

namespace mongo {
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
        : _expCtx{expCtx},
          _docObj{docObj},
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
                CollatorFactoryInterface::get(expCtx->opCtx->getServiceContext());
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
        ExpressionParams::parseTwoDParams(_indexDescriptor->infoObj(), &twoDIndexingParams);

        const boost::optional<RecordId> recordId = boost::none;
        SharedBufferFragmentBuilder pooledBufferBuilder(BufBuilder::kDefaultInitSizeBytes);

        ExpressionKeysPrivate::get2DKeys(pooledBufferBuilder,
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
        ExpressionParams::initialize2dsphereParams(
            _indexDescriptor->infoObj(), _collatorInterface.get(), &s2IndexingParams);

        MultikeyPaths multikeyPaths;
        const boost::optional<RecordId> recordId = boost::none;
        SharedBufferFragmentBuilder pooledBufferBuilder(BufBuilder::kDefaultInitSizeBytes);

        ExpressionKeysPrivate::getS2Keys(pooledBufferBuilder,
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

    // An existing expression context.
    const ExpressionContext* const _expCtx;

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

Value ExpressionInternalOwningShard::evaluate(const Document& root, Variables* variables) const {
    uassert(6868600,
            "$_internalOwningShard is currently not supported on mongos",
            !serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer));

    Value input = _children[0]->evaluate(root, variables);
    if (input.nullish()) {
        return Value(BSONNULL);
    }

    // Retrieve the values from the incoming document.
    auto expCtx = getExpressionContext();
    NamespaceString ns(NamespaceStringUtil::deserialize(
        expCtx->ns.tenantId(), input["ns"_sd].getStringData(), expCtx->serializationCtxt));
    const auto shardVersionObj = input["shardVersion"_sd].getDocument().toBson();
    const auto shardVersion = ShardVersion::parse(BSON("" << shardVersionObj).firstElement());
    const auto shardKeyVal = input["shardKeyVal"_sd].getDocument().toBson();

    // Get the 'chunkManager' from the catalog cache.
    auto opCtx = expCtx->opCtx;
    const auto catalogCache = Grid::get(opCtx)->catalogCache();
    uassert(6868602,
            "$_internalOwningShard expression only makes sense in sharded environment",
            catalogCache);

    // Setting 'allowLocks' to true when evaluating on mongod, as otherwise an invariant is thrown.
    // We can safely set it to true as there is no risk of deadlock, because the code still throws
    // when a refresh would actually need to take place.
    const auto cri =
        uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, ns, true /* allowLocks */));

    // Invalidate catalog cache if the chunk manager is not yet available or its version is stale.
    if (!cri.cm.hasRoutingTable() ||
        cri.cm.getVersion().isOlderThan(shardVersion.placementVersion())) {
        catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
            ns, boost::none /* wanted */, ShardId());

        uasserted(ShardCannotRefreshDueToLocksHeldInfo(ns),
                  str::stream()
                      << "Routing information for collection " << ns.toStringForErrorMsg()
                      << " is currently stale and needs to be refreshed from the config server");
    }

    // Retrieve the shard id for the given shard key value.
    std::set<ShardId> shardIds;
    cri.cm.getShardIdsForRange(shardKeyVal, shardKeyVal, &shardIds);
    uassert(6868601, "The value should belong to exactly one ShardId", shardIds.size() == 1);
    const auto shardId = *(shardIds.begin());
    return Value(shardId.toString());
}

boost::intrusive_ptr<Expression> ExpressionInternalIndexKey::parse(ExpressionContext* expCtx,
                                                                   BSONElement bsonExpr,
                                                                   const VariablesParseState& vps) {
    uassert(6868506,
            str::stream() << opName << " supports an object as its argument",
            bsonExpr.type() == BSONType::Object);

    BSONElement docElement;
    BSONElement specElement;

    for (auto&& bsonArgs : bsonExpr.embeddedObject()) {
        if (bsonArgs.fieldNameStringData() == kDocField) {
            docElement = bsonArgs;
        } else if (bsonArgs.fieldNameStringData() == kSpecField) {
            uassert(6868507,
                    str::stream() << opName << " requires 'spec' argument to be an object",
                    bsonArgs.type() == BSONType::Object);
            specElement = bsonArgs;
        } else {
            uasserted(6868508,
                      str::stream() << "Unknown argument: " << bsonArgs.fieldNameStringData()
                                    << "found while parsing" << opName);
        }
    }

    uassert(6868509,
            str::stream() << opName << " requires both 'doc' and 'spec' arguments",
            !docElement.eoo() && !specElement.eoo());

    return new ExpressionInternalIndexKey(expCtx,
                                          parseOperand(expCtx, docElement, vps),
                                          ExpressionConstant::create(expCtx, Value{specElement}));
}

ExpressionInternalIndexKey::ExpressionInternalIndexKey(ExpressionContext* expCtx,
                                                       boost::intrusive_ptr<Expression> doc,
                                                       boost::intrusive_ptr<Expression> spec)
    : Expression(expCtx, {std::move(doc), std::move(spec)}),
      _doc(_children[0]),
      _spec(_children[1]) {
    expCtx->sbeCompatibility = SbeCompatibility::notCompatible;
}

boost::intrusive_ptr<Expression> ExpressionInternalIndexKey::optimize() {
    invariant(_doc);
    invariant(_spec);

    _doc = _doc->optimize();
    _spec = _spec->optimize();
    return this;
}

Value ExpressionInternalIndexKey::serialize(const SerializationOptions& options) const {
    invariant(_doc);
    invariant(_spec);

    auto specExprConstant = dynamic_cast<ExpressionConstant*>(_spec.get());
    tassert(7250400, "Failed to dynamic cast the 'spec' to 'ExpressionConstant'", specExprConstant);

    // The 'spec' is always treated as a constant so do not call '_spec->serialize()' which would
    // wrap the value in an unnecessary '$const' object.
    return Value(DOC(opName << DOC(kDocField << _doc->serialize(options) << kSpecField
                                             << specExprConstant->getValue())));
}

Value ExpressionInternalIndexKey::evaluate(const Document& root, Variables* variables) const {
    uassert(6868510,
            str::stream() << opName << " is currently not supported on mongos",
            !serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer));

    auto docObj = _doc->evaluate(root, variables).getDocument().toBson();
    auto specObj = _spec->evaluate(root, variables).getDocument().toBson();

    // Parse and validate the index spec and then create the index descriptor object from it.
    auto indexSpec =
        index_key_validate::parseAndValidateIndexSpecs(getExpressionContext()->opCtx, specObj);
    BSONObj keyPattern = indexSpec.getObjectField(kIndexSpecKeyField);
    auto indexDescriptor =
        std::make_unique<IndexDescriptor>(IndexNames::findPluginName(keyPattern), indexSpec);

    IndexKeysObjectsGenerator indexKeysObjectsGenerator(
        getExpressionContext(), docObj, indexDescriptor.get());

    return indexKeysObjectsGenerator.generateKeys();
}

REGISTER_STABLE_EXPRESSION(_internalOwningShard, ExpressionInternalOwningShard::parse);
REGISTER_STABLE_EXPRESSION(_internalIndexKey, ExpressionInternalIndexKey::parse);

}  // namespace mongo
