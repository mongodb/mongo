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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_catalog_impl.h"

#include <vector>

#include "mongo/base/init.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/audit.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/disable_index_spec_namespace_generation_gen.h"
#include "mongo/db/catalog/index_catalog_entry_impl.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/curop.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_legacy.h"
#include "mongo/db/index_names.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/kv/kv_catalog.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/represent_as.h"
#include "mongo/util/str.h"

namespace mongo {

using std::endl;
using std::string;
using std::unique_ptr;
using std::vector;

using IndexVersion = IndexDescriptor::IndexVersion;

static const int INDEX_CATALOG_INIT = 283711;
static const int INDEX_CATALOG_UNINIT = 654321;

const BSONObj IndexCatalogImpl::_idObj = BSON("_id" << 1);

// -------------

IndexCatalogImpl::IndexCatalogImpl(Collection* collection, int maxNumIndexesAllowed)
    : _magic(INDEX_CATALOG_UNINIT),
      _collection(collection),
      _maxNumIndexesAllowed(maxNumIndexesAllowed) {}

IndexCatalogImpl::~IndexCatalogImpl() {
    if (_magic != INDEX_CATALOG_UNINIT) {
        // only do this check if we haven't been initialized
        _checkMagic();
    }
    _magic = 123456;
}

Status IndexCatalogImpl::init(OperationContext* opCtx) {
    vector<string> indexNames;
    _collection->getCatalogEntry()->getAllIndexes(opCtx, &indexNames);

    for (size_t i = 0; i < indexNames.size(); i++) {
        const string& indexName = indexNames[i];
        BSONObj spec = _collection->getCatalogEntry()->getIndexSpec(opCtx, indexName).getOwned();

        invariant(_collection->getCatalogEntry()->isIndexReady(opCtx, indexName));

        BSONObj keyPattern = spec.getObjectField("key");
        auto descriptor =
            std::make_unique<IndexDescriptor>(_collection, _getAccessMethodName(keyPattern), spec);
        const bool initFromDisk = true;
        const bool isReadyIndex = true;
        IndexCatalogEntry* entry =
            _setupInMemoryStructures(opCtx, std::move(descriptor), initFromDisk, isReadyIndex);

        fassert(17340, entry->isReady(opCtx));
    }

    _magic = INDEX_CATALOG_INIT;
    return Status::OK();
}

IndexCatalogEntry* IndexCatalogImpl::_setupInMemoryStructures(
    OperationContext* opCtx,
    std::unique_ptr<IndexDescriptor> descriptor,
    bool initFromDisk,
    bool isReadyIndex) {
    Status status = _isSpecOk(opCtx, descriptor->infoObj());
    if (!status.isOK()) {
        severe() << "Found an invalid index " << descriptor->infoObj() << " on the "
                 << _collection->ns() << " collection: " << redact(status);
        fassertFailedNoTrace(28782);
    }

    auto* const descriptorPtr = descriptor.get();
    auto entry = std::make_shared<IndexCatalogEntryImpl>(opCtx,
                                                         _collection->ns().ns(),
                                                         _collection->getCatalogEntry(),
                                                         std::move(descriptor),
                                                         _collection->infoCache());

    IndexDescriptor* desc = entry->descriptor();

    KVStorageEngine* engine =
        checked_cast<KVStorageEngine*>(opCtx->getServiceContext()->getStorageEngine());
    std::string ident =
        engine->getCatalog()->getIndexIdent(opCtx, _collection->ns(), desc->indexName());

    std::unique_ptr<SortedDataInterface> sdi =
        engine->getEngine()->getGroupedSortedDataInterface(opCtx, ident, desc, entry->getPrefix());

    std::unique_ptr<IndexAccessMethod> accessMethod =
        IndexAccessMethodFactory::get(opCtx)->make(entry.get(), std::move(sdi));

    entry->init(std::move(accessMethod));

    IndexCatalogEntry* save = entry.get();
    if (isReadyIndex) {
        _readyIndexes.add(std::move(entry));
    } else {
        _buildingIndexes.add(std::move(entry));
    }

    if (!initFromDisk) {
        opCtx->recoveryUnit()->onRollback(
            [ this, opCtx, isReadyIndex, descriptor = descriptorPtr ] {
                // Need to preserve indexName as descriptor no longer exists after remove().
                const std::string indexName = descriptor->indexName();
                if (isReadyIndex) {
                    _readyIndexes.remove(descriptor);
                } else {
                    _buildingIndexes.remove(descriptor);
                }
                _collection->infoCache()->droppedIndex(opCtx, indexName);
            });
    }

    return save;
}

bool IndexCatalogImpl::ok() const {
    return (_magic == INDEX_CATALOG_INIT);
}

void IndexCatalogImpl::_checkMagic() const {
    if (ok()) {
        return;
    }
    log() << "IndexCatalog::_magic wrong, is : " << _magic;
    fassertFailed(17198);
}

std::unique_ptr<IndexCatalog::IndexIterator> IndexCatalogImpl::getIndexIterator(
    OperationContext* const opCtx, const bool includeUnfinishedIndexes) const {
    if (!includeUnfinishedIndexes) {
        // If the caller only wants the ready indexes, we return an iterator over the catalog's
        // ready indexes vector. When the user advances this iterator, it will filter out any
        // indexes that were not ready at the OperationContext's read timestamp.
        return std::make_unique<ReadyIndexesIterator>(
            opCtx, _readyIndexes.begin(), _readyIndexes.end());
    }

    // If the caller wants all indexes, for simplicity of implementation, we copy the pointers to
    // a new vector. The vector's ownership is passed to the iterator. The query code path from an
    // external client is not expected to hit this case so the cost isn't paid by the important
    // code path.
    auto allIndexes = std::make_unique<std::vector<IndexCatalogEntry*>>();
    for (auto it = _readyIndexes.begin(); it != _readyIndexes.end(); ++it) {
        allIndexes->push_back(it->get());
    }

    for (auto it = _buildingIndexes.begin(); it != _buildingIndexes.end(); ++it) {
        allIndexes->push_back(it->get());
    }

    return std::make_unique<AllIndexesIterator>(opCtx, std::move(allIndexes));
}

string IndexCatalogImpl::_getAccessMethodName(const BSONObj& keyPattern) const {
    string pluginName = IndexNames::findPluginName(keyPattern);

    // This assert will be triggered when downgrading from a future version that
    // supports an index plugin unsupported by this version.
    uassert(17197,
            str::stream() << "Invalid index type '" << pluginName << "' "
                          << "in index "
                          << keyPattern,
            IndexNames::isKnownName(pluginName));

    return pluginName;
}


// ---------------------------

StatusWith<BSONObj> IndexCatalogImpl::_validateAndFixIndexSpec(OperationContext* opCtx,
                                                               const BSONObj& original) const {
    Status status = _isSpecOk(opCtx, original);
    if (!status.isOK()) {
        return status;
    }

    auto swFixed = _fixIndexSpec(opCtx, _collection, original);
    if (!swFixed.isOK()) {
        return swFixed;
    }

    // we double check with new index spec
    status = _isSpecOk(opCtx, swFixed.getValue());
    if (!status.isOK()) {
        return status;
    }

    return swFixed;
}

Status IndexCatalogImpl::_isNonIDIndexAndNotAllowedToBuild(OperationContext* opCtx,
                                                           const BSONObj& spec) const {
    const BSONObj key = spec.getObjectField("key");
    invariant(!key.isEmpty());
    if (!IndexDescriptor::isIdIndexPattern(key)) {
        // Check whether the replica set member's config has {buildIndexes:false} set, which means
        // we are not allowed to build non-_id indexes on this server.
        if (!repl::ReplicationCoordinator::get(opCtx)->buildsIndexes()) {
            // We return an IndexAlreadyExists error so that the caller can catch it and silently
            // skip building it.
            return Status(ErrorCodes::IndexAlreadyExists,
                          "this replica set member's 'buildIndexes' setting is set to false");
        }
    }

    return Status::OK();
}

void IndexCatalogImpl::_logInternalState(OperationContext* opCtx,
                                         long long numIndexesInCollectionCatalogEntry,
                                         const std::vector<std::string>& indexNamesToDrop,
                                         bool haveIdIndex) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_collection->ns(), MODE_X));

    error() << "Internal Index Catalog state: "
            << " numIndexesTotal(): " << numIndexesTotal(opCtx)
            << " numSystemIndexesEntries: " << numIndexesInCollectionCatalogEntry
            << " _readyIndexes.size(): " << _readyIndexes.size()
            << " _buildingIndexes.size(): " << _buildingIndexes.size()
            << " indexNamesToDrop: " << indexNamesToDrop.size() << " haveIdIndex: " << haveIdIndex;

    // Report the ready indexes.
    error() << "Ready indexes:";
    for (const auto& entry : _readyIndexes) {
        const IndexDescriptor* desc = entry->descriptor();
        error() << "Index '" << desc->indexName()
                << "' with specification: " << redact(desc->infoObj());
    }

    // Report the in-progress indexes.
    error() << "In-progress indexes:";
    for (const auto& entry : _buildingIndexes) {
        const IndexDescriptor* desc = entry->descriptor();
        error() << "Index '" << desc->indexName()
                << "' with specification: " << redact(desc->infoObj());
    }

    error() << "Internal Collection Catalog Entry state:";
    std::vector<std::string> allIndexes;
    std::vector<std::string> readyIndexes;

    _collection->getCatalogEntry()->getAllIndexes(opCtx, &allIndexes);
    _collection->getCatalogEntry()->getReadyIndexes(opCtx, &readyIndexes);

    error() << "All indexes:";
    for (const auto& index : allIndexes) {
        error() << "Index '" << index << "' with specification: "
                << redact(_collection->getCatalogEntry()->getIndexSpec(opCtx, index));
    }

    error() << "Ready indexes:";
    for (const auto& index : readyIndexes) {
        error() << "Index '" << index << "' with specification: "
                << redact(_collection->getCatalogEntry()->getIndexSpec(opCtx, index));
    }

    error() << "Index names to drop:";
    for (const auto& indexNameToDrop : indexNamesToDrop) {
        error() << "Index '" << indexNameToDrop << "' with specification: "
                << redact(_collection->getCatalogEntry()->getIndexSpec(opCtx, indexNameToDrop));
    }
}

