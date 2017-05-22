/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/index_create.h"
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
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/represent_as.h"

namespace mongo {
namespace {
MONGO_INITIALIZER(InitializeIndexCatalogFactory)(InitializerContext* const) {
    IndexCatalog::registerFactory([](
        IndexCatalog* const this_, Collection* const collection, const int maxNumIndexesAllowed) {
        return stdx::make_unique<IndexCatalogImpl>(this_, collection, maxNumIndexesAllowed);
    });
    return Status::OK();
}

MONGO_INITIALIZER(InitializeIndexCatalogIndexIteratorFactory)(InitializerContext* const) {
    IndexCatalog::IndexIterator::registerFactory([](OperationContext* const opCtx,
                                                    const IndexCatalog* const cat,
                                                    const bool includeUnfinishedIndexes) {
        return stdx::make_unique<IndexCatalogImpl::IndexIteratorImpl>(
            opCtx, cat, includeUnfinishedIndexes);
    });
    return Status::OK();
}

MONGO_INITIALIZER(InitializeFixIndexKeyImpl)(InitializerContext* const) {
    IndexCatalog::registerFixIndexKeyImpl(&IndexCatalogImpl::fixIndexKey);
    return Status::OK();
}

MONGO_INITIALIZER(InitializePrepareInsertDeleteOptionsImpl)(InitializerContext* const) {
    IndexCatalog::registerPrepareInsertDeleteOptionsImpl(
        &IndexCatalogImpl::prepareInsertDeleteOptions);
    return Status::OK();
}

}  // namespace

using std::unique_ptr;
using std::endl;
using std::string;
using std::vector;

using IndexVersion = IndexDescriptor::IndexVersion;

static const int INDEX_CATALOG_INIT = 283711;
static const int INDEX_CATALOG_UNINIT = 654321;

const BSONObj IndexCatalogImpl::_idObj = BSON("_id" << 1);

// -------------

IndexCatalogImpl::IndexCatalogImpl(IndexCatalog* const this_,
                                   Collection* collection,
                                   int maxNumIndexesAllowed)
    : _magic(INDEX_CATALOG_UNINIT),
      _collection(collection),
      _maxNumIndexesAllowed(maxNumIndexesAllowed),
      _this(this_) {}

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

        if (!_collection->getCatalogEntry()->isIndexReady(opCtx, indexName)) {
            _unfinishedIndexes.push_back(spec);
            continue;
        }

        BSONObj keyPattern = spec.getObjectField("key");
        auto descriptor = stdx::make_unique<IndexDescriptor>(
            _collection, _getAccessMethodName(opCtx, keyPattern), spec);
        const bool initFromDisk = true;
        IndexCatalogEntry* entry =
            _setupInMemoryStructures(opCtx, std::move(descriptor), initFromDisk);

        fassert(17340, entry->isReady(opCtx));
    }

    if (_unfinishedIndexes.size()) {
        // if there are left over indexes, we don't let anyone add/drop indexes
        // until someone goes and fixes them
        log() << "found " << _unfinishedIndexes.size()
              << " index(es) that wasn't finished before shutdown";
    }

    _magic = INDEX_CATALOG_INIT;
    return Status::OK();
}

