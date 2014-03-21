// index_catalog.cpp

/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/catalog/index_catalog.h"

#include <vector>

#include "mongo/db/audit.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/curop.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index/2d_access_method.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/index/btree_based_access_method.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/index/hash_access_method.h"
#include "mongo/db/index/haystack_access_method.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/s2_access_method.h"
#include "mongo/db/index_legacy.h"
#include "mongo/db/index_names.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/jsobjmanipulator.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/rs.h" // this is ugly
#include "mongo/db/storage/data_file.h"
#include "mongo/db/structure/catalog/namespace_details-inl.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

    static const int INDEX_CATALOG_INIT = 283711;
    static const int INDEX_CATALOG_UNINIT = 654321;

    // What's the default version of our indices?
    const int DefaultIndexVersionNumber = 1;

    const BSONObj IndexCatalog::_idObj = BSON( "_id" << 1 );

    // -------------

    IndexCatalog::IndexCatalog( Collection* collection, NamespaceDetails* details )
        : _magic(INDEX_CATALOG_UNINIT), _collection( collection ), _details( details ) {
    }

    IndexCatalog::~IndexCatalog() {
        if ( _magic != INDEX_CATALOG_UNINIT ) {
            // only do this check if we haven't been initialized
            _checkMagic();
        }
        _magic = 123456;
    }

    Status IndexCatalog::init() {

        NamespaceDetails::IndexIterator ii = _details->ii(true);
        while ( ii.more() ) {
            IndexDetails& id = ii.next();
            int idxNo = ii.pos() - 1;

            if ( idxNo >= _details->getCompletedIndexCount() ) {
                _unfinishedIndexes.push_back( id.info.obj().getOwned() );
                continue;
            }

            BSONObj ownedInfoObj = id.info.obj().getOwned();
            BSONObj keyPattern = ownedInfoObj.getObjectField("key");
            IndexDescriptor* descriptor = new IndexDescriptor( _collection,
                                                               _getAccessMethodName(keyPattern),
                                                               ownedInfoObj );
            IndexCatalogEntry* entry = _setupInMemoryStructures( descriptor );

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

    IndexCatalogEntry* IndexCatalog::_setupInMemoryStructures( IndexDescriptor* descriptor ) {
        auto_ptr<IndexDescriptor> descriptorCleanup( descriptor );

        NamespaceDetails* indexMetadata =
            _collection->_database->namespaceIndex().details( descriptor->indexNamespace() );

        massert( 17329,
                 str::stream() << "no NamespaceDetails for index: " << descriptor->toString(),
                 indexMetadata );

        auto_ptr<RecordStore> recordStore( new SimpleRecordStoreV1( descriptor->indexNamespace(),
                                                                    indexMetadata,
                                                                    _collection->getExtentManager(),
                                                                    false ) );

        auto_ptr<IndexCatalogEntry> entry( new IndexCatalogEntry( _collection,
                                                                  descriptorCleanup.release(),
                                                                  recordStore.release() ) );

        entry->init( _createAccessMethod( entry->descriptor(),
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

    Status IndexCatalog::_checkUnfinished() const {
        if ( _unfinishedIndexes.size() == 0 )
            return Status::OK();

        return Status( ErrorCodes::InternalError,
                       str::stream()
                       << "IndexCatalog has left over indexes that must be cleared"
                       << " ns: " << _collection->ns().ns() );
    }

    bool IndexCatalog::_shouldOverridePlugin(const BSONObj& keyPattern) const {
        string pluginName = IndexNames::findPluginName(keyPattern);
        bool known = IndexNames::isKnownName(pluginName);

        int majorVersion;
        int minorVersion;

        _collection->_database->getFileFormat( &majorVersion, &minorVersion );
            
        if (minorVersion == PDFILE_VERSION_MINOR_24_AND_NEWER) {
            // RulesFor24
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

    string IndexCatalog::_getAccessMethodName(const BSONObj& keyPattern) const {
        if ( _shouldOverridePlugin(keyPattern) ) {
            return "";
        }

        return IndexNames::findPluginName(keyPattern);
    }


    // ---------------------------

    Status IndexCatalog::_upgradeDatabaseMinorVersionIfNeeded( const string& newPluginName ) {

        // first check if requested index requires pdfile minor version to be bumped
        if ( IndexNames::existedBefore24(newPluginName) ) {
            return Status::OK();
        }

        Database* db = _collection->_database;

        DataFileHeader* dfh = db->getExtentManager().getFile(0)->getHeader();
        if ( dfh->versionMinor == PDFILE_VERSION_MINOR_24_AND_NEWER ) {
            return Status::OK(); // these checks have already been done
        }

        fassert(16737, dfh->versionMinor == PDFILE_VERSION_MINOR_22_AND_OLDER);

        auto_ptr<Runner> runner( InternalPlanner::collectionScan( db->_indexesName ) );

        BSONObj index;
        Runner::RunnerState state;
        while ( Runner::RUNNER_ADVANCED == (state = runner->getNext(&index, NULL)) ) {
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

        if ( Runner::RUNNER_EOF != state ) {
            warning() << "Internal error while reading system.indexes collection";
        }

        getDur().writingInt(dfh->versionMinor) = PDFILE_VERSION_MINOR_24_AND_NEWER;

        return Status::OK();
    }

    StatusWith<BSONObj> IndexCatalog::prepareSpecForCreate( const BSONObj& original ) const {
        Status status = _isSpecOk( original );
        if ( !status.isOK() )
            return StatusWith<BSONObj>( status );

        BSONObj fixed = _fixIndexSpec( original );

        // we double check with new index spec
        status = _isSpecOk( fixed );
        if ( !status.isOK() )
            return StatusWith<BSONObj>( status );

        status = _doesSpecConflictWithExisting( fixed );
        if ( !status.isOK() )
            return StatusWith<BSONObj>( status );

        return StatusWith<BSONObj>( fixed );
    }

    Status IndexCatalog::createIndex( BSONObj spec,
                                      bool mayInterrupt,
                                      ShutdownBehavior shutdownBehavior ) {
        Lock::assertWriteLocked( _collection->_database->name() );
        _checkMagic();
        Status status = _checkUnfinished();
        if ( !status.isOK() )
            return status;

        StatusWith<BSONObj> statusWithSpec = prepareSpecForCreate( spec );
        status = statusWithSpec.getStatus();
        if ( !status.isOK() )
            return status;
        spec = statusWithSpec.getValue();

        string pluginName = IndexNames::findPluginName( spec["key"].Obj() );
        if ( pluginName.size() ) {
            Status s = _upgradeDatabaseMinorVersionIfNeeded( pluginName );
            if ( !s.isOK() )
                return s;
        }

        // now going to touch disk
        IndexBuildBlock indexBuildBlock( _collection, spec );
        status = indexBuildBlock.init();
        if ( !status.isOK() )
            return status;

        // sanity checks, etc...
        IndexCatalogEntry* entry = indexBuildBlock.getEntry();
        invariant( entry );
        IndexDescriptor* descriptor = entry->descriptor();
        invariant( descriptor );

        string idxName = descriptor->indexName(); // out copy for yields, etc...

        invariant( entry == _entries.find( descriptor ) );
        invariant( _details->_catalogFindIndexByName( idxName, true ) >= 0 );

        try {
            Client& client = cc();

            _inProgressIndexes[descriptor] = &client;

            // buildAnIndex can yield.  During a yield, the Collection that owns this
            // IndexCatalog can be dropped, which means both the Collection and IndexCatalog
            // can be destructed out from under us.  The runner used by the index build will
            // throw a particular exception when it detects that this occurred.
            buildAnIndex( _collection, entry, mayInterrupt );
            indexBuildBlock.success();

            InProgressIndexesMap::iterator it = _inProgressIndexes.find(descriptor);
            _inProgressIndexes.erase(it);

            // sanity check
            int idxNo = _details->_catalogFindIndexByName( idxName, true );
            invariant( idxNo < numIndexesReady() );

            return Status::OK();
        }
        catch ( const AssertionException& exc ) {
            // At this point, *this may have been destructed, if we dropped the collection
            // while we were yielding.  indexBuildBlock will not touch an invalid _collection
            // pointer if you call abort() on it.

            log() << "index build failed." << " spec: " << spec << " error: " << exc;

            if ( shutdownBehavior == SHUTDOWN_LEAVE_DIRTY &&
                 exc.getCode() == ErrorCodes::InterruptedAtShutdown ) {
                indexBuildBlock.abort();
            }
            else if ( exc.getCode() == ErrorCodes::CursorNotFound ) {
                // The cursor was killed because the collection was dropped. No need to clean up.
                indexBuildBlock.abort();
            }
            else {
                indexBuildBlock.fail();

                InProgressIndexesMap::iterator it = _inProgressIndexes.find(descriptor);
                _inProgressIndexes.erase(it);            
            }

            ErrorCodes::Error codeToUse = ErrorCodes::fromInt( exc.getCode() );
            if ( codeToUse == ErrorCodes::UnknownError )
                return Status( ErrorCodes::InternalError, exc.what(), exc.getCode() );
            return Status( codeToUse, exc.what() );
        }
    }

    IndexCatalog::IndexBuildBlock::IndexBuildBlock( Collection* collection,
                                                    const BSONObj& spec )
        : _collection( collection ),
          _catalog( collection->getIndexCatalog() ),
          _ns( _catalog->_collection->ns().ns() ),
          _spec( spec.getOwned() ),
          _entry( NULL ),
          _inProgress( false ) {

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

        Database* db = _collection->_database;

        // 1) insert into system.indexes

        Collection* systemIndexes = db->getOrCreateCollection( db->_indexesName );
        invariant( systemIndexes );

        StatusWith<DiskLoc> systemIndexesEntry = systemIndexes->insertDocument( _spec, false );
        if ( !systemIndexesEntry.isOK() )
            return systemIndexesEntry.getStatus();

        // 2) collection's NamespaceDetails
        IndexDetails& indexDetails = _collection->details()->getNextIndexDetails( _ns.c_str() );

        try {
            getDur().writingDiskLoc( indexDetails.info ) = systemIndexesEntry.getValue();
            getDur().writingDiskLoc( indexDetails.head ).Null();
        }
        catch ( DBException& e ) {
            log() << "got exception trying to assign loc to IndexDetails" << e;
            _catalog->_removeFromSystemIndexes( descriptor->indexName() );
            return Status( ErrorCodes::InternalError, e.toString() );
        }

        int before = _collection->details()->_indexBuildsInProgress;
        try {
            getDur().writingInt( _collection->details()->_indexBuildsInProgress ) += 1;
        }
        catch ( DBException& e ) {
            log() << "got exception trying to incrementStats _indexBuildsInProgress: " << e;
            fassert( 17344, before == _collection->details()->_indexBuildsInProgress );
            _catalog->_removeFromSystemIndexes( descriptor->indexName() );
            return Status( ErrorCodes::InternalError, e.toString() );
        }

        // at this point we can do normal clean up procedure, so we mark ourselves
        // as in progress.
        _inProgress = true;

        // 3) indexes entry in .ns file
        NamespaceIndex& nsi = db->namespaceIndex();
        invariant( nsi.details( descriptor->indexNamespace() ) == NULL );
        nsi.add_ns( descriptor->indexNamespace(), DiskLoc(), false );

        // 4) system.namespaces entry index ns
        db->_addNamespaceToCatalog( descriptor->indexNamespace(), NULL );

        /// ----------   setup in memory structures  ----------------

        _entry = _catalog->_setupInMemoryStructures( descriptorCleaner.release() );

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
        if ( !_inProgress ) {
            // taken care of already when success() is called
            return;
        }

        Client::Context context( _collection->ns().ns(),
                                 _collection->_database );

        // if we're here, the index build failed or was interrupted

        _inProgress = false; // defensive
        fassert( 17204, _catalog->_collection->ok() ); // defensive

        int idxNo = _collection->details()->_catalogFindIndexByName( _indexName, true );
        fassert( 17205, idxNo >= 0 );

        IndexCatalogEntry* entry = _catalog->_entries.find( _indexName );
        invariant( entry == _entry );

        if ( entry ) {
            _catalog->_dropIndex( entry );
        }
        else {
            _catalog->_deleteIndexFromDisk( _indexName,
                                            _indexNamespace,
                                            idxNo );
        }

    }

    void IndexCatalog::IndexBuildBlock::abort() {
        _inProgress = false;
    }

    void IndexCatalog::IndexBuildBlock::success() {

        fassert( 17206, _inProgress );
        _inProgress = false;

        fassert( 17207, _catalog->_collection->ok() );

        NamespaceDetails* nsd = _collection->details();

        int idxNo = nsd->_catalogFindIndexByName( _indexName, true );
        fassert( 17202, idxNo >= 0 );

        // Make sure the newly created index is relocated to nIndexes, if it isn't already there
        if ( idxNo != nsd->getCompletedIndexCount() ) {
            log() << "switching indexes at position " << idxNo << " and "
                  << nsd->getCompletedIndexCount() << endl;

            int toIdxNo = nsd->getCompletedIndexCount();

            nsd->swapIndex( idxNo, toIdxNo );

            idxNo = nsd->getCompletedIndexCount();
        }

        getDur().writingInt( nsd->_indexBuildsInProgress ) -= 1;
        getDur().writingInt( nsd->_nIndexes ) += 1;

        _catalog->_collection->infoCache()->addedIndex();

        IndexDescriptor* desc = _catalog->findIndexByName( _indexName, true );
        fassert( 17330, desc );
        IndexCatalogEntry* entry = _catalog->_entries.find( desc );
        fassert( 17331, entry && entry == _entry );

        entry->setIsReady( true );

        IndexLegacy::postBuildHook( _catalog->_collection,
                                    _catalog->findIndexByName( _indexName )->keyPattern() );
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

        if ( _collection->ns() != specNamespace )
            return Status( ErrorCodes::CannotCreateIndex,
                           "the index spec ns does not match" );

        // logical name of the index
        const char *name = spec.getStringField("name");
        if ( !name[0] )
            return Status( ErrorCodes::CannotCreateIndex, "no index name specified" );

        string indexNamespace = IndexDetails::indexNamespaceFromObj(spec);
        if ( indexNamespace.length() > Namespace::MaxNsLen )
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

        if ( !IndexDetails::isIdIndexPattern( key ) ) {
            // for non _id indexes, we check to see if replication has turned off all indexes
            // we _always_ created _id index
            if( theReplSet && !theReplSet->buildIndexes() ) {
                // this is not exactly the right error code, but I think will make the most sense
                return Status( ErrorCodes::IndexAlreadyExists, "no indexes per repl" );
            }
        }

        return Status::OK();
    }

    Status IndexCatalog::_doesSpecConflictWithExisting( const BSONObj& spec ) const {
        const char *name = spec.getStringField("name");
        invariant( name[0] );

        const BSONObj key = spec.getObjectField("key");

        {
            // Check both existing and in-progress indexes (2nd param = true)
            const IndexDescriptor* desc = findIndexByName( name, true );
            if ( desc ) {
                // index already exists with same name

                if ( !desc->keyPattern().equal( key ) )
                    return Status( ErrorCodes::CannotCreateIndex,
                                   str::stream() << "Trying to create an index "
                                   << "with same name " << name
                                   << " with different key spec " << key
                                   << " vs existing spec " << desc->keyPattern() );

                IndexDescriptor temp( _collection,
                                      _getAccessMethodName( key ),
                                      spec );
                if ( !desc->areIndexOptionsEquivalent( &temp ) )
                    return Status( ErrorCodes::CannotCreateIndex,
                                   str::stream() << "Index with name: " << name
                                   << " already exists with different options",
                                   IndexOptionsDiffer);

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
                                      _getAccessMethodName( key ),
                                      spec );
                if ( !desc->areIndexOptionsEquivalent( &temp ) )
                    return Status( ErrorCodes::CannotCreateIndex,
                                   str::stream() << "Index with pattern: " << key
                                   << " already exists with different options",
                                   IndexOptionsDiffer );

                return Status( ErrorCodes::IndexAlreadyExists, name );
            }
        }

        if ( _details->getTotalIndexCount() >= NamespaceDetails::NIndexesMax ) {
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
                               << "\"");
            }
        }
        return Status::OK();
    }

    Status IndexCatalog::ensureHaveIdIndex() {
        if ( _details->isSystemFlagSet( NamespaceDetails::Flag_HaveIdIndex ) )
            return Status::OK();

        dassert( _idObj["_id"].type() == NumberInt );

        BSONObjBuilder b;
        b.append( "name", "_id_" );
        b.append( "ns", _collection->ns().ns() );
        b.append( "key", _idObj );
        BSONObj o = b.done();

        Status s = createIndex( o, false );
        if ( s.isOK() || s.code() == ErrorCodes::IndexAlreadyExists ) {
            _details->setSystemFlag( NamespaceDetails::Flag_HaveIdIndex );
            return Status::OK();
        }

        return s;
    }

    Status IndexCatalog::dropAllIndexes( bool includingIdIndex ) {
        Lock::assertWriteLocked( _collection->_database->name() );

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
            _dropIndex( entry );
        }

        // verify state is sane post cleaning

        long long numSystemIndexesEntries = 0;
        {
            Collection* systemIndexes =
                _collection->_database->getCollection( _collection->_database->_indexesName );
            if ( systemIndexes ) {
                EqualityMatchExpression expr;
                BSONObj nsBSON = BSON( "ns" << _collection->ns() );
                invariant( expr.init( "ns", nsBSON.firstElement() ).isOK() );
                numSystemIndexesEntries = systemIndexes->countTableScan( &expr );
            }
            else {
                // this is ok, 0 is the right number
            }
        }

        if ( haveIdIndex ) {
            fassert( 17324, numIndexesTotal() == 1 );
            fassert( 17325, numIndexesReady() == 1 );
            fassert( 17326, numSystemIndexesEntries == 1 );
            fassert( 17336, _entries.size() == 1 );
        }
        else {
            if ( numIndexesTotal() || numSystemIndexesEntries || _entries.size() ) {
                error() << "about to fassert - "
                        << " numIndexesTotal(): " << numIndexesTotal()
                        << " numSystemIndexesEntries: " << numSystemIndexesEntries
                        << " _entries.size(): " << _entries.size()
                        << " indexNamesToDrop: " << indexNamesToDrop.size()
                        << " haveIdIndex: " << haveIdIndex;
            }
            fassert( 17327, numIndexesTotal() == 0 );
            fassert( 17328, numSystemIndexesEntries == 0 );
            fassert( 17337, _entries.size() == 0 );
        }

        return Status::OK();
    }

    Status IndexCatalog::dropIndex( IndexDescriptor* desc ) {
        Lock::assertWriteLocked( _collection->_database->name() );
        IndexCatalogEntry* entry = _entries.find( desc );
        if ( !entry )
            return Status( ErrorCodes::InternalError, "cannot find index to delete" );
        if ( !entry->isReady() )
            return Status( ErrorCodes::InternalError, "cannot delete not ready index" );
        return _dropIndex( entry );
    }

    Status IndexCatalog::_dropIndex( IndexCatalogEntry* entry ) {
        /**
         * IndexState in order
         *  <db>.system.indexes
         *    NamespaceDetails
         *      <db>.system.ns
         */

        // ----- SANITY CHECKS -------------
        if ( !entry )
            return Status( ErrorCodes::BadValue, "IndexCatalog::_dropIndex passed NULL" );

        BackgroundOperation::assertNoBgOpInProgForNs( _collection->ns().ns() );
        _checkMagic();
        Status status = _checkUnfinished();
        if ( !status.isOK() )
            return status;

        // there may be pointers pointing at keys in the btree(s).  kill them.
        // TODO: can this can only clear cursors on this index?
        _collection->cursorCache()->invalidateAll( false );

        // wipe out stats
        _collection->infoCache()->reset();

        string indexNamespace = entry->descriptor()->indexNamespace();
        string indexName = entry->descriptor()->indexName();

        int idxNo = _details->_catalogFindIndexByName( indexName, true );
        invariant( idxNo >= 0 );

        // --------- START REAL WORK ----------

        audit::logDropIndex( currentClient.get(), indexName, _collection->ns().ns() );

        _entries.remove( entry->descriptor() );
        entry = NULL;

        try {
            _details->clearSystemFlag( NamespaceDetails::Flag_HaveIdIndex );

            // ****   this is the first disk change ****
            _deleteIndexFromDisk( indexName,
                                  indexNamespace,
                                  idxNo );
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

    void IndexCatalog::_deleteIndexFromDisk( const string& indexName,
                                             const string& indexNamespace,
                                             int idxNo ) {
        invariant( idxNo >= 0 );
        invariant( _details->_catalogFindIndexByName( indexName, true ) == idxNo );

        // data + system.namespacesa
        Status status = _collection->_database->_dropNS( indexNamespace );
        if ( status.code() == ErrorCodes::NamespaceNotFound ) {
            // this is ok, as we may be partially through index creation
        }
        else if ( !status.isOK() ) {
            warning() << "couldn't drop extents for " << indexNamespace << " " << status.toString();
        }

        // all info in the .ns file
        _details->_removeIndexFromMe( idxNo );

        // remove from system.indexes
        // n is how many things were removed from this
        // probably should clean this up
        int n = _removeFromSystemIndexes( indexName );
        wassert( n == 1 );
    }

    int IndexCatalog::_removeFromSystemIndexes( const StringData& indexName ) {
        BSONObjBuilder b;
        b.append( "ns", _collection->ns() );
        b.append( "name", indexName );
        BSONObj cond = b.obj(); // e.g.: { name: "ts_1", ns: "foo.coll" }
        return static_cast<int>( deleteObjects( _collection->_database->_indexesName,
                                                cond,
                                                false,
                                                false,
                                                true ) );
    }

    vector<BSONObj> IndexCatalog::getAndClearUnfinishedIndexes() {
        vector<BSONObj> toReturn = _unfinishedIndexes;
        _unfinishedIndexes.clear();
        for ( size_t i = 0; i < toReturn.size(); i++ ) {
            BSONObj spec = toReturn[i];

            BSONObj keyPattern = spec.getObjectField("key");
            IndexDescriptor desc( _collection, _getAccessMethodName(keyPattern), spec );

            int idxNo = _details->_catalogFindIndexByName( desc.indexName(), true );
            invariant( idxNo >= 0 );
            invariant( idxNo >= numIndexesReady() );

            _deleteIndexFromDisk( desc.indexName(),
                                  desc.indexNamespace(),
                                  idxNo );
        }
        return toReturn;
    }

    void IndexCatalog::updateTTLSetting( const IndexDescriptor* idx, long long newExpireSeconds ) {
        IndexDetails* indexDetails = _getIndexDetails( idx );

        BSONElement oldExpireSecs = indexDetails->info.obj().getField("expireAfterSeconds");

        // Important that we set the new value in-place.  We are writing directly to the
        // object here so must be careful not to overwrite with a longer numeric type.

        BSONElementManipulator manip( oldExpireSecs );
        switch( oldExpireSecs.type() ) {
        case EOO:
            massert( 16631, "index does not have an 'expireAfterSeconds' field", false );
            break;
        case NumberInt:
            manip.SetInt( static_cast<int>( newExpireSeconds ) );
            break;
        case NumberDouble:
            manip.SetNumber( static_cast<double>( newExpireSeconds ) );
            break;
        case NumberLong:
            manip.SetLong( newExpireSeconds );
            break;
        default:
            massert( 16632, "current 'expireAfterSeconds' is not a number", false );
        }
    }

    bool IndexCatalog::isMultikey( const IndexDescriptor* idx ) {
        IndexCatalogEntry* entry = _entries.find( idx );
        invariant( entry );
        return entry->isMultikey();
    }


    // ---------------------------

    int IndexCatalog::numIndexesTotal() const {
        return _details->getTotalIndexCount();
    }

    int IndexCatalog::numIndexesReady() const {
        return _details->getCompletedIndexCount();
    }

    bool IndexCatalog::haveIdIndex() const {
        return _details->isSystemFlagSet( NamespaceDetails::Flag_HaveIdIndex )
            || findIdIndex() != NULL;
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
        const IndexCatalogEntry* entry = _entries.find( desc );
        massert( 17357, "cannot find index entry", entry );
        return entry->accessMethod();
    }

    IndexAccessMethod* IndexCatalog::_createAccessMethod( const IndexDescriptor* desc,
                                                          IndexCatalogEntry* entry ) {
        const string& type = desc->getAccessMethodName();

        if (IndexNames::HASHED == type)
            return new HashAccessMethod( entry );

        if (IndexNames::GEO_2DSPHERE == type)
            return new S2AccessMethod( entry );

        if (IndexNames::TEXT == type)
            return new FTSAccessMethod( entry );

        if (IndexNames::GEO_HAYSTACK == type)
            return new HaystackAccessMethod( entry );

        if ("" == type)
            return new BtreeAccessMethod( entry );

        if (IndexNames::GEO_2D == type)
            return new TwoDAccessMethod( entry );

        log() << "Can't find index for keypattern " << desc->keyPattern();
        invariant(0);
        return NULL;
    }

    IndexDetails* IndexCatalog::_getIndexDetails( const IndexDescriptor* descriptor ) const {
        int idxNo = _details->_catalogFindIndexByName( descriptor->indexName(), true );
        invariant( idxNo >= 0 );
        return &_details->idx( idxNo );
    }

    // ---------------------------

    Status IndexCatalog::_indexRecord( IndexCatalogEntry* index,
                                       const BSONObj& obj,
                                       const DiskLoc &loc ) {
        InsertDeleteOptions options;
        options.logIfError = false;

        bool isUnique =
            KeyPattern::isIdKeyPattern(index->descriptor()->keyPattern()) ||
            index->descriptor()->unique();

        options.dupsAllowed = ignoreUniqueIndex( index->descriptor() ) || !isUnique;

        int64_t inserted;
        return index->accessMethod()->insert(obj, loc, options, &inserted);
    }

    Status IndexCatalog::_unindexRecord( IndexCatalogEntry* index,
                                         const BSONObj& obj,
                                         const DiskLoc &loc,
                                         bool logIfError ) {
        InsertDeleteOptions options;
        options.logIfError = logIfError;

        int64_t removed;
        Status status = index->accessMethod()->remove(obj, loc, options, &removed);

        if ( !status.isOK() ) {
            problem() << "Couldn't unindex record " << obj.toString()
                      << " status: " << status.toString();
        }

        return Status::OK();
    }


    void IndexCatalog::indexRecord( const BSONObj& obj, const DiskLoc &loc ) {

        for ( IndexCatalogEntryContainer::const_iterator i = _entries.begin();
              i != _entries.end();
              ++i ) {

            IndexCatalogEntry* entry = *i;

            try {
                Status s = _indexRecord( entry, obj, loc );
                uassert(s.location(), s.reason(), s.isOK() );
            }
            catch ( AssertionException& ae ) {

                LOG(2) << "IndexCatalog::indexRecord failed: " << ae;

                for ( IndexCatalogEntryContainer::const_iterator j = _entries.begin();
                      j != _entries.end();
                      ++j ) {

                    IndexCatalogEntry* toDelete = *j;

                    try {
                        _unindexRecord( toDelete, obj, loc, false );
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

    void IndexCatalog::unindexRecord( const BSONObj& obj, const DiskLoc& loc, bool noWarn ) {
        for ( IndexCatalogEntryContainer::const_iterator i = _entries.begin();
              i != _entries.end();
              ++i ) {

            IndexCatalogEntry* entry = *i;

            // If it's a background index, we DO NOT want to log anything.
            bool logIfError = entry->isReady() ? !noWarn : false;
            _unindexRecord( entry, obj, loc, logIfError );
        }

    }

    Status IndexCatalog::checkNoIndexConflicts( const BSONObj &obj ) {
        IndexIterator ii = getIndexIterator( true );
        while ( ii.more() ) {
            IndexDescriptor* descriptor = ii.next();

            if ( !descriptor->unique() )
                continue;

            if ( ignoreUniqueIndex(descriptor) )
                continue;

            IndexAccessMethod* iam = getIndex( descriptor );

            InsertDeleteOptions options;
            options.logIfError = false;
            options.dupsAllowed = false;

            UpdateTicket ticket;
            Status ret = iam->validateUpdate(BSONObj(), obj, DiskLoc(), options, &ticket);
            if ( !ret.isOK() )
                return ret;
        }

        return Status::OK();
    }

    BSONObj IndexCatalog::fixIndexKey( const BSONObj& key ) {
        if ( IndexDetails::isIdIndexPattern( key ) ) {
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
        if ( IndexDetails::isIdIndexPattern( key ) ) {
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
                else if ( s == "v" || s == "unique" ||
                          s == "key" || s == "name" ) {
                    // covered above
                }
                else if ( s == "key" ) {
                    b.append( "key", fixIndexKey( e.Obj() ) );
                }
                else {
                    b.append(e);
                }
            }
        }

        return b.obj();
    }

    std::vector<BSONObj> 
    IndexCatalog::killMatchingIndexBuilds(const IndexCatalog::IndexKillCriteria& criteria) {
        verify(Lock::somethingWriteLocked());
        std::vector<BSONObj> indexes;
        for (InProgressIndexesMap::iterator it = _inProgressIndexes.begin();
             it != _inProgressIndexes.end();
             it++) {
            // check criteria
            IndexDescriptor* desc = it->first;
            Client* client = it->second;
            if (!criteria.ns.empty() && (desc->parentNS() != criteria.ns)) {
                continue;
            }
            if (!criteria.name.empty() && (desc->indexName() != criteria.name)) {
                continue;
            }
            if (!criteria.key.isEmpty() && (desc->keyPattern() != criteria.key)) {
                continue;
            }
            indexes.push_back(desc->keyPattern());
            CurOp* op = client->curop();
            log() << "halting index build: " << desc->keyPattern();
            // Note that we can only be here if the background index build in question is
            // yielding.  The bg index code is set up specially to check for interrupt
            // immediately after it recovers from yield, such that no further work is done
            // on the index build.  Thus this thread does not have to synchronize with the
            // bg index operation; we can just assume that it is safe to proceed.
            killCurrentOp.kill(op->opNum());
        }

        if (indexes.size() > 0) {
            log() << "halted " << indexes.size() << " index build(s)" << endl;
        }

        return indexes;
    }
}