StatusWith<BSONObj> IndexCatalogImpl::prepareSpecForCreate(OperationContext* opCtx,
                                                           const BSONObj& original) const {
    auto swValidatedAndFixed = _validateAndFixIndexSpec(opCtx, original);
    if (!swValidatedAndFixed.isOK()) {
        return swValidatedAndFixed.getStatus().withContext(
            str::stream() << "Error in specification " << original.toString());
    }

    // Check whether this is a non-_id index and there are any settings disallowing this server
    // from building non-_id indexes.
    Status status = _isNonIDIndexAndNotAllowedToBuild(opCtx, swValidatedAndFixed.getValue());
    if (!status.isOK()) {
        return status;
    }

    // First check against only the ready indexes for conflicts.
    status = _doesSpecConflictWithExisting(opCtx, swValidatedAndFixed.getValue(), false);
    if (!status.isOK()) {
        return status;
    }

    // Now we will check against all indexes, in-progress included.
    //
    // The index catalog cannot currently iterate over only in-progress indexes. So by previously
    // checking against only ready indexes without error, we know that any errors encountered
    // checking against all indexes occurred due to an in-progress index.
    status = _doesSpecConflictWithExisting(opCtx, swValidatedAndFixed.getValue(), true);
    if (!status.isOK()) {
        if (ErrorCodes::IndexAlreadyExists == status.code()) {
            // Callers need to be able to distinguish conflicts against ready indexes versus
            // in-progress indexes.
            return {ErrorCodes::IndexBuildAlreadyInProgress, status.reason()};
        }
        return status;
    }

    return swValidatedAndFixed.getValue();
}

std::vector<BSONObj> IndexCatalogImpl::removeExistingIndexesNoChecks(
    OperationContext* const opCtx, const std::vector<BSONObj>& indexSpecsToBuild) const {
    std::vector<BSONObj> result;
    // Filter out ready and in-progress index builds, and any non-_id indexes if 'buildIndexes' is
    // set to false in the replica set's config.
    for (const auto& spec : indexSpecsToBuild) {
        // returned to be built by the caller.
        if (ErrorCodes::OK != _isNonIDIndexAndNotAllowedToBuild(opCtx, spec)) {
            continue;
        }

        // _doesSpecConflictWithExisting currently does more work than we require here: we are only
        // interested in the index already exists error.
        if (ErrorCodes::IndexAlreadyExists ==
            _doesSpecConflictWithExisting(opCtx, spec, true /*includeUnfinishedIndexes*/)) {
            continue;
        }

        result.push_back(spec);
    }
    return result;
}

std::vector<BSONObj> IndexCatalogImpl::removeExistingIndexes(
    OperationContext* const opCtx,
    const std::vector<BSONObj>& indexSpecsToBuild,
    const bool removeIndexBuildsToo) const {
    std::vector<BSONObj> result;
    for (const auto& spec : indexSpecsToBuild) {
        auto prepareResult = prepareSpecForCreate(opCtx, spec);
        if (prepareResult == ErrorCodes::IndexAlreadyExists ||
            (removeIndexBuildsToo && prepareResult == ErrorCodes::IndexBuildAlreadyInProgress)) {
            continue;
        }
        uassertStatusOK(prepareResult);
        result.push_back(spec);
    }
    return result;
}

StatusWith<BSONObj> IndexCatalogImpl::createIndexOnEmptyCollection(OperationContext* opCtx,
                                                                   BSONObj spec) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_collection->ns(), MODE_X));
    invariant(_collection->numRecords(opCtx) == 0,
              str::stream() << "Collection must be empty. Collection: " << _collection->ns()
                            << " UUID: "
                            << _collection->uuid()
                            << " Count: "
                            << _collection->numRecords(opCtx));

    _checkMagic();

    StatusWith<BSONObj> statusWithSpec = prepareSpecForCreate(opCtx, spec);
    Status status = statusWithSpec.getStatus();
    if (!status.isOK())
        return status;
    spec = statusWithSpec.getValue();

    // now going to touch disk
    IndexBuildBlock indexBuildBlock(this, _collection->ns(), spec, IndexBuildMethod::kForeground);
    status = indexBuildBlock.init(opCtx, _collection);
    if (!status.isOK())
        return status;

    // sanity checks, etc...
    IndexCatalogEntry* entry = indexBuildBlock.getEntry();
    invariant(entry);
    IndexDescriptor* descriptor = entry->descriptor();
    invariant(descriptor);
    invariant(entry == _buildingIndexes.find(descriptor));

    status = entry->accessMethod()->initializeAsEmpty(opCtx);
    if (!status.isOK())
        return status;
    indexBuildBlock.success(opCtx, _collection);

    // sanity check
    invariant(_collection->getCatalogEntry()->isIndexReady(opCtx, descriptor->indexName()));

    return spec;
}