IndexCatalogEntry* IndexCatalogImpl::_setupInMemoryStructures(
    OperationContext* opCtx, std::unique_ptr<IndexDescriptor> descriptor, bool initFromDisk) {
    Status status = _isSpecOk(opCtx, descriptor->infoObj());
    if (!status.isOK() && status != ErrorCodes::IndexAlreadyExists) {
        severe() << "Found an invalid index " << descriptor->infoObj() << " on the "
                 << _collection->ns().ns() << " collection: " << redact(status);
        fassertFailedNoTrace(28782);
    }

    auto* const descriptorPtr = descriptor.get();
    auto entry = stdx::make_unique<IndexCatalogEntry>(opCtx,
                                                      _collection->ns().ns(),
                                                      _collection->getCatalogEntry(),
                                                      std::move(descriptor),
                                                      _collection->infoCache());
    std::unique_ptr<IndexAccessMethod> accessMethod(
        _collection->dbce()->getIndex(opCtx, _collection->getCatalogEntry(), entry.get()));
    entry->init(std::move(accessMethod));

    IndexCatalogEntry* save = entry.get();
    _entries.add(entry.release());

    if (!initFromDisk) {
        opCtx->recoveryUnit()->onRollback([ this, opCtx, descriptor = descriptorPtr ] {
            // Need to preserve indexName as descriptor no longer exists after remove().
            const std::string indexName = descriptor->indexName();
            _entries.remove(descriptor);
            _collection->infoCache()->droppedIndex(opCtx, indexName);
        });
    }

    invariant(save == _entries.find(descriptorPtr));
    invariant(save == _entries.find(descriptorPtr->indexName()));

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

Status IndexCatalogImpl::checkUnfinished() const {
    if (_unfinishedIndexes.size() == 0)
        return Status::OK();

    return Status(ErrorCodes::InternalError,
                  str::stream() << "IndexCatalog has left over indexes that must be cleared"
                                << " ns: "
                                << _collection->ns().ns());
}

bool IndexCatalogImpl::_shouldOverridePlugin(OperationContext* opCtx,
                                             const BSONObj& keyPattern) const {
    string pluginName = IndexNames::findPluginName(keyPattern);
    bool known = IndexNames::isKnownName(pluginName);

    if (!_collection->dbce()->isOlderThan24(opCtx)) {
        // RulesFor24+
        // This assert will be triggered when downgrading from a future version that
        // supports an index plugin unsupported by this version.
        uassert(17197,
                str::stream() << "Invalid index type '" << pluginName << "' "
                              << "in index "
                              << keyPattern,
                known);
        return false;
    }

    // RulesFor22
    if (!known) {
        log() << "warning: can't find plugin [" << pluginName << "]";
        return true;
    }

    if (!IndexNames::existedBefore24(pluginName)) {
        warning() << "Treating index " << keyPattern << " as ascending since "
                  << "it was created before 2.4 and '" << pluginName << "' "
                  << "was not a valid type at that time.";
        return true;
    }

    return false;
}

string IndexCatalogImpl::_getAccessMethodName(OperationContext* opCtx,
                                              const BSONObj& keyPattern) const {
    if (_shouldOverridePlugin(opCtx, keyPattern)) {
        return "";
    }

    return IndexNames::findPluginName(keyPattern);
}


// ---------------------------

Status IndexCatalogImpl::_upgradeDatabaseMinorVersionIfNeeded(OperationContext* opCtx,
                                                              const string& newPluginName) {
    // first check if requested index requires pdfile minor version to be bumped
    if (IndexNames::existedBefore24(newPluginName)) {
        return Status::OK();
    }

    DatabaseCatalogEntry* dbce = _collection->dbce();

    if (!dbce->isOlderThan24(opCtx)) {
        return Status::OK();  // these checks have already been done
    }

    // Everything below is MMAPv1 specific since it was the only storage engine that existed
    // before 2.4. We look at all indexes in this database to make sure that none of them use
    // plugins that didn't exist before 2.4. If that holds, we mark the database as "2.4-clean"
    // which allows creation of indexes using new plugins.

    RecordStore* indexes = dbce->getRecordStore(dbce->name() + ".system.indexes");
    auto cursor = indexes->getCursor(opCtx);
    while (auto record = cursor->next()) {
        const BSONObj index = record->data.releaseToBson();
        const BSONObj key = index.getObjectField("key");
        const string plugin = IndexNames::findPluginName(key);
        if (IndexNames::existedBefore24(plugin))
            continue;

        const string errmsg = str::stream()
            << "Found pre-existing index " << index << " with invalid type '" << plugin << "'. "
            << "Disallowing creation of new index type '" << newPluginName << "'. See "
            << "http://dochub.mongodb.org/core/index-type-changes";

        return Status(ErrorCodes::CannotCreateIndex, errmsg);
    }

    dbce->markIndexSafe24AndUp(opCtx);

    return Status::OK();
}

StatusWith<BSONObj> IndexCatalogImpl::prepareSpecForCreate(OperationContext* opCtx,
                                                           const BSONObj& original) const {
    Status status = _isSpecOk(opCtx, original);
    if (!status.isOK())
        return StatusWith<BSONObj>(status);

    auto fixed = _fixIndexSpec(opCtx, _collection, original);
    if (!fixed.isOK()) {
        return fixed;
    }

    // we double check with new index spec
    status = _isSpecOk(opCtx, fixed.getValue());
    if (!status.isOK())
        return StatusWith<BSONObj>(status);

    status = _doesSpecConflictWithExisting(opCtx, fixed.getValue());
    if (!status.isOK())
        return StatusWith<BSONObj>(status);

    return fixed;
}

StatusWith<BSONObj> IndexCatalogImpl::createIndexOnEmptyCollection(OperationContext* opCtx,
                                                                   BSONObj spec) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_collection->ns().toString(), MODE_X));
    invariant(_collection->numRecords(opCtx) == 0);

    _checkMagic();
    Status status = checkUnfinished();
    if (!status.isOK())
        return status;

    StatusWith<BSONObj> statusWithSpec = prepareSpecForCreate(opCtx, spec);
    status = statusWithSpec.getStatus();
    if (!status.isOK())
        return status;
    spec = statusWithSpec.getValue();

    string pluginName = IndexNames::findPluginName(spec["key"].Obj());
    if (pluginName.size()) {
        Status s = _upgradeDatabaseMinorVersionIfNeeded(opCtx, pluginName);
        if (!s.isOK())
            return s;
    }

    // now going to touch disk
    IndexBuildBlock indexBuildBlock(opCtx, _collection, spec);
    status = indexBuildBlock.init();
    if (!status.isOK())
        return status;

    // sanity checks, etc...
    IndexCatalogEntry* entry = indexBuildBlock.getEntry();
    invariant(entry);
    IndexDescriptor* descriptor = entry->descriptor();
    invariant(descriptor);
    invariant(entry == _entries.find(descriptor));

    status = entry->accessMethod()->initializeAsEmpty(opCtx);
    if (!status.isOK())
        return status;
    indexBuildBlock.success();

    // sanity check
    invariant(_collection->getCatalogEntry()->isIndexReady(opCtx, descriptor->indexName()));

    return spec;
}

