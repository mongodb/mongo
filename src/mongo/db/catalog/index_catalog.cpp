// index_catalog.cpp

/**
*    Copyright (C) 2013-2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndexing

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_catalog.h"

#include <vector>

#include "mongo/db/audit.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/curop.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_legacy.h"
#include "mongo/db/index_names.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/repl/rs.h" // this is ugly
#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

    static const int INDEX_CATALOG_INIT = 283711;
    static const int INDEX_CATALOG_UNINIT = 654321;

    // What's the default version of our indices?
    const int DefaultIndexVersionNumber = 1;

    const BSONObj IndexCatalog::_idObj = BSON( "_id" << 1 );

    // -------------

    IndexCatalog::IndexCatalog( Collection* collection )
        : _magic(INDEX_CATALOG_UNINIT), _collection( collection ) {
    }

    IndexCatalog::~IndexCatalog() {
        if ( _magic != INDEX_CATALOG_UNINIT ) {
            // only do this check if we haven't been initialized
            _checkMagic();
        }
        _magic = 123456;
    }

    Status IndexCatalog::init(OperationContext* txn) {
        vector<string> indexNames;
        _collection->getCatalogEntry()->getAllIndexes( &indexNames );

        for ( size_t i = 0; i < indexNames.size(); i++ ) {
            const string& indexName = indexNames[i];
            BSONObj spec = _collection->getCatalogEntry()->getIndexSpec( indexName ).getOwned();

            if ( !_collection->getCatalogEntry()->isIndexReady( indexName ) ) {
                _unfinishedIndexes.push_back( spec );
                continue;
            }

            BSONObj keyPattern = spec.getObjectField("key");
            IndexDescriptor* descriptor = new IndexDescriptor( _collection,
                                                               _getAccessMethodName(txn, keyPattern),
                                                               spec );
            IndexCatalogEntry* entry = _setupInMemoryStructures( txn, descriptor );

            fassert( 17340, entry->isReady()  );
        }

        if ( _unfinishedIndexes.size() ) {
            // if there are left over indexes, we don't let anyone add/drop indexes
            // until someone goes and fixes them
            log() << "found " << _unfinishedIndexes.size()
                  << " index(es) that wasn't finished before shutdown";
        }

        _magic = INDEX_CATALOG_INIT;
        return Status::OK();
    }

    IndexCatalogEntry* IndexCatalog::_setupInMemoryStructures(OperationContext* txn,
                                                              IndexDescriptor* descriptor) {
        auto_ptr<IndexDescriptor> descriptorCleanup( descriptor );

        auto_ptr<IndexCatalogEntry> entry( new IndexCatalogEntry( _collection->ns().ns(),
                                                                  _collection->getCatalogEntry(),
                                                                  descriptorCleanup.release(),
                                                                  _collection->infoCache() ) );

        entry->init( _collection->_database->_dbEntry->getIndex( txn,
                                                                 _collection->getCatalogEntry(),
                                                                 entry.get() ) );

        IndexCatalogEntry* save = entry.get();
        _entries.add( entry.release() );

        invariant( save == _entries.find( descriptor ) );
        invariant( save == _entries.find( descriptor->indexName() ) );

        return save;
    }

    bool IndexCatalog::ok() const {
        return ( _magic == INDEX_CATALOG_INIT );
    }

    void IndexCatalog::_checkMagic() const {
        if ( ok() ) {
            return;
        }
        log() << "IndexCatalog::_magic wrong, is : " << _magic;
        fassertFailed(17198);
    }

    Status IndexCatalog::checkUnfinished() const {
        if ( _unfinishedIndexes.size() == 0 )
            return Status::OK();

        return Status( ErrorCodes::InternalError,
                       str::stream()
                       << "IndexCatalog has left over indexes that must be cleared"
                       << " ns: " << _collection->ns().ns() );
    }

    bool IndexCatalog::_shouldOverridePlugin(OperationContext* txn,
                                             const BSONObj& keyPattern) const {
        string pluginName = IndexNames::findPluginName(keyPattern);
        bool known = IndexNames::isKnownName(pluginName);

        if ( !_collection->_database->getDatabaseCatalogEntry()->isOlderThan24( txn ) ) {
            // RulesFor24+
            // This assert will be triggered when downgrading from a future version that
            // supports an index plugin unsupported by this version.
            uassert(17197, str::stream() << "Invalid index type '" << pluginName << "' "
                    << "in index " << keyPattern,
                    known);
            return false;
        }

        // RulesFor22
        if (!known) {
            log() << "warning: can't find plugin [" << pluginName << "]" << endl;
            return true;
        }

        if (!IndexNames::existedBefore24(pluginName)) {
            warning() << "Treating index " << keyPattern << " as ascending since "
                      << "it was created before 2.4 and '" << pluginName << "' "
                      << "was not a valid type at that time."
                      << endl;
            return true;
        }

        return false;
    }

    string IndexCatalog::_getAccessMethodName(OperationContext* txn,
                                              const BSONObj& keyPattern) const {
        if ( _shouldOverridePlugin(txn, keyPattern) ) {
            return "";
        }

        return IndexNames::findPluginName(keyPattern);
    }


    // ---------------------------

    Status IndexCatalog::_upgradeDatabaseMinorVersionIfNeeded( OperationContext* txn,
                                                               const string& newPluginName ) {

        // first check if requested index requires pdfile minor version to be bumped
        if ( IndexNames::existedBefore24(newPluginName) ) {
            return Status::OK();
        }

        Database* db = _collection->_database;

        if ( !db->getDatabaseCatalogEntry()->isOlderThan24( txn ) ) {
            return Status::OK(); // these checks have already been done
        }

        auto_ptr<PlanExecutor> exec(
            InternalPlanner::collectionScan(txn,
                                            db->_indexesName,
                                            db->getCollection(txn, db->_indexesName)));

        BSONObj index;
        PlanExecutor::ExecState state;
        while ( PlanExecutor::ADVANCED == (state = exec->getNext(&index, NULL)) ) {
            const BSONObj key = index.getObjectField("key");
            const string plugin = IndexNames::findPluginName(key);
            if ( IndexNames::existedBefore24(plugin) )
                continue;

            const string errmsg = str::stream()
                << "Found pre-existing index " << index << " with invalid type '" << plugin << "'. "
                << "Disallowing creation of new index type '" << newPluginName << "'. See "
                << "http://dochub.mongodb.org/core/index-type-changes"
                ;

            return Status( ErrorCodes::CannotCreateIndex, errmsg );
        }

        if ( PlanExecutor::IS_EOF != state ) {
            warning() << "Internal error while reading system.indexes collection";
        }

        db->_dbEntry->markIndexSafe24AndUp( txn );

        return Status::OK();
    }

    StatusWith<BSONObj> IndexCatalog::prepareSpecForCreate( OperationContext* txn,
                                                            const BSONObj& original ) const {
        Status status = _isSpecOk( original );
        if ( !status.isOK() )
            return StatusWith<BSONObj>( status );

        BSONObj fixed = _fixIndexSpec( original );

        // we double check with new index spec
        status = _isSpecOk( fixed );
        if ( !status.isOK() )
            return StatusWith<BSONObj>( status );

        status = _doesSpecConflictWithExisting( txn, fixed );
        if ( !status.isOK() )
            return StatusWith<BSONObj>( status );

        return StatusWith<BSONObj>( fixed );
    }

namespace {
    class IndexCleanupOnRollback : public RecoveryUnit::Change {
    public:
        /**
         * None of these pointers are owned by this class.
         */
        IndexCleanupOnRollback(Collection* collection,
                               IndexCatalogEntryContainer* entries,
                               const IndexDescriptor* desc)
            : _collection(collection),
              _entries(entries),
              _desc(desc) {
        }

        virtual void commit() {}

        virtual void rollback() {
            _entries->remove(_desc);
            _collection->infoCache()->reset();
        }

    private:
        Collection* _collection;
        IndexCatalogEntryContainer* _entries;
        const IndexDescriptor* _desc;
    };
} // namespace

    Status IndexCatalog::createIndexOnEmptyCollection(OperationContext* txn, BSONObj spec) {
        txn->lockState()->assertWriteLocked( _collection->_database->name() );
        invariant(_collection->numRecords() == 0);

        _checkMagic();
        Status status = checkUnfinished();
        if ( !status.isOK() )
            return status;

        StatusWith<BSONObj> statusWithSpec = prepareSpecForCreate( txn, spec );
        status = statusWithSpec.getStatus();
        if ( !status.isOK() )
            return status;
        spec = statusWithSpec.getValue();

        string pluginName = IndexNames::findPluginName( spec["key"].Obj() );
        if ( pluginName.size() ) {
            Status s = _upgradeDatabaseMinorVersionIfNeeded( txn, pluginName );
            if ( !s.isOK() )
                return s;
        }

        // now going to touch disk
        IndexBuildBlock indexBuildBlock(txn, _collection, spec);
        status = indexBuildBlock.init();
        if ( !status.isOK() )
            return status;

        // sanity checks, etc...
        IndexCatalogEntry* entry = indexBuildBlock.getEntry();
        invariant( entry );
        IndexDescriptor* descriptor = entry->descriptor();
        invariant( descriptor );
        invariant( entry == _entries.find( descriptor ) );

        status = entry->accessMethod()->initializeAsEmpty(txn);
        if (!status.isOK())
            return status;

        txn->recoveryUnit()->registerChange(new IndexCleanupOnRollback(_collection,
                                                                       &_entries,
                                                                       entry->descriptor()));
        indexBuildBlock.success();

        // sanity check
        invariant(_collection->getCatalogEntry()->isIndexReady(descriptor->indexName()));

        return Status::OK();
    }

    IndexCatalog::IndexBuildBlock::IndexBuildBlock(OperationContext* txn,
                                                   Collection* collection,
                                                   const BSONObj& spec )
        : _collection( collection ),
          _catalog( collection->getIndexCatalog() ),
          _ns( _catalog->_collection->ns().ns() ),
          _spec( spec.getOwned() ),
          _entry( NULL ),
          _inProgress( false ),
          _txn(txn) {

        invariant( collection );
    }

    Status IndexCatalog::IndexBuildBlock::init() {
        // we do special cleanup until we're far enough in
        invariant( _inProgress == false );

        // need this first for names, etc...
        BSONObj keyPattern = _spec.getObjectField("key");
        IndexDescriptor* descriptor = new IndexDescriptor( _collection,
                                                           IndexNames::findPluginName(keyPattern),
                                                           _spec );
        auto_ptr<IndexDescriptor> descriptorCleaner( descriptor );

        _indexName = descriptor->indexName();
        _indexNamespace = descriptor->indexNamespace();

        /// ----------   setup on disk structures ----------------

        Status status = _collection->getCatalogEntry()->prepareForIndexBuild( _txn, descriptor );
        if ( !status.isOK() )
            return status;

        // at this point we can do normal clean up procedure, so we mark ourselves
        // as in progress.
        _inProgress = true;

        /// ----------   setup in memory structures  ----------------

        _entry = _catalog->_setupInMemoryStructures(_txn, descriptorCleaner.release());

        return Status::OK();
    }

    IndexCatalog::IndexBuildBlock::~IndexBuildBlock() {
        if ( !_inProgress ) {
            // taken care of already when success() is called
            return;
        }

        try {
            fail();
        }
        catch ( const AssertionException& exc ) {
            log() << "exception in ~IndexBuildBlock trying to cleanup: " << exc;
            log() << " going to fassert to preserve state";
            fassertFailed( 17345 );
        }
    }

    void IndexCatalog::IndexBuildBlock::fail() {
        try {
            fassert( 17204, _catalog->_collection->ok() ); // defensive

            _inProgress = false;

            IndexCatalogEntry* entry = _catalog->_entries.find( _indexName );
            invariant( entry == _entry );

            if ( entry ) {
                _catalog->_dropIndex(_txn, entry);
            }
            else {
                _catalog->_deleteIndexFromDisk( _txn,
                                                _indexName,
                                                _indexNamespace );
            }
        }
        catch (const DBException& exc) {
            error() << "exception while cleaning up in-progress index build: " << exc.what();
            fassertFailedWithStatus(17493, exc.toStatus());
        }
    }

    void IndexCatalog::IndexBuildBlock::abortWithoutCleanup() {
        _inProgress = false;
    }

    void IndexCatalog::IndexBuildBlock::success() {
        fassert( 17206, _inProgress );
        _inProgress = false;

        fassert( 17207, _catalog->_collection->ok() );

        _catalog->_collection->getCatalogEntry()->indexBuildSuccess( _txn, _indexName );

        _catalog->_collection->infoCache()->addedIndex();

        IndexDescriptor* desc = _catalog->findIndexByName( _indexName, true );
        fassert( 17330, desc );
        IndexCatalogEntry* entry = _catalog->_entries.find( desc );
        fassert( 17331, entry && entry == _entry );

        entry->setIsReady( true );
    }


    Status IndexCatalog::_isSpecOk( const BSONObj& spec ) const {

        const NamespaceString& nss = _collection->ns();


        if ( nss.isSystemDotIndexes() )
            return Status( ErrorCodes::CannotCreateIndex,
                           "cannot create indexes on the system.indexes collection" );

        if ( nss.isOplog() )
            return Status( ErrorCodes::CannotCreateIndex,
                           "cannot create indexes on the oplog" );

        if ( nss.coll() == "$freelist" ) {
            // this isn't really proper, but we never want it and its not an error per se
            return Status( ErrorCodes::IndexAlreadyExists, "cannot index freelist" );
        }

        StringData specNamespace = spec.getStringField("ns");
        if ( specNamespace.size() == 0 )
            return Status( ErrorCodes::CannotCreateIndex,
                           "the index spec needs a 'ns' field'" );

        if ( nss.ns() != specNamespace )
            return Status( ErrorCodes::CannotCreateIndex,
                           "the index spec ns does not match" );

        // logical name of the index
        const char *name = spec.getStringField("name");
        if ( !name[0] )
            return Status( ErrorCodes::CannotCreateIndex, "no index name specified" );

        string indexNamespace = IndexDescriptor::makeIndexNamespace( specNamespace, name );
        if ( indexNamespace.length() > NamespaceString::MaxNsLen )
            return Status( ErrorCodes::CannotCreateIndex,
                           str::stream() << "namespace name generated from index name \"" <<
                           indexNamespace << "\" is too long (127 byte max)" );

        const BSONObj key = spec.getObjectField("key");
        const Status keyStatus = validateKeyPattern(key);
        if (!keyStatus.isOK()) {
            return Status( ErrorCodes::CannotCreateIndex,
                           str::stream() << "bad index key pattern " << key << ": "
                                         << keyStatus.reason() );
        }

        if ( _collection->isCapped() && spec["dropDups"].trueValue() ) {
            return Status( ErrorCodes::CannotCreateIndex,
                           str::stream() << "Cannot create an index with dropDups=true on a "
                                         << "capped collection, as capped collections do "
                                         << "not allow document removal." );
        }

        if ( !IndexDescriptor::isIdIndexPattern( key ) ) {
            // for non _id indexes, we check to see if replication has turned off all indexes
            // we _always_ created _id index
            repl::ReplicationCoordinator* replCoord = repl::getGlobalReplicationCoordinator();
            if (replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet &&
                    !repl::getGlobalReplicationCoordinator()->buildsIndexes()) {
                // this is not exactly the right error code, but I think will make the most sense
                return Status( ErrorCodes::IndexAlreadyExists, "no indexes per repl" );
            }
        }

        return Status::OK();
    }

    Status IndexCatalog::_doesSpecConflictWithExisting( OperationContext* txn,
                                                        const BSONObj& spec ) const {
        const char *name = spec.getStringField("name");
        invariant( name[0] );

        const BSONObj key = spec.getObjectField("key");

        {
            // Check both existing and in-progress indexes (2nd param = true)
            const IndexDescriptor* desc = findIndexByName( name, true );
            if ( desc ) {
                // index already exists with same name

                if ( !desc->keyPattern().equal( key ) )
                    return Status( ErrorCodes::IndexKeySpecsConflict,
                                   str::stream() << "Trying to create an index "
                                   << "with same name " << name
                                   << " with different key spec " << key
                                   << " vs existing spec " << desc->keyPattern() );

                IndexDescriptor temp( _collection,
                                      _getAccessMethodName( txn, key ),
                                      spec );
                if ( !desc->areIndexOptionsEquivalent( &temp ) )
                    return Status( ErrorCodes::IndexOptionsConflict,
                                   str::stream() << "Index with name: " << name
                                   << " already exists with different options" );

                // Index already exists with the same options, so no need to build a new
                // one (not an error). Most likely requested by a client using ensureIndex.
                return Status( ErrorCodes::IndexAlreadyExists, name );
            }
        }

        {
            // Check both existing and in-progress indexes (2nd param = true)
            const IndexDescriptor* desc = findIndexByKeyPattern(key, true);
            if (desc) {
                LOG(2) << "index already exists with diff name " << name
                        << ' ' << key << endl;

                IndexDescriptor temp( _collection,
                                      _getAccessMethodName( txn, key ),
                                      spec );
                if ( !desc->areIndexOptionsEquivalent( &temp ) )
                    return Status( ErrorCodes::IndexOptionsConflict,
                                   str::stream() << "Index with pattern: " << key
                                   << " already exists with different options" );

                return Status( ErrorCodes::IndexAlreadyExists, name );
            }
        }

        if ( _collection->getCatalogEntry()->getTotalIndexCount() >=
             _collection->getCatalogEntry()->getMaxAllowedIndexes() ) {
            string s = str::stream() << "add index fails, too many indexes for "
                                     << _collection->ns().ns() << " key:" << key.toString();
            log() << s;
            return Status( ErrorCodes::CannotCreateIndex, s );
        }

        // Refuse to build text index if another text index exists or is in progress.
        // Collections should only have one text index.
        string pluginName = IndexNames::findPluginName( key );
        if ( pluginName == IndexNames::TEXT ) {
            vector<IndexDescriptor*> textIndexes;
            const bool includeUnfinishedIndexes = true;
            findIndexByType( IndexNames::TEXT, textIndexes, includeUnfinishedIndexes );
            if ( textIndexes.size() > 0 ) {
                return Status( ErrorCodes::CannotCreateIndex,
                               str::stream() << "only one text index per collection allowed, "
                               << "found existing text index \"" << textIndexes[0]->indexName()
                               << "\"" );
            }
        }
        return Status::OK();
    }

    BSONObj IndexCatalog::getDefaultIdIndexSpec() const {
        dassert( _idObj["_id"].type() == NumberInt );

        BSONObjBuilder b;
        b.append( "name", "_id_" );
        b.append( "ns", _collection->ns().ns() );
        b.append( "key", _idObj );
        return b.obj();
    }

    Status IndexCatalog::dropAllIndexes(OperationContext* txn,
                                        bool includingIdIndex) {

        txn->lockState()->assertWriteLocked( _collection->_database->name() );

        BackgroundOperation::assertNoBgOpInProgForNs( _collection->ns().ns() );

        // there may be pointers pointing at keys in the btree(s).  kill them.
        // TODO: can this can only clear cursors on this index?
        _collection->cursorCache()->invalidateAll( false );

        // make sure nothing in progress
        massert( 17348,
                 "cannot dropAllIndexes when index builds in progress",
                 numIndexesTotal() == numIndexesReady() );

        bool haveIdIndex = false;

        vector<string> indexNamesToDrop;
        {
            int seen = 0;
            IndexIterator ii = getIndexIterator( true );
            while ( ii.more() ) {
                seen++;
                IndexDescriptor* desc = ii.next();
                if ( desc->isIdIndex() && includingIdIndex == false ) {
                    haveIdIndex = true;
                    continue;
                }
                indexNamesToDrop.push_back( desc->indexName() );
            }
            invariant( seen == numIndexesTotal() );
        }

        for ( size_t i = 0; i < indexNamesToDrop.size(); i++ ) {
            string indexName = indexNamesToDrop[i];
            IndexDescriptor* desc = findIndexByName( indexName, true );
            invariant( desc );
            LOG(1) << "\t dropAllIndexes dropping: " << desc->toString();
            IndexCatalogEntry* entry = _entries.find( desc );
            invariant( entry );
            _dropIndex(txn, entry);
        }

        // verify state is sane post cleaning

        long long numIndexesInCollectionCatalogEntry =
            _collection->getCatalogEntry()->getTotalIndexCount();

        if ( haveIdIndex ) {
            fassert( 17324, numIndexesTotal() == 1 );
            fassert( 17325, numIndexesReady() == 1 );
            fassert( 17326, numIndexesInCollectionCatalogEntry == 1 );
            fassert( 17336, _entries.size() == 1 );
        }
        else {
            if ( numIndexesTotal() || numIndexesInCollectionCatalogEntry || _entries.size() ) {
                error() << "About to fassert - "
                        << " numIndexesTotal(): " << numIndexesTotal()
                        << " numSystemIndexesEntries: " << numIndexesInCollectionCatalogEntry
                        << " _entries.size(): " << _entries.size()
                        << " indexNamesToDrop: " << indexNamesToDrop.size()
                        << " haveIdIndex: " << haveIdIndex;
            }
            fassert( 17327, numIndexesTotal() == 0 );
            fassert( 17328, numIndexesInCollectionCatalogEntry == 0 );
            fassert( 17337, _entries.size() == 0 );
        }

        return Status::OK();
    }

    Status IndexCatalog::dropIndex(OperationContext* txn,
                                   IndexDescriptor* desc ) {

        txn->lockState()->assertWriteLocked( _collection->_database->name() );
        IndexCatalogEntry* entry = _entries.find( desc );

        if ( !entry )
            return Status( ErrorCodes::InternalError, "cannot find index to delete" );

        if ( !entry->isReady() )
            return Status( ErrorCodes::InternalError, "cannot delete not ready index" );

        BackgroundOperation::assertNoBgOpInProgForNs( _collection->ns().ns() );

        return _dropIndex(txn, entry);
    }

    Status IndexCatalog::_dropIndex(OperationContext* txn,
                                    IndexCatalogEntry* entry ) {
        /**
         * IndexState in order
         *  <db>.system.indexes
         *    NamespaceDetails
         *      <db>.system.ns
         */

        // ----- SANITY CHECKS -------------
        if ( !entry )
            return Status( ErrorCodes::BadValue, "IndexCatalog::_dropIndex passed NULL" );

        _checkMagic();
        Status status = checkUnfinished();
        if ( !status.isOK() )
            return status;

        // there may be pointers pointing at keys in the btree(s).  kill them.
        // TODO: can this can only clear cursors on this index?
        _collection->cursorCache()->invalidateAll( false );

        // wipe out stats
        _collection->infoCache()->reset();

        string indexNamespace = entry->descriptor()->indexNamespace();
        string indexName = entry->descriptor()->indexName();

        // --------- START REAL WORK ----------

        audit::logDropIndex( currentClient.get(), indexName, _collection->ns().ns() );

        _entries.remove( entry->descriptor() );
        entry = NULL;

        try {

            // ****   this is the first disk change ****
            _deleteIndexFromDisk( txn,
                                  indexName,
                                  indexNamespace );
        }
        catch ( std::exception& ) {
            // this is bad, and we don't really know state
            // going to leak to make sure things are safe

            log() << "error dropping index: " << indexNamespace
                  << " going to leak some memory to be safe";


            _collection->_database->_clearCollectionCache( indexNamespace );

            throw;
        }

        _collection->_database->_clearCollectionCache( indexNamespace );

        _checkMagic();

        return Status::OK();
    }

    void IndexCatalog::_deleteIndexFromDisk( OperationContext* txn,
                                             const string& indexName,
                                             const string& indexNamespace ) {
        Status status = _collection->getCatalogEntry()->removeIndex( txn, indexName );
        if ( status.code() == ErrorCodes::NamespaceNotFound ) {
            // this is ok, as we may be partially through index creation
        }
        else if ( !status.isOK() ) {
            warning() << "couldn't drop index " << indexName
                      << " on collection: " << _collection->ns()
                      << " because of " << status.toString();
        }
    }

    vector<BSONObj> IndexCatalog::getAndClearUnfinishedIndexes(OperationContext* txn) {
        vector<BSONObj> toReturn = _unfinishedIndexes;
        _unfinishedIndexes.clear();
        for ( size_t i = 0; i < toReturn.size(); i++ ) {
            BSONObj spec = toReturn[i];

            BSONObj keyPattern = spec.getObjectField("key");
            IndexDescriptor desc( _collection, _getAccessMethodName(txn, keyPattern), spec );

            _deleteIndexFromDisk( txn,
                                  desc.indexName(),
                                  desc.indexNamespace() );
        }
        return toReturn;
    }

    bool IndexCatalog::isMultikey( const IndexDescriptor* idx ) {
        IndexCatalogEntry* entry = _entries.find( idx );
        invariant( entry );
        return entry->isMultikey();
    }


    // ---------------------------

    int IndexCatalog::numIndexesTotal() const {
        return _collection->getCatalogEntry()->getTotalIndexCount();
    }

    int IndexCatalog::numIndexesReady() const {
        return _collection->getCatalogEntry()->getCompletedIndexCount();
    }

    bool IndexCatalog::haveIdIndex() const {
        return findIdIndex() != NULL;
    }

    IndexCatalog::IndexIterator::IndexIterator( const IndexCatalog* cat,
                                                bool includeUnfinishedIndexes )
        : _includeUnfinishedIndexes( includeUnfinishedIndexes ),
          _catalog( cat ),
          _iterator( cat->_entries.begin() ),
          _start( true ),
          _prev( NULL ),
          _next( NULL ) {
    }

    bool IndexCatalog::IndexIterator::more() {
        if ( _start ) {
            _advance();
            _start = false;
        }
        return _next != NULL;
    }

    IndexDescriptor* IndexCatalog::IndexIterator::next() {
        if ( !more() )
            return NULL;
        _prev = _next;
        _advance();
        return _prev->descriptor();
    }

    IndexAccessMethod* IndexCatalog::IndexIterator::accessMethod( IndexDescriptor* desc ) {
        invariant( desc == _prev->descriptor() );
        return _prev->accessMethod();
    }

    void IndexCatalog::IndexIterator::_advance() {
        _next = NULL;

        while ( _iterator != _catalog->_entries.end() ) {
            IndexCatalogEntry* entry = *_iterator;
            ++_iterator;

            if ( _includeUnfinishedIndexes ||
                 entry->isReady() ) {
                _next = entry;
                return;
            }
        }

    }


    IndexDescriptor* IndexCatalog::findIdIndex() const {
        IndexIterator ii = getIndexIterator( false );
        while ( ii.more() ) {
            IndexDescriptor* desc = ii.next();
            if ( desc->isIdIndex() )
                return desc;
        }
        return NULL;
    }

    IndexDescriptor* IndexCatalog::findIndexByName( const StringData& name,
                                                    bool includeUnfinishedIndexes ) const {
        IndexIterator ii = getIndexIterator( includeUnfinishedIndexes );
        while ( ii.more() ) {
            IndexDescriptor* desc = ii.next();
            if ( desc->indexName() == name )
                return desc;
        }
        return NULL;
    }

    IndexDescriptor* IndexCatalog::findIndexByKeyPattern( const BSONObj& key,
                                                          bool includeUnfinishedIndexes ) const {
        IndexIterator ii = getIndexIterator( includeUnfinishedIndexes );
        while ( ii.more() ) {
            IndexDescriptor* desc = ii.next();
            if ( desc->keyPattern() == key )
                return desc;
        }
        return NULL;
    }

    IndexDescriptor* IndexCatalog::findIndexByPrefix( const BSONObj &keyPattern,
                                                      bool requireSingleKey ) const {
        IndexDescriptor* best = NULL;

        IndexIterator ii = getIndexIterator( false );
        while ( ii.more() ) {
            IndexDescriptor* desc = ii.next();

            if ( !keyPattern.isPrefixOf( desc->keyPattern() ) )
                continue;

            if( !desc->isMultikey() )
                return desc;

            if ( !requireSingleKey )
                best = desc;
        }

        return best;
    }

    void IndexCatalog::findIndexByType( const string& type , vector<IndexDescriptor*>& matches,
                                        bool includeUnfinishedIndexes ) const {
        IndexIterator ii = getIndexIterator( includeUnfinishedIndexes );
        while ( ii.more() ) {
            IndexDescriptor* desc = ii.next();
            if ( IndexNames::findPluginName( desc->keyPattern() ) == type ) {
                matches.push_back( desc );
            }
        }
    }

    IndexAccessMethod* IndexCatalog::getIndex( const IndexDescriptor* desc ) {
        IndexCatalogEntry* entry = _entries.find( desc );
        massert( 17334, "cannot find index entry", entry );
        return entry->accessMethod();
    }

    const IndexAccessMethod* IndexCatalog::getIndex( const IndexDescriptor* desc ) const {
        return getEntry( desc )->accessMethod();
    }

    const IndexCatalogEntry* IndexCatalog::getEntry( const IndexDescriptor* desc ) const {
        const IndexCatalogEntry* entry = _entries.find( desc );
        massert( 17357, "cannot find index entry", entry );
        return entry;
    }

    // ---------------------------

    Status IndexCatalog::_indexRecord(OperationContext* txn,
                                      IndexCatalogEntry* index,
                                      const BSONObj& obj,
                                      const DiskLoc &loc ) {
        InsertDeleteOptions options;
        options.logIfError = false;

        bool isUnique =
            KeyPattern::isIdKeyPattern(index->descriptor()->keyPattern()) ||
            index->descriptor()->unique();

        options.dupsAllowed =
            repl::getGlobalReplicationCoordinator()->shouldIgnoreUniqueIndex(index->descriptor())
            || !isUnique;

        int64_t inserted;
        return index->accessMethod()->insert(txn, obj, loc, options, &inserted);
    }

    Status IndexCatalog::_unindexRecord(OperationContext* txn,
                                        IndexCatalogEntry* index,
                                        const BSONObj& obj,
                                        const DiskLoc &loc,
                                        bool logIfError) {
        InsertDeleteOptions options;
        options.logIfError = logIfError;

        int64_t removed;
        Status status = index->accessMethod()->remove(txn, obj, loc, options, &removed);

        if ( !status.isOK() ) {
            log() << "Couldn't unindex record " << obj.toString()
                  << " from collection " << _collection->ns()
                  << ". Status: " << status.toString();
        }

        return Status::OK();
    }


    void IndexCatalog::indexRecord(OperationContext* txn,
                                   const BSONObj& obj,
                                   const DiskLoc &loc ) {

        for ( IndexCatalogEntryContainer::const_iterator i = _entries.begin();
              i != _entries.end();
              ++i ) {

            IndexCatalogEntry* entry = *i;

            try {
                Status s = _indexRecord( txn, entry, obj, loc );
                uassertStatusOK( s );
            }
            catch ( AssertionException& ae ) {
                LOG(2) << "IndexCatalog::indexRecord failed: " << ae;

                for ( IndexCatalogEntryContainer::const_iterator j = _entries.begin();
                      j != _entries.end();
                      ++j ) {

                    IndexCatalogEntry* toDelete = *j;

                    try {
                        _unindexRecord(txn, toDelete, obj, loc, false);
                    }
                    catch ( DBException& e ) {
                        LOG(1) << "IndexCatalog::indexRecord rollback failed: " << e;
                    }

                    if ( toDelete == entry )
                        break;
                }

                throw;
            }
        }

    }

    void IndexCatalog::unindexRecord(OperationContext* txn,
                                     const BSONObj& obj,
                                     const DiskLoc& loc,
                                     bool noWarn) {

        for ( IndexCatalogEntryContainer::const_iterator i = _entries.begin();
              i != _entries.end();
              ++i ) {

            IndexCatalogEntry* entry = *i;

            // If it's a background index, we DO NOT want to log anything.
            bool logIfError = entry->isReady() ? !noWarn : false;
            _unindexRecord(txn, entry, obj, loc, logIfError);
        }
    }

    Status IndexCatalog::checkNoIndexConflicts( OperationContext* txn, const BSONObj &obj ) {
        IndexIterator ii = getIndexIterator( true );
        while ( ii.more() ) {
            IndexDescriptor* descriptor = ii.next();

            if ( !descriptor->unique() )
                continue;

            if (repl::getGlobalReplicationCoordinator()->shouldIgnoreUniqueIndex(descriptor))
                continue;

            IndexAccessMethod* iam = getIndex( descriptor );

            InsertDeleteOptions options;
            options.logIfError = false;
            options.dupsAllowed = false;

            UpdateTicket ticket;
            Status ret = iam->validateUpdate(txn, BSONObj(), obj, DiskLoc(), options, &ticket);
            if ( !ret.isOK() )
                return ret;
        }

        return Status::OK();
    }

    BSONObj IndexCatalog::fixIndexKey( const BSONObj& key ) {
        if ( IndexDescriptor::isIdIndexPattern( key ) ) {
            return _idObj;
        }
        if ( key["_id"].type() == Bool && key.nFields() == 1 ) {
            return _idObj;
        }
        return key;
    }

    BSONObj IndexCatalog::_fixIndexSpec( const BSONObj& spec ) {
        BSONObj o = IndexLegacy::adjustIndexSpecObject( spec );

        BSONObjBuilder b;

        int v = DefaultIndexVersionNumber;
        if( !o["v"].eoo() ) {
            double vv = o["v"].Number();
            // note (one day) we may be able to fresh build less versions than we can use
            // isASupportedIndexVersionNumber() is what we can use
            uassert(14803, str::stream() << "this version of mongod cannot build new indexes of version number " << vv, 
                    vv == 0 || vv == 1);
            v = (int) vv;
        }
        // idea is to put things we use a lot earlier
        b.append("v", v);

        if( o["unique"].trueValue() )
            b.appendBool("unique", true); // normalize to bool true in case was int 1 or something...

        BSONObj key = fixIndexKey( o["key"].Obj() );
        b.append( "key", key );

        string name = o["name"].String();
        if ( IndexDescriptor::isIdIndexPattern( key ) ) {
            name = "_id_";
        }
        b.append( "name", name );

        {
            BSONObjIterator i(o);
            while ( i.more() ) {
                BSONElement e = i.next();
                string s = e.fieldName();

                if ( s == "_id" ) {
                    // skip
                }
                else if ( s == "dropDup" ) {
                    // dropDups is silently ignored and removed from the spec as of SERVER-14710.
                }
                else if ( s == "v" || s == "unique" ||
                          s == "key" || s == "name" ) {
                    // covered above
                }
                else {
                    b.append(e);
                }
            }
        }

        return b.obj();
    }

    std::vector<BSONObj> IndexCatalog::killMatchingIndexBuilds(
            const IndexCatalog::IndexKillCriteria& criteria) {
        // This is just a no-op stub. When SERVER-14860 is resolved, this will either be filled out
        // or removed entirely.
        return std::vector<BSONObj>();
    }
}