namespace {
// While technically recursive, only current possible with 2 levels.
Status _checkValidFilterExpressions(MatchExpression* expression, int level = 0) {
    if (!expression)
        return Status::OK();

    switch (expression->matchType()) {
        case MatchExpression::AND:
            if (level > 0)
                return Status(ErrorCodes::CannotCreateIndex,
                              "$and only supported in partialFilterExpression at top level");
            for (size_t i = 0; i < expression->numChildren(); i++) {
                Status status = _checkValidFilterExpressions(expression->getChild(i), level + 1);
                if (!status.isOK())
                    return status;
            }
            return Status::OK();
        case MatchExpression::EQ:
        case MatchExpression::LT:
        case MatchExpression::LTE:
        case MatchExpression::GT:
        case MatchExpression::GTE:
        case MatchExpression::EXISTS:
        case MatchExpression::TYPE_OPERATOR:
            return Status::OK();
        default:
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream() << "unsupported expression in partial index: "
                                        << expression->debugString());
    }
}
}  // namespace

Status IndexCatalogImpl::_isSpecOk(OperationContext* opCtx, const BSONObj& spec) const {
    const NamespaceString& nss = _collection->ns();

    BSONElement vElt = spec["v"];
    if (!vElt) {
        return {ErrorCodes::InternalError,
                str::stream()
                    << "An internal operation failed to specify the 'v' field, which is a required "
                       "property of an index specification: "
                    << spec};
    }

    if (!vElt.isNumber()) {
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream() << "non-numeric value for \"v\" field: " << vElt);
    }

    auto vEltAsInt = representAs<int>(vElt.number());
    if (!vEltAsInt) {
        return {ErrorCodes::CannotCreateIndex,
                str::stream() << "Index version must be representable as a 32-bit integer, but got "
                              << vElt.toString(false, false)};
    }

    auto indexVersion = static_cast<IndexVersion>(*vEltAsInt);

    if (indexVersion >= IndexVersion::kV2) {
        auto status = index_key_validate::validateIndexSpecFieldNames(spec);
        if (!status.isOK()) {
            return status;
        }
    }

    if (!IndexDescriptor::isIndexVersionSupported(indexVersion)) {
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream() << "this version of mongod cannot build new indexes "
                                    << "of version number "
                                    << static_cast<int>(indexVersion));
    }

    if (nss.isOplog())
        return Status(ErrorCodes::CannotCreateIndex, "cannot have an index on the oplog");

    // If we stop generating the 'ns' field for index specs during testing, then we shouldn't
    // validate that the 'ns' field is missing.
    if (!disableIndexSpecNamespaceGeneration.load()) {
        const BSONElement specNamespace = spec["ns"];
        if (specNamespace.type() != String)
            return Status(ErrorCodes::CannotCreateIndex,
                          "the index spec is missing a \"ns\" string field");

        if (nss.ns() != specNamespace.valueStringData())
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream() << "the \"ns\" field of the index spec '"
                                        << specNamespace.valueStringData()
                                        << "' does not match the collection name '"
                                        << nss
                                        << "'");
    }

    // logical name of the index
    const BSONElement nameElem = spec["name"];
    if (nameElem.type() != String)
        return Status(ErrorCodes::CannotCreateIndex, "index name must be specified as a string");

    const StringData name = nameElem.valueStringData();
    if (name.find('\0') != std::string::npos)
        return Status(ErrorCodes::CannotCreateIndex, "index name cannot contain NUL bytes");

    if (name.empty())
        return Status(ErrorCodes::CannotCreateIndex, "index name cannot be empty");

    // Drop pending collections are internal to the server and will not be exported to another
    // storage engine. The indexes contained in these collections are not subject to the same
    // namespace length constraints as the ones in created by users.
    //
    // Index names do not limit the maximum allowable length of the target namespace under FCV 4.2
    // and above.
    const auto checkIndexNamespace =
        serverGlobalParams.featureCompatibility.isVersionInitialized() &&
        serverGlobalParams.featureCompatibility.getVersion() !=
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42;
    if (checkIndexNamespace && !nss.isDropPendingNamespace()) {
        auto indexNamespace = nss.makeIndexNamespace(name);
        if (indexNamespace.size() > NamespaceString::MaxNsLen)
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream() << "namespace name generated from index name \""
                                        << indexNamespace
                                        << "\" is too long (127 byte max)");
    }

    const BSONObj key = spec.getObjectField("key");
    const Status keyStatus = index_key_validate::validateKeyPattern(key, indexVersion);
    if (!keyStatus.isOK()) {
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream() << "bad index key pattern " << key << ": "
                                    << keyStatus.reason());
    }

    const string pluginName = IndexNames::findPluginName(key);
    std::unique_ptr<CollatorInterface> collator;
    BSONElement collationElement = spec.getField("collation");
    if (collationElement) {
        if (collationElement.type() != BSONType::Object) {
            return Status(ErrorCodes::CannotCreateIndex,
                          "\"collation\" for an index must be a document");
        }
        auto statusWithCollator = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                      ->makeFromBSON(collationElement.Obj());
        if (!statusWithCollator.isOK()) {
            return statusWithCollator.getStatus();
        }
        collator = std::move(statusWithCollator.getValue());

        if (!collator) {
            return {ErrorCodes::InternalError,
                    str::stream() << "An internal operation specified the collation "
                                  << CollationSpec::kSimpleSpec
                                  << " explicitly, which should instead be implied by omitting the "
                                     "'collation' field from the index specification"};
        }

        if (static_cast<IndexVersion>(vElt.numberInt()) < IndexVersion::kV2) {
            return {ErrorCodes::CannotCreateIndex,
                    str::stream() << "Index version " << vElt.fieldNameStringData() << "="
                                  << vElt.numberInt()
                                  << " does not support the '"
                                  << collationElement.fieldNameStringData()
                                  << "' option"};
        }

        if ((pluginName != IndexNames::BTREE) && (pluginName != IndexNames::GEO_2DSPHERE) &&
            (pluginName != IndexNames::HASHED) && (pluginName != IndexNames::WILDCARD)) {
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream() << "Index type '" << pluginName
                                        << "' does not support collation: "
                                        << collator->getSpec().toBSON());
        }
    }

    const bool isSparse = spec["sparse"].trueValue();

    if (pluginName == IndexNames::WILDCARD) {
        if (isSparse) {
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream() << "Index type '" << pluginName
                                        << "' does not support the sparse option");
        }

        if (spec["unique"].trueValue()) {
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream() << "Index type '" << pluginName
                                        << "' does not support the unique option");
        }

        if (spec.getField("expireAfterSeconds")) {
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream() << "Index type '" << pluginName
                                        << "' cannot be a TTL index");
        }
    }

    // Ensure if there is a filter, its valid.
    BSONElement filterElement = spec.getField("partialFilterExpression");
    if (filterElement) {
        if (isSparse) {
            return Status(ErrorCodes::CannotCreateIndex,
                          "cannot mix \"partialFilterExpression\" and \"sparse\" options");
        }

        if (filterElement.type() != Object) {
            return Status(ErrorCodes::CannotCreateIndex,
                          "\"partialFilterExpression\" for an index must be a document");
        }

        // The collator must outlive the constructed MatchExpression.
        boost::intrusive_ptr<ExpressionContext> expCtx(
            new ExpressionContext(opCtx, collator.get()));

        // Parsing the partial filter expression is not expected to fail here since the
        // expression would have been successfully parsed upstream during index creation.
        StatusWithMatchExpression statusWithMatcher =
            MatchExpressionParser::parse(filterElement.Obj(),
                                         std::move(expCtx),
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kBanAllSpecialFeatures);
        if (!statusWithMatcher.isOK()) {
            return statusWithMatcher.getStatus();
        }
        const std::unique_ptr<MatchExpression> filterExpr = std::move(statusWithMatcher.getValue());

        Status status = _checkValidFilterExpressions(filterExpr.get());
        if (!status.isOK()) {
            return status;
        }
    }

    if (IndexDescriptor::isIdIndexPattern(key)) {
        BSONElement uniqueElt = spec["unique"];
        if (uniqueElt && !uniqueElt.trueValue()) {
            return Status(ErrorCodes::CannotCreateIndex, "_id index cannot be non-unique");
        }

        if (filterElement) {
            return Status(ErrorCodes::CannotCreateIndex, "_id index cannot be a partial index");
        }

        if (isSparse) {
            return Status(ErrorCodes::CannotCreateIndex, "_id index cannot be sparse");
        }

        if (collationElement &&
            !CollatorInterface::collatorsMatch(collator.get(), _collection->getDefaultCollator())) {
            return Status(ErrorCodes::CannotCreateIndex,
                          "_id index must have the collection default collation");
        }
    }

    // --- only storage engine checks allowed below this ----

    BSONElement storageEngineElement = spec.getField("storageEngine");
    if (storageEngineElement.eoo()) {
        return Status::OK();
    }
    if (storageEngineElement.type() != mongo::Object) {
        return Status(ErrorCodes::CannotCreateIndex,
                      "\"storageEngine\" options must be a document if present");
    }
    BSONObj storageEngineOptions = storageEngineElement.Obj();
    if (storageEngineOptions.isEmpty()) {
        return Status(ErrorCodes::CannotCreateIndex,
                      "Empty \"storageEngine\" options are invalid. "
                      "Please remove the field or include valid options.");
    }
    Status storageEngineStatus = validateStorageOptions(
        opCtx->getServiceContext(), storageEngineOptions, [](const auto& x, const auto& y) {
            return x->validateIndexStorageOptions(y);
        });
    if (!storageEngineStatus.isOK()) {
        return storageEngineStatus;
    }

    return Status::OK();
}