IndexCatalogImpl::IndexBuildBlock::IndexBuildBlock(OperationContext* opCtx,
                                                   Collection* collection,
                                                   const BSONObj& spec)
    : _collection(collection),
      _catalog(collection->getIndexCatalog()),
      _ns(_catalog->_getCollection()->ns().ns()),
      _spec(spec.getOwned()),
      _entry(nullptr),
      _opCtx(opCtx) {
    invariant(collection);
}

Status IndexCatalogImpl::IndexBuildBlock::init() {
    // need this first for names, etc...
    BSONObj keyPattern = _spec.getObjectField("key");
    auto descriptor = stdx::make_unique<IndexDescriptor>(
        _collection, IndexNames::findPluginName(keyPattern), _spec);

    _indexName = descriptor->indexName();
    _indexNamespace = descriptor->indexNamespace();

    /// ----------   setup on disk structures ----------------

    Status status = _collection->getCatalogEntry()->prepareForIndexBuild(_opCtx, descriptor.get());
    if (!status.isOK())
        return status;

    auto* const descriptorPtr = descriptor.get();
    /// ----------   setup in memory structures  ----------------
    const bool initFromDisk = false;
    _entry = IndexCatalogImpl::_setupInMemoryStructures(
        _catalog, _opCtx, std::move(descriptor), initFromDisk);

    // Register this index with the CollectionInfoCache to regenerate the cache. This way, updates
    // occurring while an index is being build in the background will be aware of whether or not
    // they need to modify any indexes.
    _collection->infoCache()->addedIndex(_opCtx, descriptorPtr);

    return Status::OK();
}

IndexCatalogImpl::IndexBuildBlock::~IndexBuildBlock() {
    // Don't need to call fail() here, as rollback will clean everything up for us.
}

void IndexCatalogImpl::IndexBuildBlock::fail() {
    fassert(17204, _catalog->_getCollection()->ok());  // defensive

    IndexCatalogEntry* entry = IndexCatalog::_getEntries(_catalog).find(_indexName);
    invariant(entry == _entry);

    if (entry) {
        IndexCatalogImpl::_dropIndex(_catalog, _opCtx, entry);
    } else {
        IndexCatalog::_deleteIndexFromDisk(_catalog, _opCtx, _indexName, _indexNamespace);
    }
}