Status IndexCatalogImpl::_doesSpecConflictWithExisting(OperationContext* opCtx,
                                                       const BSONObj& spec,
                                                       const bool includeUnfinishedIndexes) const {
    const char* name = spec.getStringField("name");
    invariant(name[0]);

    const BSONObj key = spec.getObjectField("key");
    const BSONObj collation = spec.getObjectField("collation");

    {
        const IndexDescriptor* desc = findIndexByName(opCtx, name, includeUnfinishedIndexes);
        if (desc) {
            // index already exists with same name

            if (SimpleBSONObjComparator::kInstance.evaluate(desc->keyPattern() == key) &&
                SimpleBSONObjComparator::kInstance.evaluate(
                    desc->infoObj().getObjectField("collation") != collation)) {
                // key patterns are equal but collations differ.
                return Status(ErrorCodes::IndexOptionsConflict,
                              str::stream()
                                  << "An index with the same key pattern, but a different "
                                  << "collation already exists with the same name.  Try again with "
                                  << "a unique name. "
                                  << "Existing index: "
                                  << desc->infoObj()
                                  << " Requested index: "
                                  << spec);
            }

            if (SimpleBSONObjComparator::kInstance.evaluate(desc->keyPattern() != key) ||
                SimpleBSONObjComparator::kInstance.evaluate(
                    desc->infoObj().getObjectField("collation") != collation)) {
                return Status(ErrorCodes::IndexKeySpecsConflict,
                              str::stream() << "Index must have unique name."
                                            << "The existing index: "
                                            << desc->infoObj()
                                            << " has the same name as the requested index: "
                                            << spec);
            }

            IndexDescriptor temp(_collection, _getAccessMethodName(key), spec);
            if (!desc->areIndexOptionsEquivalent(&temp))
                return Status(ErrorCodes::IndexOptionsConflict,
                              str::stream() << "Index with name: " << name
                                            << " already exists with different options");

            // Index already exists with the same options, so no need to build a new
            // one (not an error). Most likely requested by a client using ensureIndex.
            return Status(ErrorCodes::IndexAlreadyExists,
                          str::stream() << "Identical index already exists: " << name);
        }
    }

    {
        const IndexDescriptor* desc =
            findIndexByKeyPatternAndCollationSpec(opCtx, key, collation, includeUnfinishedIndexes);
        if (desc) {
            LOG(2) << "Index already exists with a different name: " << name << " pattern: " << key
                   << " collation: " << collation;

            IndexDescriptor temp(_collection, _getAccessMethodName(key), spec);
            if (!desc->areIndexOptionsEquivalent(&temp))
                return Status(ErrorCodes::IndexOptionsConflict,
                              str::stream() << "Index: " << spec
                                            << " already exists with different options: "
                                            << desc->infoObj());

            return Status(ErrorCodes::IndexOptionsConflict,
                          str::stream() << "Index with name: " << name
                                        << " already exists with a different name");
        }
    }

    if (numIndexesTotal(opCtx) >= _maxNumIndexesAllowed) {
        string s = str::stream() << "add index fails, too many indexes for " << _collection->ns()
                                 << " key:" << key;
        log() << s;
        return Status(ErrorCodes::CannotCreateIndex, s);
    }

    // Refuse to build text index if another text index exists or is in progress.
    // Collections should only have one text index.
    string pluginName = IndexNames::findPluginName(key);
    if (pluginName == IndexNames::TEXT) {
        vector<const IndexDescriptor*> textIndexes;
        findIndexByType(opCtx, IndexNames::TEXT, textIndexes, includeUnfinishedIndexes);
        if (textIndexes.size() > 0) {
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream() << "only one text index per collection allowed, "
                                        << "found existing text index \""
                                        << textIndexes[0]->indexName()
                                        << "\"");
        }
    }
    return Status::OK();
}

BSONObj IndexCatalogImpl::getDefaultIdIndexSpec() const {
    dassert(_idObj["_id"].type() == NumberInt);

    const auto indexVersion = IndexDescriptor::getDefaultIndexVersion();

    BSONObjBuilder b;
    b.append("v", static_cast<int>(indexVersion));
    b.append("name", "_id_");
    b.append("ns", _collection->ns().ns());
    b.append("key", _idObj);
    if (_collection->getDefaultCollator() && indexVersion >= IndexVersion::kV2) {
        // Creating an index with the "collation" option requires a v=2 index.
        b.append("collation", _collection->getDefaultCollator()->getSpec().toBSON());
    }
    return b.obj();
}

void IndexCatalogImpl::dropAllIndexes(OperationContext* opCtx,
                                      bool includingIdIndex,
                                      std::function<void(const IndexDescriptor*)> onDropFn) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_collection->ns(), MODE_X));

    uassert(ErrorCodes::BackgroundOperationInProgressForNamespace,
            str::stream() << "cannot perform operation: an index build is currently running",
            !haveAnyIndexesInProgress());

    // make sure nothing in progress
    massert(17348,
            "cannot dropAllIndexes when index builds in progress",
            numIndexesTotal(opCtx) == numIndexesReady(opCtx));

    bool haveIdIndex = false;

    invariant(_buildingIndexes.size() == 0);
    vector<string> indexNamesToDrop;
    {
        int seen = 0;
        std::unique_ptr<IndexIterator> ii = getIndexIterator(opCtx, true);
        while (ii->more()) {
            seen++;
            const IndexDescriptor* desc = ii->next()->descriptor();
            if (desc->isIdIndex() && includingIdIndex == false) {
                haveIdIndex = true;
                continue;
            }
            indexNamesToDrop.push_back(desc->indexName());
        }
        invariant(seen == numIndexesTotal(opCtx));
    }

    for (size_t i = 0; i < indexNamesToDrop.size(); i++) {
        string indexName = indexNamesToDrop[i];
        const IndexDescriptor* desc = findIndexByName(opCtx, indexName, true);
        invariant(desc);
        LOG(1) << "\t dropAllIndexes dropping: " << desc->toString();
        IndexCatalogEntry* entry = _readyIndexes.find(desc);
        invariant(entry);

        // If the onDrop function creates an oplog entry, it should run first so that the drop is
        // timestamped at the same optime.
        if (onDropFn) {
            onDropFn(desc);
        }
        invariant(_dropIndex(opCtx, entry).isOK());
    }

    // verify state is sane post cleaning

    long long numIndexesInCollectionCatalogEntry =
        _collection->getCatalogEntry()->getTotalIndexCount(opCtx);

    if (haveIdIndex) {
        fassert(17324, numIndexesTotal(opCtx) == 1);
        fassert(17325, numIndexesReady(opCtx) == 1);
        fassert(17326, numIndexesInCollectionCatalogEntry == 1);
        fassert(17336, _readyIndexes.size() == 1);
    } else {
        if (numIndexesTotal(opCtx) || numIndexesInCollectionCatalogEntry || _readyIndexes.size()) {
            _logInternalState(
                opCtx, numIndexesInCollectionCatalogEntry, indexNamesToDrop, haveIdIndex);
        }
        fassert(17327, numIndexesTotal(opCtx) == 0);
        fassert(17328, numIndexesInCollectionCatalogEntry == 0);
        fassert(17337, _readyIndexes.size() == 0);
    }
}

void IndexCatalogImpl::dropAllIndexes(OperationContext* opCtx, bool includingIdIndex) {
    dropAllIndexes(opCtx, includingIdIndex, {});
}

Status IndexCatalogImpl::dropIndex(OperationContext* opCtx, const IndexDescriptor* desc) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_collection->ns(), MODE_X));
    invariant(!haveAnyIndexesInProgress());

    IndexCatalogEntry* entry = _readyIndexes.find(desc);

    if (!entry)
        return Status(ErrorCodes::InternalError, "cannot find index to delete");

    if (!entry->isReady(opCtx))
        return Status(ErrorCodes::InternalError, "cannot delete not ready index");

    return _dropIndex(opCtx, entry);
}

namespace {
class IndexRemoveChange final : public RecoveryUnit::Change {
public:
    IndexRemoveChange(OperationContext* opCtx,
                      Collection* collection,
                      IndexCatalogEntryContainer* entries,
                      std::shared_ptr<IndexCatalogEntry> entry)
        : _opCtx(opCtx), _collection(collection), _entries(entries), _entry(std::move(entry)) {}

    void commit(boost::optional<Timestamp> commitTime) final {
        // Ban reading from this collection on committed reads on snapshots before now.
        if (!commitTime) {
            // This is called when we refresh the index catalog entry, which does not always have
            // a commit timestamp. We use the cluster time since it's guaranteed to be greater
            // than the time of the index removal. It is possible the cluster time could be in the
            // future, and we will need to do another write to reach the minimum visible snapshot.
            commitTime = LogicalClock::getClusterTimeForReplicaSet(_opCtx).asTimestamp();
        }
        _collection->setMinimumVisibleSnapshot(commitTime.get());
    }

    void rollback() final {
        auto indexDescriptor = _entry->descriptor();
        _entries->add(std::move(_entry));

        // Refresh the CollectionInfoCache's knowledge of what indices are present. This must be
        // done after re-adding our IndexCatalogEntry to the '_entries' list, since 'addedIndex()'
        // refreshes its knowledge by iterating the list of indices currently in the catalog.
        _collection->infoCache()->addedIndex(_opCtx, indexDescriptor);
    }

private:
    OperationContext* _opCtx;
    Collection* _collection;
    IndexCatalogEntryContainer* _entries;
    std::shared_ptr<IndexCatalogEntry> _entry;
};
}  // namespace

Status IndexCatalogImpl::_dropIndex(OperationContext* opCtx, IndexCatalogEntry* entry) {
    // ----- SANITY CHECKS -------------
    if (!entry)
        return Status(ErrorCodes::BadValue, "IndexCatalog::_dropIndex passed NULL");

    _checkMagic();

    // Pulling indexName/indexNamespace out as they are needed post descriptor release.
    string indexName = entry->descriptor()->indexName();
    string indexNamespace = entry->descriptor()->indexNamespace();

    // --------- START REAL WORK ----------
    audit::logDropIndex(&cc(), indexName, _collection->ns().ns());

    auto released = _readyIndexes.release(entry->descriptor());
    if (released) {
        invariant(released.get() == entry);
        opCtx->recoveryUnit()->registerChange(
            new IndexRemoveChange(opCtx, _collection, &_readyIndexes, std::move(released)));
    } else {
        released = _buildingIndexes.release(entry->descriptor());
        invariant(released.get() == entry);
        opCtx->recoveryUnit()->registerChange(
            new IndexRemoveChange(opCtx, _collection, &_buildingIndexes, std::move(released)));
    }

    _collection->infoCache()->droppedIndex(opCtx, indexName);
    entry = nullptr;
    _deleteIndexFromDisk(opCtx, indexName, indexNamespace);

    _checkMagic();

    return Status::OK();
}

void IndexCatalogImpl::_deleteIndexFromDisk(OperationContext* opCtx,
                                            const string& indexName,
                                            const string& indexNamespace) {
    Status status = _collection->getCatalogEntry()->removeIndex(opCtx, indexName);
    if (status.code() == ErrorCodes::NamespaceNotFound) {
        // this is ok, as we may be partially through index creation
    } else if (!status.isOK()) {
        warning() << "couldn't drop index " << indexName << " on collection: " << _collection->ns()
                  << " because of " << redact(status);
    }
}