void IndexCatalogImpl::IndexBuildBlock::success() {
    Collection* collection = _catalog->_getCollection();
    fassert(17207, collection->ok());
    NamespaceString ns(_indexNamespace);
    invariant(_opCtx->lockState()->isDbLockedForMode(ns.db(), MODE_X));

    collection->getCatalogEntry()->indexBuildSuccess(_opCtx, _indexName);

    IndexDescriptor* desc = _catalog->findIndexByName(_opCtx, _indexName, true);
    fassert(17330, desc);
    IndexCatalogEntry* entry = _catalog->_getEntries().find(desc);
    fassert(17331, entry && entry == _entry);

    OperationContext* opCtx = _opCtx;
    LOG(2) << "marking index " << _indexName << " as ready in snapshot id "
           << opCtx->recoveryUnit()->getSnapshotId();
    _opCtx->recoveryUnit()->onCommit([opCtx, entry, collection] {
        // Note: this runs after the WUOW commits but before we release our X lock on the
        // collection. This means that any snapshot created after this must include the full index,
        // and no one can try to read this index before we set the visibility.
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        auto snapshotName = replCoord->reserveSnapshotName(opCtx);
        replCoord->forceSnapshotCreation();  // Ensures a newer snapshot gets created even if idle.
        entry->setMinimumVisibleSnapshot(snapshotName);

        // TODO remove this once SERVER-20439 is implemented. It is a stopgap solution for
        // SERVER-20260 to make sure that reads with majority readConcern level can see indexes that
        // are created with w:majority by making the readers block.
        collection->setMinimumVisibleSnapshot(snapshotName);
    });

    entry->setIsReady(true);
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
                                        << expression->toString());
    }
}
}

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

    // SERVER-16893 Forbid use of v0 indexes with non-mmapv1 engines
    if (indexVersion == IndexVersion::kV0 &&
        !opCtx->getServiceContext()->getGlobalStorageEngine()->isMmapV1()) {
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream() << "use of v0 indexes is only allowed with the "
                                    << "mmapv1 storage engine");
    }

    if (!IndexDescriptor::isIndexVersionSupported(indexVersion)) {
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream() << "this version of mongod cannot build new indexes "
                                    << "of version number "
                                    << static_cast<int>(indexVersion));
    }

    if (nss.isSystemDotIndexes())
        return Status(ErrorCodes::CannotCreateIndex,
                      "cannot have an index on the system.indexes collection");

    if (nss.isOplog())
        return Status(ErrorCodes::CannotCreateIndex, "cannot have an index on the oplog");

    if (nss.coll() == "$freelist") {
        // this isn't really proper, but we never want it and its not an error per se
        return Status(ErrorCodes::IndexAlreadyExists, "cannot index freelist");
    }

    const BSONElement specNamespace = spec["ns"];
    if (specNamespace.type() != String)
        return Status(ErrorCodes::CannotCreateIndex,
                      "the index spec is missing a \"ns\" string field");

    if (nss.ns() != specNamespace.valueStringData())
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream() << "the \"ns\" field of the index spec '"
                                    << specNamespace.valueStringData()
                                    << "' does not match the collection name '"
                                    << nss.ns()
                                    << "'");

    // logical name of the index
    const BSONElement nameElem = spec["name"];
    if (nameElem.type() != String)
        return Status(ErrorCodes::CannotCreateIndex, "index name must be specified as a string");

    const StringData name = nameElem.valueStringData();
    if (name.find('\0') != std::string::npos)
        return Status(ErrorCodes::CannotCreateIndex, "index name cannot contain NUL bytes");

    if (name.empty())
        return Status(ErrorCodes::CannotCreateIndex, "index name cannot be empty");

    const std::string indexNamespace = IndexDescriptor::makeIndexNamespace(nss.ns(), name);
    if (indexNamespace.length() > NamespaceString::MaxNsLen)
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream() << "namespace name generated from index name \""
                                    << indexNamespace
                                    << "\" is too long (127 byte max)");

    const BSONObj key = spec.getObjectField("key");
    const Status keyStatus = index_key_validate::validateKeyPattern(key, indexVersion);
    if (!keyStatus.isOK()) {
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream() << "bad index key pattern " << key << ": "
                                    << keyStatus.reason());
    }

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

        string pluginName = IndexNames::findPluginName(key);
        if ((pluginName != IndexNames::BTREE) && (pluginName != IndexNames::GEO_2DSPHERE) &&
            (pluginName != IndexNames::HASHED)) {
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream() << "Index type '" << pluginName
                                        << "' does not support collation: "
                                        << collator->getSpec().toBSON());
        }
    }

    const bool isSparse = spec["sparse"].trueValue();

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
        StatusWithMatchExpression statusWithMatcher = MatchExpressionParser::parse(
            filterElement.Obj(), ExtensionsCallbackDisallowExtensions(), collator.get());
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
    } else {
        // for non _id indexes, we check to see if replication has turned off all indexes
        // we _always_ created _id index
        if (!repl::getGlobalReplicationCoordinator()->buildsIndexes()) {
            // this is not exactly the right error code, but I think will make the most sense
            return Status(ErrorCodes::IndexAlreadyExists, "no indexes per repl");
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
    Status storageEngineStatus =
        validateStorageOptions(storageEngineOptions,
                               stdx::bind(&StorageEngine::Factory::validateIndexStorageOptions,
                                          stdx::placeholders::_1,
                                          stdx::placeholders::_2));
    if (!storageEngineStatus.isOK()) {
        return storageEngineStatus;
    }

    return Status::OK();
}

Status IndexCatalogImpl::_doesSpecConflictWithExisting(OperationContext* opCtx,
                                                       const BSONObj& spec) const {
    const char* name = spec.getStringField("name");
    invariant(name[0]);

    const BSONObj key = spec.getObjectField("key");
    const BSONObj collation = spec.getObjectField("collation");

    {
        // Check both existing and in-progress indexes (2nd param = true)
        const IndexDescriptor* desc = findIndexByName(opCtx, name, true);
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

            IndexDescriptor temp(_collection, _getAccessMethodName(opCtx, key), spec);
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
        // Check both existing and in-progress indexes.
        const bool findInProgressIndexes = true;
        const IndexDescriptor* desc =
            findIndexByKeyPatternAndCollationSpec(opCtx, key, collation, findInProgressIndexes);
        if (desc) {
            LOG(2) << "index already exists with diff name " << name << " pattern: " << key
                   << " collation: " << collation;

            IndexDescriptor temp(_collection, _getAccessMethodName(opCtx, key), spec);
            if (!desc->areIndexOptionsEquivalent(&temp))
                return Status(ErrorCodes::IndexOptionsConflict,
                              str::stream() << "Index: " << spec
                                            << " already exists with different options: "
                                            << desc->infoObj());

            return Status(ErrorCodes::IndexAlreadyExists,
                          str::stream() << "index already exists with different name: " << name);
        }
    }

    if (numIndexesTotal(opCtx) >= _maxNumIndexesAllowed) {
        string s = str::stream() << "add index fails, too many indexes for "
                                 << _collection->ns().ns() << " key:" << key;
        log() << s;
        return Status(ErrorCodes::CannotCreateIndex, s);
    }

    // Refuse to build text index if another text index exists or is in progress.
    // Collections should only have one text index.
    string pluginName = IndexNames::findPluginName(key);
    if (pluginName == IndexNames::TEXT) {
        vector<IndexDescriptor*> textIndexes;
        const bool includeUnfinishedIndexes = true;
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

BSONObj IndexCatalogImpl::getDefaultIdIndexSpec(
    ServerGlobalParams::FeatureCompatibility::Version featureCompatibilityVersion) const {
    dassert(_idObj["_id"].type() == NumberInt);

    const auto indexVersion = IndexDescriptor::getDefaultIndexVersion(featureCompatibilityVersion);

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
                                      std::map<std::string, BSONObj>* droppedIndexes) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_collection->ns().toString(), MODE_X));

    BackgroundOperation::assertNoBgOpInProgForNs(_collection->ns().ns());

    // there may be pointers pointing at keys in the btree(s).  kill them.
    // TODO: can this can only clear cursors on this index?
    _collection->getCursorManager()->invalidateAll(
        opCtx, false, "all indexes on collection dropped");

    // make sure nothing in progress
    massert(17348,
            "cannot dropAllIndexes when index builds in progress",
            numIndexesTotal(opCtx) == numIndexesReady(opCtx));

    bool haveIdIndex = false;

    vector<string> indexNamesToDrop;
    {
        int seen = 0;
        IndexIterator ii = _this->getIndexIterator(opCtx, true);
        while (ii.more()) {
            seen++;
            IndexDescriptor* desc = ii.next();
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
        IndexDescriptor* desc = findIndexByName(opCtx, indexName, true);
        invariant(desc);
        LOG(1) << "\t dropAllIndexes dropping: " << desc->toString();
        IndexCatalogEntry* entry = _entries.find(desc);
        invariant(entry);
        _dropIndex(opCtx, entry);

        if (droppedIndexes != nullptr) {
            droppedIndexes->emplace(desc->indexName(), desc->infoObj());
        }
    }

    // verify state is sane post cleaning

    long long numIndexesInCollectionCatalogEntry =
        _collection->getCatalogEntry()->getTotalIndexCount(opCtx);

    if (haveIdIndex) {
        fassert(17324, numIndexesTotal(opCtx) == 1);
        fassert(17325, numIndexesReady(opCtx) == 1);
        fassert(17326, numIndexesInCollectionCatalogEntry == 1);
        fassert(17336, _entries.size() == 1);
    } else {
        if (numIndexesTotal(opCtx) || numIndexesInCollectionCatalogEntry || _entries.size()) {
            error() << "About to fassert - "
                    << " numIndexesTotal(): " << numIndexesTotal(opCtx)
                    << " numSystemIndexesEntries: " << numIndexesInCollectionCatalogEntry
                    << " _entries.size(): " << _entries.size()
                    << " indexNamesToDrop: " << indexNamesToDrop.size()
                    << " haveIdIndex: " << haveIdIndex;
        }
        fassert(17327, numIndexesTotal(opCtx) == 0);
        fassert(17328, numIndexesInCollectionCatalogEntry == 0);
        fassert(17337, _entries.size() == 0);
    }
}

Status IndexCatalogImpl::dropIndex(OperationContext* opCtx, IndexDescriptor* desc) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_collection->ns().toString(), MODE_X));
    IndexCatalogEntry* entry = _entries.find(desc);

    if (!entry)
        return Status(ErrorCodes::InternalError, "cannot find index to delete");

    if (!entry->isReady(opCtx))
        return Status(ErrorCodes::InternalError, "cannot delete not ready index");

    BackgroundOperation::assertNoBgOpInProgForNs(_collection->ns().ns());

    return _dropIndex(opCtx, entry);
}