bool IndexCatalogImpl::isMultikey(OperationContext* opCtx, const IndexDescriptor* idx) {
    IndexCatalogEntry* entry = _readyIndexes.find(idx);
    invariant(entry);
    return entry->isMultikey(opCtx);
}

MultikeyPaths IndexCatalogImpl::getMultikeyPaths(OperationContext* opCtx,
                                                 const IndexDescriptor* idx) {
    IndexCatalogEntry* entry = _readyIndexes.find(idx);
    invariant(entry);
    return entry->getMultikeyPaths(opCtx);
}

void IndexCatalogImpl::setMultikeyPaths(OperationContext* const opCtx,
                                        const IndexDescriptor* desc,
                                        const MultikeyPaths& multikeyPaths) {
    IndexCatalogEntry* entry = _readyIndexes.find(desc);
    if (!entry) {
        entry = _buildingIndexes.find(desc);
    }
    invariant(entry);
    entry->setMultikey(opCtx, multikeyPaths);
};

// ---------------------------

bool IndexCatalogImpl::haveAnyIndexes() const {
    return _readyIndexes.size() > 0 || _buildingIndexes.size() > 0;
}

bool IndexCatalogImpl::haveAnyIndexesInProgress() const {
    return _buildingIndexes.size() > 0;
}

int IndexCatalogImpl::numIndexesTotal(OperationContext* opCtx) const {
    int count = _readyIndexes.size() + _buildingIndexes.size();
    dassert(_collection->getCatalogEntry()->getTotalIndexCount(opCtx) == count);
    return count;
}

int IndexCatalogImpl::numIndexesReady(OperationContext* opCtx) const {
    std::vector<const IndexDescriptor*> itIndexes;
    std::unique_ptr<IndexIterator> ii = getIndexIterator(opCtx, /*includeUnfinished*/ false);
    while (ii->more()) {
        itIndexes.push_back(ii->next()->descriptor());
    }
    DEV {
        std::vector<std::string> completedIndexes;
        _collection->getCatalogEntry()->getReadyIndexes(opCtx, &completedIndexes);

        // There is a potential inconistency where the index information in the collection catalog
        // entry and the index catalog differ. Log as much information as possible here.
        if (itIndexes.size() != completedIndexes.size()) {
            log() << "index catalog reports: ";
            for (const IndexDescriptor* i : itIndexes) {
                log() << "  index: " << i->toString();
            }

            log() << "collection catalog reports: ";
            for (auto const& i : completedIndexes) {
                log() << "  index: " << i;
            }

            if (_collection->uuid()) {
                log() << "collection uuid: " << _collection->uuid();
            }

            invariant(itIndexes.size() == completedIndexes.size(),
                      "The number of ready indexes reported in the collection metadata catalog did "
                      "not match the number of ready indexes reported by the index catalog.");
        }
    }
    return itIndexes.size();
}

bool IndexCatalogImpl::haveIdIndex(OperationContext* opCtx) const {
    return findIdIndex(opCtx) != nullptr;
}

const IndexDescriptor* IndexCatalogImpl::findIdIndex(OperationContext* opCtx) const {
    std::unique_ptr<IndexIterator> ii = getIndexIterator(opCtx, false);
    while (ii->more()) {
        const IndexDescriptor* desc = ii->next()->descriptor();
        if (desc->isIdIndex())
            return desc;
    }
    return nullptr;
}

const IndexDescriptor* IndexCatalogImpl::findIndexByName(OperationContext* opCtx,
                                                         StringData name,
                                                         bool includeUnfinishedIndexes) const {
    std::unique_ptr<IndexIterator> ii = getIndexIterator(opCtx, includeUnfinishedIndexes);
    while (ii->more()) {
        const IndexDescriptor* desc = ii->next()->descriptor();
        if (desc->indexName() == name)
            return desc;
    }
    return nullptr;
}

const IndexDescriptor* IndexCatalogImpl::findIndexByKeyPatternAndCollationSpec(
    OperationContext* opCtx,
    const BSONObj& key,
    const BSONObj& collationSpec,
    bool includeUnfinishedIndexes) const {
    std::unique_ptr<IndexIterator> ii = getIndexIterator(opCtx, includeUnfinishedIndexes);
    while (ii->more()) {
        const IndexDescriptor* desc = ii->next()->descriptor();
        if (SimpleBSONObjComparator::kInstance.evaluate(desc->keyPattern() == key) &&
            SimpleBSONObjComparator::kInstance.evaluate(
                desc->infoObj().getObjectField("collation") == collationSpec)) {
            return desc;
        }
    }
    return nullptr;
}

void IndexCatalogImpl::findIndexesByKeyPattern(OperationContext* opCtx,
                                               const BSONObj& key,
                                               bool includeUnfinishedIndexes,
                                               std::vector<const IndexDescriptor*>* matches) const {
    invariant(matches);
    std::unique_ptr<IndexIterator> ii = getIndexIterator(opCtx, includeUnfinishedIndexes);
    while (ii->more()) {
        const IndexDescriptor* desc = ii->next()->descriptor();
        if (SimpleBSONObjComparator::kInstance.evaluate(desc->keyPattern() == key)) {
            matches->push_back(desc);
        }
    }
}

const IndexDescriptor* IndexCatalogImpl::findShardKeyPrefixedIndex(OperationContext* opCtx,
                                                                   const BSONObj& shardKey,
                                                                   bool requireSingleKey) const {
    const IndexDescriptor* best = nullptr;

    std::unique_ptr<IndexIterator> ii = getIndexIterator(opCtx, false);
    while (ii->more()) {
        const IndexDescriptor* desc = ii->next()->descriptor();
        bool hasSimpleCollation = desc->infoObj().getObjectField("collation").isEmpty();

        if (desc->isPartial())
            continue;

        if (!shardKey.isPrefixOf(desc->keyPattern(), SimpleBSONElementComparator::kInstance))
            continue;

        if (!desc->isMultikey(opCtx) && hasSimpleCollation)
            return desc;

        if (!requireSingleKey && hasSimpleCollation)
            best = desc;
    }

    return best;
}

void IndexCatalogImpl::findIndexByType(OperationContext* opCtx,
                                       const string& type,
                                       vector<const IndexDescriptor*>& matches,
                                       bool includeUnfinishedIndexes) const {
    std::unique_ptr<IndexIterator> ii = getIndexIterator(opCtx, includeUnfinishedIndexes);
    while (ii->more()) {
        const IndexDescriptor* desc = ii->next()->descriptor();
        if (IndexNames::findPluginName(desc->keyPattern()) == type) {
            matches.push_back(desc);
        }
    }
}

const IndexCatalogEntry* IndexCatalogImpl::getEntry(const IndexDescriptor* desc) const {
    const IndexCatalogEntry* entry = _readyIndexes.find(desc);
    if (!entry) {
        entry = _buildingIndexes.find(desc);
    }

    massert(17357, "cannot find index entry", entry);
    return entry;
}

std::shared_ptr<const IndexCatalogEntry> IndexCatalogImpl::getEntryShared(
    const IndexDescriptor* indexDescriptor) const {
    auto entry = _readyIndexes.findShared(indexDescriptor);
    if (entry) {
        return entry;
    }
    return _buildingIndexes.findShared(indexDescriptor);
}

std::vector<std::shared_ptr<const IndexCatalogEntry>> IndexCatalogImpl::getAllReadyEntriesShared()
    const {
    return _readyIndexes.getAllEntries();
}

const IndexDescriptor* IndexCatalogImpl::refreshEntry(OperationContext* opCtx,
                                                      const IndexDescriptor* oldDesc) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_collection->ns(), MODE_X));
    invariant(_buildingIndexes.size() == 0);

    const std::string indexName = oldDesc->indexName();
    invariant(_collection->getCatalogEntry()->isIndexReady(opCtx, indexName));

    // Delete the IndexCatalogEntry that owns this descriptor.  After deletion, 'oldDesc' is
    // invalid and should not be dereferenced.
    auto oldEntry = _readyIndexes.release(oldDesc);
    invariant(oldEntry);
    opCtx->recoveryUnit()->registerChange(
        new IndexRemoveChange(opCtx, _collection, &_readyIndexes, std::move(oldEntry)));

    // Ask the CollectionCatalogEntry for the new index spec.
    BSONObj spec = _collection->getCatalogEntry()->getIndexSpec(opCtx, indexName).getOwned();
    BSONObj keyPattern = spec.getObjectField("key");

    // Re-register this index in the index catalog with the new spec.
    auto newDesc =
        std::make_unique<IndexDescriptor>(_collection, _getAccessMethodName(keyPattern), spec);
    const bool initFromDisk = false;
    const bool isReadyIndex = true;
    const IndexCatalogEntry* newEntry =
        _setupInMemoryStructures(opCtx, std::move(newDesc), initFromDisk, isReadyIndex);
    invariant(newEntry->isReady(opCtx));

    // Return the new descriptor.
    return newEntry->descriptor();
}

// ---------------------------