namespace {
class IndexRemoveChange final : public RecoveryUnit::Change {
public:
    IndexRemoveChange(OperationContext* opCtx,
                      Collection* collection,
                      IndexCatalogEntryContainer* entries,
                      IndexCatalogEntry* entry)
        : _opCtx(opCtx), _collection(collection), _entries(entries), _entry(entry) {}

    void commit() final {
        // Ban reading from this collection on committed reads on snapshots before now.
        auto replCoord = repl::ReplicationCoordinator::get(_opCtx);
        auto snapshotName = replCoord->reserveSnapshotName(_opCtx);
        replCoord->forceSnapshotCreation();  // Ensures a newer snapshot gets created even if idle.
        _collection->setMinimumVisibleSnapshot(snapshotName);

        delete _entry;
    }

    void rollback() final {
        _entries->add(_entry);
        _collection->infoCache()->addedIndex(_opCtx, _entry->descriptor());
    }

private:
    OperationContext* _opCtx;
    Collection* _collection;
    IndexCatalogEntryContainer* _entries;
    IndexCatalogEntry* _entry;
};
}  // namespace

Status IndexCatalogImpl::_dropIndex(OperationContext* opCtx, IndexCatalogEntry* entry) {
    /**
     * IndexState in order
     *  <db>.system.indexes
     *    NamespaceDetails
     *      <db>.system.ns
     */

    // ----- SANITY CHECKS -------------
    if (!entry)
        return Status(ErrorCodes::BadValue, "IndexCatalog::_dropIndex passed NULL");

    _checkMagic();
    Status status = checkUnfinished();
    if (!status.isOK())
        return status;

    // Pulling indexName/indexNamespace out as they are needed post descriptor release.
    string indexName = entry->descriptor()->indexName();
    string indexNamespace = entry->descriptor()->indexNamespace();

    // If any cursors could be using this index, invalidate them. Note that we do not use indexes
    // until they are ready, so we do not need to invalidate anything if the index fails while it is
    // being built.
    // TODO only kill cursors that are actually using the index rather than everything on this
    // collection.
    if (entry->isReady(opCtx)) {
        _collection->getCursorManager()->invalidateAll(
            opCtx, false, str::stream() << "index '" << indexName << "' dropped");
    }

    // --------- START REAL WORK ----------
    audit::logDropIndex(&cc(), indexName, _collection->ns().ns());

    invariant(_entries.release(entry->descriptor()) == entry);
    opCtx->recoveryUnit()->registerChange(
        new IndexRemoveChange(opCtx, _collection, &_entries, entry));
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

vector<BSONObj> IndexCatalogImpl::getAndClearUnfinishedIndexes(OperationContext* opCtx) {
    vector<BSONObj> toReturn = _unfinishedIndexes;
    _unfinishedIndexes.clear();
    for (size_t i = 0; i < toReturn.size(); i++) {
        BSONObj spec = toReturn[i];

        BSONObj keyPattern = spec.getObjectField("key");
        IndexDescriptor desc(_collection, _getAccessMethodName(opCtx, keyPattern), spec);

        _deleteIndexFromDisk(opCtx, desc.indexName(), desc.indexNamespace());
    }
    return toReturn;
}

bool IndexCatalogImpl::isMultikey(OperationContext* opCtx, const IndexDescriptor* idx) {
    IndexCatalogEntry* entry = _entries.find(idx);
    invariant(entry);
    return entry->isMultikey();
}

MultikeyPaths IndexCatalogImpl::getMultikeyPaths(OperationContext* opCtx,
                                                 const IndexDescriptor* idx) {
    IndexCatalogEntry* entry = _entries.find(idx);
    invariant(entry);
    return entry->getMultikeyPaths(opCtx);
}

// ---------------------------

bool IndexCatalogImpl::haveAnyIndexes() const {
    return _entries.size() != 0;
}

int IndexCatalogImpl::numIndexesTotal(OperationContext* opCtx) const {
    int count = _entries.size() + _unfinishedIndexes.size();
    dassert(_collection->getCatalogEntry()->getTotalIndexCount(opCtx) == count);
    return count;
}

int IndexCatalogImpl::numIndexesReady(OperationContext* opCtx) const {
    int count = 0;
    IndexIterator ii = _this->getIndexIterator(opCtx, /*includeUnfinished*/ false);
    while (ii.more()) {
        ii.next();
        count++;
    }
    dassert(_collection->getCatalogEntry()->getCompletedIndexCount(opCtx) == count);
    return count;
}

bool IndexCatalogImpl::haveIdIndex(OperationContext* opCtx) const {
    return findIdIndex(opCtx) != nullptr;
}

IndexCatalogImpl::IndexIteratorImpl::IndexIteratorImpl(OperationContext* opCtx,
                                                       const IndexCatalog* cat,
                                                       bool includeUnfinishedIndexes)
    : _includeUnfinishedIndexes(includeUnfinishedIndexes),
      _opCtx(opCtx),
      _catalog(cat),
      _iterator(cat->_getEntries().begin()),
      _start(true),
      _prev(nullptr),
      _next(nullptr) {}

auto IndexCatalogImpl::IndexIteratorImpl::clone_impl() const -> IndexIteratorImpl* {
    return new IndexIteratorImpl(*this);
}

bool IndexCatalogImpl::IndexIteratorImpl::more() {
    if (_start) {
        _advance();
        _start = false;
    }
    return _next != nullptr;
}

IndexDescriptor* IndexCatalogImpl::IndexIteratorImpl::next() {
    if (!more())
        return nullptr;
    _prev = _next;
    _advance();
    return _prev->descriptor();
}

IndexAccessMethod* IndexCatalogImpl::IndexIteratorImpl::accessMethod(const IndexDescriptor* desc) {
    invariant(desc == _prev->descriptor());
    return _prev->accessMethod();
}

IndexCatalogEntry* IndexCatalogImpl::IndexIteratorImpl::catalogEntry(const IndexDescriptor* desc) {
    invariant(desc == _prev->descriptor());
    return _prev;
}

void IndexCatalogImpl::IndexIteratorImpl::_advance() {
    _next = nullptr;

    while (_iterator != _catalog->_getEntries().end()) {
        IndexCatalogEntry* entry = _iterator->get();
        ++_iterator;

        if (!_includeUnfinishedIndexes) {
            if (auto minSnapshot = entry->getMinimumVisibleSnapshot()) {
                if (auto mySnapshot = _opCtx->recoveryUnit()->getMajorityCommittedSnapshot()) {
                    if (mySnapshot < minSnapshot) {
                        // This index isn't finished in my snapshot.
                        continue;
                    }
                }
            }

            if (!entry->isReady(_opCtx))
                continue;
        }

        _next = entry;
        return;
    }
}


IndexDescriptor* IndexCatalogImpl::findIdIndex(OperationContext* opCtx) const {
    IndexIterator ii = _this->getIndexIterator(opCtx, false);
    while (ii.more()) {
        IndexDescriptor* desc = ii.next();
        if (desc->isIdIndex())
            return desc;
    }
    return nullptr;
}

IndexDescriptor* IndexCatalogImpl::findIndexByName(OperationContext* opCtx,
                                                   StringData name,
                                                   bool includeUnfinishedIndexes) const {
    IndexIterator ii = _this->getIndexIterator(opCtx, includeUnfinishedIndexes);
    while (ii.more()) {
        IndexDescriptor* desc = ii.next();
        if (desc->indexName() == name)
            return desc;
    }
    return nullptr;
}

IndexDescriptor* IndexCatalogImpl::findIndexByKeyPatternAndCollationSpec(
    OperationContext* opCtx,
    const BSONObj& key,
    const BSONObj& collationSpec,
    bool includeUnfinishedIndexes) const {
    IndexIterator ii = _this->getIndexIterator(opCtx, includeUnfinishedIndexes);
    while (ii.more()) {
        IndexDescriptor* desc = ii.next();
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
                                               std::vector<IndexDescriptor*>* matches) const {
    invariant(matches);
    IndexIterator ii = _this->getIndexIterator(opCtx, includeUnfinishedIndexes);
    while (ii.more()) {
        IndexDescriptor* desc = ii.next();
        if (SimpleBSONObjComparator::kInstance.evaluate(desc->keyPattern() == key)) {
            matches->push_back(desc);
        }
    }
}

IndexDescriptor* IndexCatalogImpl::findShardKeyPrefixedIndex(OperationContext* opCtx,
                                                             const BSONObj& shardKey,
                                                             bool requireSingleKey) const {
    IndexDescriptor* best = nullptr;

    IndexIterator ii = _this->getIndexIterator(opCtx, false);
    while (ii.more()) {
        IndexDescriptor* desc = ii.next();
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
                                       vector<IndexDescriptor*>& matches,
                                       bool includeUnfinishedIndexes) const {
    IndexIterator ii = _this->getIndexIterator(opCtx, includeUnfinishedIndexes);
    while (ii.more()) {
        IndexDescriptor* desc = ii.next();
        if (IndexNames::findPluginName(desc->keyPattern()) == type) {
            matches.push_back(desc);
        }
    }
}

IndexAccessMethod* IndexCatalogImpl::getIndex(const IndexDescriptor* desc) {
    IndexCatalogEntry* entry = _entries.find(desc);
    massert(17334, "cannot find index entry", entry);
    return entry->accessMethod();
}

const IndexAccessMethod* IndexCatalogImpl::getIndex(const IndexDescriptor* desc) const {
    return getEntry(desc)->accessMethod();
}

const IndexCatalogEntry* IndexCatalogImpl::getEntry(const IndexDescriptor* desc) const {
    const IndexCatalogEntry* entry = _entries.find(desc);
    massert(17357, "cannot find index entry", entry);
    return entry;
}


const IndexDescriptor* IndexCatalogImpl::refreshEntry(OperationContext* opCtx,
                                                      const IndexDescriptor* oldDesc) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_collection->ns().ns(), MODE_X));
    invariant(!BackgroundOperation::inProgForNs(_collection->ns()));

    const std::string indexName = oldDesc->indexName();
    invariant(_collection->getCatalogEntry()->isIndexReady(opCtx, indexName));

    // Notify other users of the IndexCatalog that we're about to invalidate 'oldDesc'.
    const bool collectionGoingAway = false;
    _collection->getCursorManager()->invalidateAll(
        opCtx,
        collectionGoingAway,
        str::stream() << "definition of index '" << indexName << "' changed");

    // Delete the IndexCatalogEntry that owns this descriptor.  After deletion, 'oldDesc' is
    // invalid and should not be dereferenced.
    IndexCatalogEntry* oldEntry = _entries.release(oldDesc);
    opCtx->recoveryUnit()->registerChange(
        new IndexRemoveChange(opCtx, _collection, &_entries, oldEntry));

    // Ask the CollectionCatalogEntry for the new index spec.
    BSONObj spec = _collection->getCatalogEntry()->getIndexSpec(opCtx, indexName).getOwned();
    BSONObj keyPattern = spec.getObjectField("key");

    // Re-register this index in the index catalog with the new spec.
    auto newDesc = stdx::make_unique<IndexDescriptor>(
        _collection, _getAccessMethodName(opCtx, keyPattern), spec);
    const bool initFromDisk = false;
    const IndexCatalogEntry* newEntry =
        _setupInMemoryStructures(opCtx, std::move(newDesc), initFromDisk);
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
        int64_t inserted;
        invariant(bsonRecord.id != RecordId());
        Status status = index->accessMethod()->insert(
            opCtx, *bsonRecord.docPtr, bsonRecord.id, options, &inserted);
        if (!status.isOK())
            return status;

        if (keysInsertedOut) {
            *keysInsertedOut += inserted;
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

    // On WiredTiger, we do blind unindexing of records for efficiency.  However, when duplicates
    // are allowed in unique indexes, WiredTiger does not do blind unindexing, and instead confirms
    // that the recordid matches the element we are removing.
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

    for (IndexCatalogEntryContainer::const_iterator i = _entries.begin(); i != _entries.end();
         ++i) {
        Status s = _indexRecords(opCtx, i->get(), bsonRecords, keysInsertedOut);
        if (!s.isOK())
            return s;
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

    for (IndexCatalogEntryContainer::const_iterator i = _entries.begin(); i != _entries.end();
         ++i) {
        IndexCatalogEntry* entry = i->get();

        // If it's a background index, we DO NOT want to log anything.
        bool logIfError = entry->isReady(opCtx) ? !noWarn : false;
        _unindexRecord(opCtx, entry, obj, loc, logIfError, keysDeletedOut);
    }
}

BSONObj IndexCatalogImpl::fixIndexKey(const BSONObj& key) {
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
                                                  InsertDeleteOptions* options) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->shouldRelaxIndexConstraints(opCtx, NamespaceString(desc->parentNS()))) {
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

StatusWith<BSONObj> IndexCatalogImpl::_fixIndexSpec(OperationContext* opCtx,
                                                    Collection* collection,
                                                    const BSONObj& spec) {
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
}  // namespace mongo