Status IndexCatalogImpl::_indexFilteredRecords(OperationContext* opCtx,
                                               IndexCatalogEntry* index,
                                               const std::vector<BsonRecord>& bsonRecords,
                                               int64_t* keysInsertedOut) {
    InsertDeleteOptions options;
    prepareInsertDeleteOptions(opCtx, index->descriptor(), &options);

    for (auto bsonRecord : bsonRecords) {
        invariant(bsonRecord.id != RecordId());

        if (!bsonRecord.ts.isNull()) {
            Status status = opCtx->recoveryUnit()->setTimestamp(bsonRecord.ts);
            if (!status.isOK())
                return status;
        }

        Status status = Status::OK();
        if (index->isHybridBuilding()) {
            int64_t inserted;
            status = index->indexBuildInterceptor()->sideWrite(opCtx,
                                                               index->accessMethod(),
                                                               bsonRecord.docPtr,
                                                               options,
                                                               bsonRecord.id,
                                                               IndexBuildInterceptor::Op::kInsert,
                                                               &inserted);
            if (keysInsertedOut) {
                *keysInsertedOut += inserted;
            }
        } else {
            InsertResult result;
            status = index->accessMethod()->insert(
                opCtx, *bsonRecord.docPtr, bsonRecord.id, options, &result);
            if (keysInsertedOut) {
                *keysInsertedOut += result.numInserted;
            }
        }

        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

Status IndexCatalogImpl::_indexRecords(OperationContext* opCtx,
                                       IndexCatalogEntry* index,
                                       const std::vector<BsonRecord>& bsonRecords,
                                       int64_t* keysInsertedOut) {
    const MatchExpression* filter = index->getFilterExpression();
    if (!filter)
        return _indexFilteredRecords(opCtx, index, bsonRecords, keysInsertedOut);

    std::vector<BsonRecord> filteredBsonRecords;
    for (auto bsonRecord : bsonRecords) {
        if (filter->matchesBSON(*(bsonRecord.docPtr)))
            filteredBsonRecords.push_back(bsonRecord);
    }

    return _indexFilteredRecords(opCtx, index, filteredBsonRecords, keysInsertedOut);
}

Status IndexCatalogImpl::_unindexRecord(OperationContext* opCtx,
                                        IndexCatalogEntry* index,
                                        const BSONObj& obj,
                                        const RecordId& loc,
                                        bool logIfError,
                                        int64_t* keysDeletedOut) {
    InsertDeleteOptions options;
    prepareInsertDeleteOptions(opCtx, index->descriptor(), &options);
    options.logIfError = logIfError;

    if (index->isHybridBuilding()) {
        // The side table interface accepts only records that meet the criteria for this partial
        // index.
        // For non-hybrid builds, the decision to use the filter for the partial index is left to
        // the IndexAccessMethod. See SERVER-28975 for details.
        if (auto filter = index->getFilterExpression()) {
            if (!filter->matchesBSON(obj)) {
                return Status::OK();
            }
        }

        int64_t removed;
        auto status = index->indexBuildInterceptor()->sideWrite(opCtx,
                                                                index->accessMethod(),
                                                                &obj,
                                                                options,
                                                                loc,
                                                                IndexBuildInterceptor::Op::kDelete,
                                                                &removed);
        if (status.isOK() && keysDeletedOut) {
            *keysDeletedOut += removed;
        }

        return status;
    }

    // On WiredTiger, we do blind unindexing of records for efficiency.  However, when duplicates
    // are allowed in unique indexes, WiredTiger does not do blind unindexing, and instead confirms
    // that the recordid matches the element we are removing.
    //
    // We need to disable blind-deletes for in-progress indexes, in order to force recordid-matching
    // for unindex operations, since initial sync can build an index over a collection with
    // duplicates. See SERVER-17487 for more details.
    options.dupsAllowed = options.dupsAllowed || !index->isReady(opCtx);

    int64_t removed;
    Status status = index->accessMethod()->remove(opCtx, obj, loc, options, &removed);

    if (!status.isOK()) {
        log() << "Couldn't unindex record " << redact(obj) << " from collection "
              << _collection->ns() << ". Status: " << redact(status);
    }

    if (keysDeletedOut) {
        *keysDeletedOut += removed;
    }

    return Status::OK();
}

Status IndexCatalogImpl::indexRecords(OperationContext* opCtx,
                                      const std::vector<BsonRecord>& bsonRecords,
                                      int64_t* keysInsertedOut) {
    if (keysInsertedOut) {
        *keysInsertedOut = 0;
    }

    for (auto&& it : _readyIndexes) {
        Status s = _indexRecords(opCtx, it.get(), bsonRecords, keysInsertedOut);
        if (!s.isOK())
            return s;
    }

    for (auto&& it : _buildingIndexes) {
        Status s = _indexRecords(opCtx, it.get(), bsonRecords, keysInsertedOut);
        if (!s.isOK())
            return s;
    }

    return Status::OK();
}

Status IndexCatalogImpl::updateRecord(OperationContext* const opCtx,
                                      const BSONObj& oldDoc,
                                      const BSONObj& newDoc,
                                      const RecordId& recordId,
                                      int64_t* const keysInsertedOut,
                                      int64_t* const keysDeletedOut) {
    *keysInsertedOut = 0;
    *keysDeletedOut = 0;

    // Ready indexes go directly through the IndexAccessMethod.
    for (IndexCatalogEntryContainer::const_iterator it = _readyIndexes.begin();
         it != _readyIndexes.end();
         ++it) {
        IndexCatalogEntry* entry = it->get();

        IndexDescriptor* descriptor = entry->descriptor();
        IndexAccessMethod* iam = entry->accessMethod();

        InsertDeleteOptions options;
        prepareInsertDeleteOptions(opCtx, descriptor, &options);

        UpdateTicket updateTicket;

        auto status = iam->validateUpdate(
            opCtx, oldDoc, newDoc, recordId, options, &updateTicket, entry->getFilterExpression());
        if (!status.isOK())
            return status;

        int64_t keysInserted;
        int64_t keysDeleted;
        status = iam->update(opCtx, updateTicket, &keysInserted, &keysDeleted);
        if (!status.isOK())
            return status;

        *keysInsertedOut += keysInserted;
        *keysDeletedOut += keysDeleted;
    }

    // Building indexes go through the interceptor.
    BsonRecord record{recordId, Timestamp(), &newDoc};
    for (IndexCatalogEntryContainer::const_iterator it = _buildingIndexes.begin();
         it != _buildingIndexes.end();
         ++it) {
        IndexCatalogEntry* entry = it->get();

        bool logIfError = false;
        invariant(_unindexRecord(opCtx, entry, oldDoc, recordId, logIfError, keysDeletedOut));

        auto status = _indexRecords(opCtx, entry, {record}, keysInsertedOut);
        if (!status.isOK())
            return status;
    }
    return Status::OK();
}

void IndexCatalogImpl::unindexRecord(OperationContext* opCtx,
                                     const BSONObj& obj,
                                     const RecordId& loc,
                                     bool noWarn,
                                     int64_t* keysDeletedOut) {
    if (keysDeletedOut) {
        *keysDeletedOut = 0;
    }

    for (IndexCatalogEntryContainer::const_iterator it = _readyIndexes.begin();
         it != _readyIndexes.end();
         ++it) {
        IndexCatalogEntry* entry = it->get();

        bool logIfError = !noWarn;
        invariant(_unindexRecord(opCtx, entry, obj, loc, logIfError, keysDeletedOut));
    }

    for (IndexCatalogEntryContainer::const_iterator it = _buildingIndexes.begin();
         it != _buildingIndexes.end();
         ++it) {
        IndexCatalogEntry* entry = it->get();

        // If it's a background index, we DO NOT want to log anything.
        bool logIfError = entry->isReady(opCtx) ? !noWarn : false;
        invariant(_unindexRecord(opCtx, entry, obj, loc, logIfError, keysDeletedOut));
    }
}

Status IndexCatalogImpl::compactIndexes(OperationContext* opCtx) {
    for (IndexCatalogEntryContainer::const_iterator it = _readyIndexes.begin();
         it != _readyIndexes.end();
         ++it) {
        IndexCatalogEntry* entry = it->get();

        LOG(1) << "compacting index: " << entry->descriptor()->toString();
        Status status = entry->accessMethod()->compact(opCtx);
        if (!status.isOK()) {
            error() << "failed to compact index: " << entry->descriptor()->toString();
            return status;
        }
    }
    return Status::OK();
}

std::unique_ptr<IndexCatalog::IndexBuildBlockInterface> IndexCatalogImpl::createIndexBuildBlock(
    OperationContext* opCtx, const BSONObj& spec, IndexBuildMethod method) {
    return std::make_unique<IndexBuildBlock>(this, _collection->ns(), spec, method);
}

std::string::size_type IndexCatalogImpl::getLongestIndexNameLength(OperationContext* opCtx) const {
    std::unique_ptr<IndexIterator> it = getIndexIterator(opCtx, true);
    std::string::size_type longestIndexNameLength = 0;
    while (it->more()) {
        auto thisLength = it->next()->descriptor()->indexName().length();
        if (thisLength > longestIndexNameLength)
            longestIndexNameLength = thisLength;
    }
    return longestIndexNameLength;
}

BSONObj IndexCatalogImpl::fixIndexKey(const BSONObj& key) const {
    if (IndexDescriptor::isIdIndexPattern(key)) {
        return _idObj;
    }
    if (key["_id"].type() == Bool && key.nFields() == 1) {
        return _idObj;
    }
    return key;
}

void IndexCatalogImpl::prepareInsertDeleteOptions(OperationContext* opCtx,
                                                  const IndexDescriptor* desc,
                                                  InsertDeleteOptions* options) const {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->shouldRelaxIndexConstraints(opCtx, desc->parentNS())) {
        options->getKeysMode = IndexAccessMethod::GetKeysMode::kRelaxConstraints;
    } else {
        options->getKeysMode = IndexAccessMethod::GetKeysMode::kEnforceConstraints;
    }

    // Don't allow dups for Id key. Allow dups for non-unique keys or when constraints relaxed.
    if (KeyPattern::isIdKeyPattern(desc->keyPattern())) {
        options->dupsAllowed = false;
    } else {
        options->dupsAllowed = !desc->unique() ||
            options->getKeysMode == IndexAccessMethod::GetKeysMode::kRelaxConstraints;
    }
}

void IndexCatalogImpl::indexBuildSuccess(OperationContext* opCtx, IndexCatalogEntry* index) {
    auto releasedEntry = _buildingIndexes.release(index->descriptor());
    invariant(releasedEntry.get() == index);
    _readyIndexes.add(std::move(releasedEntry));

    auto interceptor = index->indexBuildInterceptor();
    index->setIndexBuildInterceptor(nullptr);
    index->setIsReady(true);

    opCtx->recoveryUnit()->onRollback([this, index, interceptor]() {
        auto releasedEntry = _readyIndexes.release(index->descriptor());
        invariant(releasedEntry.get() == index);
        _buildingIndexes.add(std::move(releasedEntry));

        index->setIndexBuildInterceptor(interceptor);
        index->setIsReady(false);
    });
}

StatusWith<BSONObj> IndexCatalogImpl::_fixIndexSpec(OperationContext* opCtx,
                                                    Collection* collection,
                                                    const BSONObj& spec) const {
    auto statusWithSpec = IndexLegacy::adjustIndexSpecObject(spec);
    if (!statusWithSpec.isOK()) {
        return statusWithSpec;
    }
    BSONObj o = statusWithSpec.getValue();

    BSONObjBuilder b;

    // We've already verified in IndexCatalog::_isSpecOk() that the index version is present and
    // that it is representable as a 32-bit integer.
    auto vElt = o["v"];
    invariant(vElt);

    b.append("v", vElt.numberInt());

    if (o["unique"].trueValue())
        b.appendBool("unique", true);  // normalize to bool true in case was int 1 or something...

    BSONObj key = fixIndexKey(o["key"].Obj());
    b.append("key", key);

    string name = o["name"].String();
    if (IndexDescriptor::isIdIndexPattern(key)) {
        name = "_id_";
    }
    b.append("name", name);

    {
        BSONObjIterator i(o);
        while (i.more()) {
            BSONElement e = i.next();
            string s = e.fieldName();

            if (s == "_id") {
                // skip
            } else if (s == "dropDups") {
                // dropDups is silently ignored and removed from the spec as of SERVER-14710.
            } else if (s == "v" || s == "unique" || s == "key" || s == "name") {
                // covered above
            } else {
                b.append(e);
            }
        }
    }

    return b.obj();
}

void IndexCatalogImpl::setNs(NamespaceString ns) {
    for (auto&& ice : _readyIndexes) {
        ice->setNs(ns);
    }

    for (auto&& ice : _buildingIndexes) {
        ice->setNs(ns);
    }
}
}  // namespace mongo
