// mmap_v1_database_catalog_entry.cpp

/**
*    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/storage/mmap_v1/mmap_v1_database_catalog_entry.h"

#include <utility>

#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/index/2d_access_method.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/index/btree_based_access_method.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/index/hash_access_method.h"
#include "mongo/db/index/haystack_access_method.h"
#include "mongo/db/index/s2_access_method.h"
#include "mongo/db/pdfile_version.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/storage/mmap_v1/btree/btree_interface.h"
#include "mongo/db/storage/mmap_v1/catalog/namespace_details.h"
#include "mongo/db/storage/mmap_v1/catalog/namespace_details_collection_entry.h"
#include "mongo/db/storage/mmap_v1/catalog/namespace_details_rsv1_metadata.h"
#include "mongo/db/storage/mmap_v1/data_file.h"
#include "mongo/db/storage/mmap_v1/dur_recovery_unit.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_capped.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_simple.h"
#include "mongo/util/log.h"

namespace mongo {

    MONGO_EXPORT_SERVER_PARAMETER(newCollectionsUsePowerOf2Sizes, bool, true);

    /**
     * Registers the insertion of a new entry in the _collections cache with the RecoveryUnit,
     * allowing for rollback.
     */
    class MMAPV1DatabaseCatalogEntry::EntryInsertion : public RecoveryUnit::Change {
    public:
        EntryInsertion(const StringData& ns, MMAPV1DatabaseCatalogEntry* entry)
            : _ns(ns.toString()), _entry(entry) { }

        void rollback() {
            _entry->_removeFromCache(NULL, _ns);
        }

        void commit() { }
    private:
        std::string _ns;
        MMAPV1DatabaseCatalogEntry* const _entry;
    };

    /**
     * Registers the removal of an entry from the _collections cache with the RecoveryUnit,
     * delaying actual deletion of the information until the change is commited. This allows
     * for easy rollback.
     */
    class MMAPV1DatabaseCatalogEntry::EntryRemoval : public RecoveryUnit::Change {
    public:
        //  Rollback removing the collection from the cache. Takes ownership of the cachedEntry,
        //  and will delete it if removal is final.
        EntryRemoval(const StringData& ns,
                     MMAPV1DatabaseCatalogEntry* catalogEntry,
                     Entry *cachedEntry)
            : _ns(ns.toString()), _catalogEntry(catalogEntry), _cachedEntry(cachedEntry) { }

        void rollback() {
            boost::mutex::scoped_lock lk(_catalogEntry->_collectionsLock);
            _catalogEntry->_collections[_ns] = _cachedEntry;
        }

        void commit() {
            delete _cachedEntry;
        }

    private:
        std::string _ns;
        MMAPV1DatabaseCatalogEntry* const _catalogEntry;
        Entry* const _cachedEntry;
    };

    MMAPV1DatabaseCatalogEntry::MMAPV1DatabaseCatalogEntry( OperationContext* txn,
                                                            const StringData& name,
                                                            const StringData& path,
                                                            bool directoryPerDB,
                                                            bool transient )
        : DatabaseCatalogEntry( name ),
          _path( path.toString() ),
          _extentManager( name, path, directoryPerDB ),
          _namespaceIndex( _path, name.toString() ) {

        try {
            Status s = _extentManager.init(txn);
            if ( !s.isOK() ) {
                msgasserted( 16966, str::stream() << "_extentManager.init failed: " << s.toString() );
            }

            // If already exists, open.  Otherwise behave as if empty until
            // there's a write, then open.

            if ( _namespaceIndex.pathExists() ) {
                _namespaceIndex.init( txn );

                // upgrade freelist
                NamespaceString oldFreeList( name, "$freelist" );
                NamespaceDetails* details = _namespaceIndex.details( oldFreeList.ns() );
                if ( details ) {
                    if ( !details->firstExtent.isNull() ) {
                        _extentManager.freeExtents(txn,
                                                   details->firstExtent,
                                                   details->lastExtent);
                    }
                    _namespaceIndex.kill_ns( txn, oldFreeList.ns() );
                }

                _lazyInit( txn );

                std::list<std::string> namespaces;
                _namespaceIndex.getCollectionNamespaces( &namespaces );
                for ( std::list<std::string>::const_iterator i = namespaces.begin();
                      i != namespaces.end(); // we add to the list in the loop so can't cache end().
                      ++i ) {
                    const std::string& ns = *i;
                    Entry*& entry = _collections[ns];
                    // entry was already loaded for system.indexes and system.namespaces in
                    // _lazyInit. That is ok, since they can't have indexes on them anyway.
                    if (!entry) {
                        entry = new Entry();
                        _insertInCache_inlock(txn, ns, entry);

                        // Add the indexes on this namespace to the list of namespaces to load.
                        std::vector<std::string> indexNames;
                        entry->catalogEntry->getAllIndexes(txn, &indexNames);
                        for (size_t i = 0; i < indexNames.size(); i++) {
                            namespaces.push_back(
                                IndexDescriptor::makeIndexNamespace(ns, indexNames[i]));
                        }
                    }
                }
            }
        }
        catch(std::exception& e) {
            log() << "warning database " << path << " " << name << " could not be opened";
            DBException* dbe = dynamic_cast<DBException*>(&e);
            if ( dbe != 0 ) {
                log() << "DBException " << dbe->getCode() << ": " << e.what() << endl;
            }
            else {
                log() << e.what() << endl;
            }
            _extentManager.reset();
            throw;
        }
    }

    MMAPV1DatabaseCatalogEntry::~MMAPV1DatabaseCatalogEntry() {
        for ( CollectionMap::const_iterator i = _collections.begin();
              i != _collections.end();
              ++i ) {
            delete i->second;
        }
        _collections.clear();
    }

    void MMAPV1DatabaseCatalogEntry::_removeFromCache(RecoveryUnit* ru,
                                                      const StringData& ns) {
        boost::mutex::scoped_lock lk(_collectionsLock);
        CollectionMap::iterator i = _collections.find(ns.toString());
        if ( i == _collections.end() )
            return;

        //  If there is an operation context, register a rollback to restore the cache entry
        if (ru) {
            ru->registerChange(new EntryRemoval(ns, this, i->second));
        }
        else {
            delete i->second;
        }
        _collections.erase(i);
    }

    Status MMAPV1DatabaseCatalogEntry::dropCollection(OperationContext* txn, const StringData& ns) {
        invariant(txn->lockState()->isWriteLocked(ns));
        _removeFromCache(txn->recoveryUnit(), ns);

        NamespaceDetails* details = _namespaceIndex.details( ns );

        if ( !details ) {
            return Status( ErrorCodes::NamespaceNotFound, str::stream() << "ns not found: " << ns );
        }

        invariant( details->nIndexes == 0 ); // TODO: delete instead?
        invariant( details->indexBuildsInProgress == 0 ); // TODO: delete instead?

        _removeNamespaceFromNamespaceCollection( txn, ns );

        // free extents
        if( !details->firstExtent.isNull() ) {
            _extentManager.freeExtents(txn, details->firstExtent, details->lastExtent);
            *txn->recoveryUnit()->writing( &details->firstExtent ) = DiskLoc().setInvalid();
            *txn->recoveryUnit()->writing( &details->lastExtent ) = DiskLoc().setInvalid();
        }

        // remove from the catalog hashtable
        _namespaceIndex.kill_ns( txn, ns );

        return Status::OK();
    }


    Status MMAPV1DatabaseCatalogEntry::renameCollection( OperationContext* txn,
                                                        const StringData& fromNS,
                                                        const StringData& toNS,
                                                        bool stayTemp ) {
        Status s = _renameSingleNamespace( txn, fromNS, toNS, stayTemp );
        if ( !s.isOK() )
            return s;

        NamespaceDetails* details = _namespaceIndex.details( toNS );
        invariant( details );

        RecordStoreV1Base* systemIndexRecordStore = _getIndexRecordStore();
        scoped_ptr<RecordIterator> it( systemIndexRecordStore->getIterator(txn) );

        while ( !it->isEOF() ) {
            DiskLoc loc = it->getNext();
            BSONObj oldIndexSpec = it->dataFor( loc ).toBson();
            if ( fromNS != oldIndexSpec["ns"].valuestrsafe() )
                continue;

            BSONObj newIndexSpec;
            {
                BSONObjBuilder b;
                BSONObjIterator i( oldIndexSpec );
                while( i.more() ) {
                    BSONElement e = i.next();
                    if ( strcmp( e.fieldName(), "ns" ) != 0 )
                        b.append( e );
                    else
                        b << "ns" << toNS;
                }
                newIndexSpec = b.obj();
            }

            StatusWith<DiskLoc> newIndexSpecLoc =
                systemIndexRecordStore->insertRecord( txn,
                                                      newIndexSpec.objdata(),
                                                      newIndexSpec.objsize(),
                                                      false );
            if ( !newIndexSpecLoc.isOK() )
                return newIndexSpecLoc.getStatus();

            const string& indexName = oldIndexSpec.getStringField( "name" );

            {
                // fix IndexDetails pointer
                NamespaceDetailsCollectionCatalogEntry ce( toNS,
                                                           details,
                                                           systemIndexRecordStore,
                                                           this );
                int indexI = ce._findIndexNumber( txn, indexName );

                IndexDetails& indexDetails = details->idx(indexI);
                *txn->recoveryUnit()->writing(&indexDetails.info) = newIndexSpecLoc.getValue(); // XXX: dur
            }

            {
                // move underlying namespac
                string oldIndexNs = IndexDescriptor::makeIndexNamespace( fromNS, indexName );
                string newIndexNs = IndexDescriptor::makeIndexNamespace( toNS, indexName );

                Status s = _renameSingleNamespace( txn, oldIndexNs, newIndexNs, false );
                if ( !s.isOK() )
                    return s;
            }

            systemIndexRecordStore->deleteRecord( txn, loc );
        }

        return Status::OK();
    }

    Status MMAPV1DatabaseCatalogEntry::_renameSingleNamespace( OperationContext* txn,
                                                              const StringData& fromNS,
                                                              const StringData& toNS,
                                                              bool stayTemp ) {
        // some sanity checking
        NamespaceDetails* fromDetails = _namespaceIndex.details( fromNS );
        if ( !fromDetails )
            return Status( ErrorCodes::BadValue, "from namespace doesn't exist" );

        if ( _namespaceIndex.details( toNS ) )
            return Status( ErrorCodes::BadValue, "to namespace already exists" );

        _removeFromCache(txn->recoveryUnit(), fromNS);

        // at this point, we haven't done anything destructive yet

        // ----
        // actually start moving
        // ----

        // this could throw, but if it does we're ok
        _namespaceIndex.add_ns( txn, toNS, fromDetails );
        NamespaceDetails* toDetails = _namespaceIndex.details( toNS );

        try {
            toDetails->copyingFrom(txn,
                                   toNS,
                                   _namespaceIndex,
                                   fromDetails); // fixes extraOffset
        }
        catch( DBException& ) {
            // could end up here if .ns is full - if so try to clean up / roll back a little
            _namespaceIndex.kill_ns( txn, toNS );
            throw;
        }

        // at this point, code .ns stuff moved

        _namespaceIndex.kill_ns( txn, fromNS );
        fromDetails = NULL;

        // fix system.namespaces
        BSONObj newSpec;
        DiskLoc oldSpecLocation;
        {

            BSONObj oldSpec;
            {
                RecordStoreV1Base* rs = _getNamespaceRecordStore();
                scoped_ptr<RecordIterator> it( rs->getIterator(txn) );
                while ( !it->isEOF() ) {
                    DiskLoc loc = it->getNext();
                    BSONObj entry = it->dataFor( loc ).toBson();
                    if ( fromNS == entry["name"].String() ) {
                        oldSpecLocation = loc;
                        oldSpec = entry.getOwned();
                        break;
                    }
                }
            }
            invariant( !oldSpec.isEmpty() );
            invariant( !oldSpecLocation.isNull() );

            BSONObjBuilder b;
            BSONObjIterator i( oldSpec.getObjectField( "options" ) );
            while( i.more() ) {
                BSONElement e = i.next();
                if ( strcmp( e.fieldName(), "create" ) != 0 ) {
                    if (stayTemp || (strcmp(e.fieldName(), "temp") != 0))
                        b.append( e );
                }
                else {
                    b << "create" << toNS;
                }
            }
            newSpec = b.obj();
        }

        _addNamespaceToNamespaceCollection( txn, toNS, newSpec.isEmpty() ? 0 : &newSpec );

        _getNamespaceRecordStore()->deleteRecord( txn, oldSpecLocation );

        boost::mutex::scoped_lock lk( _collectionsLock );
        Entry*& entry = _collections[toNS.toString()];
        invariant( entry == NULL );
        txn->recoveryUnit()->registerChange(new EntryInsertion(toNS, this));
        entry = new Entry();
        _insertInCache_inlock(txn, toNS, entry);

        return Status::OK();
    }

    void MMAPV1DatabaseCatalogEntry::appendExtraStats( OperationContext* opCtx,
                                                      BSONObjBuilder* output,
                                                      double scale ) const {
        if ( isEmpty() ) {
            output->appendNumber( "fileSize", 0 );
        }
        else {
            output->appendNumber( "fileSize", _extentManager.fileSize() / scale );
            output->appendNumber( "nsSizeMB", static_cast<int>( _namespaceIndex.fileLength() /
                                                                ( 1024 * 1024 ) ) );

            int freeListSize = 0;
            int64_t freeListSpace = 0;
            _extentManager.freeListStats( &freeListSize, &freeListSpace );

            BSONObjBuilder extentFreeList( output->subobjStart( "extentFreeList" ) );
            extentFreeList.append( "num", freeListSize );
            extentFreeList.appendNumber( "totalSize",
                                         static_cast<long long>( freeListSpace / scale ) );
            extentFreeList.done();

            {

                int major = 0;
                int minor = 0;
                _extentManager.getFileFormat( opCtx, &major, &minor );

                BSONObjBuilder dataFileVersion( output->subobjStart( "dataFileVersion" ) );
                dataFileVersion.append( "major", major );
                dataFileVersion.append( "minor", minor );
                dataFileVersion.done();
            }
        }

    }

    bool MMAPV1DatabaseCatalogEntry::isOlderThan24( OperationContext* opCtx ) const {
        if ( _extentManager.numFiles() == 0 )
            return false;

        int major = 0;
        int minor = 0;

        _extentManager.getFileFormat( opCtx, &major, &minor );

        invariant( major == PDFILE_VERSION );

        return minor == PDFILE_VERSION_MINOR_22_AND_OLDER;
    }

    void MMAPV1DatabaseCatalogEntry::markIndexSafe24AndUp( OperationContext* opCtx ) {
        if ( _extentManager.numFiles() == 0 )
            return;

        int major = 0;
        int minor = 0;

        _extentManager.getFileFormat( opCtx, &major, &minor );

        invariant( major == PDFILE_VERSION );

        if ( minor == PDFILE_VERSION_MINOR_24_AND_NEWER )
            return;

        invariant( minor == PDFILE_VERSION_MINOR_22_AND_OLDER );

        DataFile* df = _extentManager.getFile( opCtx, 0 );
        opCtx->recoveryUnit()->writingInt(df->getHeader()->versionMinor) =
            PDFILE_VERSION_MINOR_24_AND_NEWER;
    }

    bool MMAPV1DatabaseCatalogEntry::currentFilesCompatible( OperationContext* opCtx ) const {
        if ( _extentManager.numFiles() == 0 )
            return true;

        return _extentManager.getOpenFile( 0 )->getHeader()->isCurrentVersion();
    }

    void MMAPV1DatabaseCatalogEntry::getCollectionNamespaces( std::list<std::string>* tofill ) const {
        _namespaceIndex.getCollectionNamespaces( tofill );
    }

    namespace {
        int _massageExtentSize( const ExtentManager* em, long long size ) {
            if ( size < em->minSize() )
                return em->minSize();
            if ( size > em->maxSize() )
                return em->maxSize();
            return static_cast<int>( size );
        }
    }

    void MMAPV1DatabaseCatalogEntry::_ensureSystemCollection_inlock( OperationContext* txn,
                                                                     const StringData& ns ) {
        NamespaceDetails* details = _namespaceIndex.details( ns );
        if ( details ) {
            return;
        }
        _namespaceIndex.add_ns( txn, ns, DiskLoc(), false );
    }

    void MMAPV1DatabaseCatalogEntry::_lazyInit( OperationContext* txn ) {
        // this is sort of insane
        // it's because the whole storage/mmap_v1 is highly recursive

        _namespaceIndex.init( txn );

        NamespaceString nsi( name(), "system.indexes" );
        NamespaceString nsn( name(), "system.namespaces" );

        boost::mutex::scoped_lock lk( _collectionsLock );

        bool isSystemNamespacesGoingToBeNew = _namespaceIndex.details( nsn.toString() ) == NULL;
        bool isSystemIndexesGoingToBeNew = _namespaceIndex.details( nsi.toString() ) == NULL;

        _ensureSystemCollection_inlock( txn, nsn.toString() );
        _ensureSystemCollection_inlock( txn, nsi.toString() );

        if ( isSystemNamespacesGoingToBeNew ) {
            txn->recoveryUnit()->registerChange(new EntryInsertion(nsn.toString(), this));
        }
        if ( isSystemIndexesGoingToBeNew ) {
            txn->recoveryUnit()->registerChange(new EntryInsertion(nsi.toString(), this));
        }
        Entry*& indexEntry = _collections[nsi.toString()];
        Entry*& nsEntry = _collections[nsn.toString()];

        NamespaceDetails* indexDetails = _namespaceIndex.details( nsi.toString() );
        NamespaceDetails* nsDetails = _namespaceIndex.details( nsn.toString() );

        // order has to be:
        // 1) ns rs
        // 2) i rs
        // 3) catalog entries

        if ( !nsEntry ) {
            nsEntry = new Entry();

            NamespaceDetailsRSV1MetaData* md = new NamespaceDetailsRSV1MetaData( nsn.toString(),
                                                                                 nsDetails,
                                                                                 NULL );
            nsEntry->recordStore.reset( new SimpleRecordStoreV1( txn,
                                                                 nsn.toString(),
                                                                 md,
                                                                 &_extentManager,
                                                                 false ) );

            if ( nsEntry->recordStore->storageSize( txn ) == 0 )
                nsEntry->recordStore->increaseStorageSize( txn, _extentManager.initialSize( 128 ), false );
        }

        if ( !indexEntry ) {
            indexEntry = new Entry();

            NamespaceDetailsRSV1MetaData* md = new NamespaceDetailsRSV1MetaData( nsi.toString(),
                                                                                 indexDetails,
                                                                                 nsEntry->recordStore.get() );
            indexEntry->recordStore.reset( new SimpleRecordStoreV1( txn,
                                                                    nsi.toString(),
                                                                    md,
                                                                    &_extentManager,
                                                                    true ) );

            if ( indexEntry->recordStore->storageSize( txn ) == 0 )
                indexEntry->recordStore->increaseStorageSize( txn, _extentManager.initialSize( 128 ), false );
        }

        if ( isSystemIndexesGoingToBeNew ) {
            _addNamespaceToNamespaceCollection_inlock( txn, nsi.toString(), NULL );
        }

        if ( !nsEntry->catalogEntry )
            nsEntry->catalogEntry.reset( new NamespaceDetailsCollectionCatalogEntry( nsn.toString(),
                                                                                     nsDetails,
                                                                                     indexEntry->recordStore.get(),
                                                                                     this ) );

        if ( !indexEntry->catalogEntry )
            indexEntry->catalogEntry.reset( new NamespaceDetailsCollectionCatalogEntry( nsi.toString(),
                                                                                        indexDetails,
                                                                                        indexEntry->recordStore.get(),
                                                                                        this ) );
    }

    Status MMAPV1DatabaseCatalogEntry::createCollection( OperationContext* txn,
                                                        const StringData& ns,
                                                        const CollectionOptions& options,
                                                        bool allocateDefaultSpace ) {
        _lazyInit( txn );

        if ( _namespaceIndex.details( ns ) ) {
            return Status( ErrorCodes::NamespaceExists,
                           str::stream() << "namespace already exists: " << ns );
        }

        BSONObj optionsAsBSON = options.toBSON();
        _addNamespaceToNamespaceCollection( txn, ns, &optionsAsBSON );

        _namespaceIndex.add_ns( txn, ns, DiskLoc(), options.capped );

        // allocation strategy set explicitly in flags or by server-wide default
        if ( !options.capped ) {
            NamespaceDetailsRSV1MetaData md( ns,
                                             _namespaceIndex.details( ns ),
                                             _getNamespaceRecordStore() );

            if ( options.flagsSet ) {
                md.setUserFlag( txn, options.flags );
            }
            else if ( newCollectionsUsePowerOf2Sizes ) {
                md.setUserFlag( txn, NamespaceDetails::Flag_UsePowerOf2Sizes );
            }
        }
        else if ( options.cappedMaxDocs > 0 ) {
            txn->recoveryUnit()->writingInt( _namespaceIndex.details( ns )->maxDocsInCapped ) =
                options.cappedMaxDocs;
        }

        {
            boost::mutex::scoped_lock lk( _collectionsLock );
            Entry*& entry = _collections[ns.toString()];
            invariant( !entry );
            txn->recoveryUnit()->registerChange(new EntryInsertion(ns, this));
            entry = new Entry();
            _insertInCache_inlock(txn, ns, entry);
        }

        if ( allocateDefaultSpace ) {
            RecordStoreV1Base* rs = _getRecordStore( txn, ns );
            if ( options.initialNumExtents > 0 ) {
                int size = _massageExtentSize( &_extentManager, options.cappedSize );
                for ( int i = 0; i < options.initialNumExtents; i++ ) {
                    rs->increaseStorageSize( txn, size, false );
                }
            }
            else if ( !options.initialExtentSizes.empty() ) {
                for ( size_t i = 0; i < options.initialExtentSizes.size(); i++ ) {
                    int size = options.initialExtentSizes[i];
                    size = _massageExtentSize( &_extentManager, size );
                    rs->increaseStorageSize( txn, size, false );
                }
            }
            else if ( options.capped ) {
                // normal
                do {
                    // Must do this at least once, otherwise we leave the collection with no
                    // extents, which is invalid.
                    int sz = _massageExtentSize( &_extentManager,
                                                 options.cappedSize - rs->storageSize(txn) );
                    sz &= 0xffffff00;
                    rs->increaseStorageSize( txn, sz, false );
                } while( rs->storageSize(txn) < options.cappedSize );
            }
            else {
                rs->increaseStorageSize( txn, _extentManager.initialSize( 128 ), false );
            }
        }

        return Status::OK();
    }

    void MMAPV1DatabaseCatalogEntry::createNamespaceForIndex(OperationContext* txn,
                                                             const StringData& name) {
        // This is a simplified form of createCollection.
        _lazyInit(txn);
        invariant(!_namespaceIndex.details(name));

        _addNamespaceToNamespaceCollection(txn, name, NULL);
        _namespaceIndex.add_ns(txn, name, DiskLoc(), false);

        boost::mutex::scoped_lock lk( _collectionsLock );
        Entry*& entry = _collections[name.toString()];
        invariant( !entry );
        txn->recoveryUnit()->registerChange(new EntryInsertion(name, this));
        entry = new Entry();
        _insertInCache_inlock(txn, name, entry);
    }

    CollectionCatalogEntry* MMAPV1DatabaseCatalogEntry::getCollectionCatalogEntry( OperationContext* txn,
                                                                                  const StringData& ns ) const {
        boost::mutex::scoped_lock lk( _collectionsLock );
        CollectionMap::const_iterator i = _collections.find( ns.toString() );
        if ( i == _collections.end() )
            return NULL;

        invariant( i->second->catalogEntry.get() );
        return i->second->catalogEntry.get();
    }

    void MMAPV1DatabaseCatalogEntry::_insertInCache_inlock(OperationContext* txn,
                                                         const StringData& ns,
                                                         Entry* entry) {
        NamespaceDetails* details = _namespaceIndex.details(ns);
        invariant(details);

        entry->catalogEntry.reset( new NamespaceDetailsCollectionCatalogEntry( ns,
                                                                               details,
                                                                               _getIndexRecordStore_inlock(),
                                                                               this ) );

        NamespaceString nss( ns );

        auto_ptr<NamespaceDetailsRSV1MetaData> md( new NamespaceDetailsRSV1MetaData( ns,
                                                                                     details,
                                                                                     _getNamespaceRecordStore_inlock() ) );

        if ( details->isCapped ) {
            entry->recordStore.reset( new CappedRecordStoreV1( txn,
                                                               NULL, //TOD(ERH) this will blow up :)
                                                               ns,
                                                               md.release(),
                                                               &_extentManager,
                                                               nss.coll() == "system.indexes" ) );
        }
        else {
            entry->recordStore.reset( new SimpleRecordStoreV1( txn,
                                                               ns,
                                                               md.release(),
                                                               &_extentManager,
                                                               nss.coll() == "system.indexes" ) );
        }
    }

    RecordStore* MMAPV1DatabaseCatalogEntry::getRecordStore( OperationContext* txn,
                                                            const StringData& ns ) {
        return _getRecordStore( txn, ns );
    }

    RecordStoreV1Base* MMAPV1DatabaseCatalogEntry::_getRecordStore( OperationContext* txn,
                                                                   const StringData& ns ) {
        boost::mutex::scoped_lock lk( _collectionsLock );
        CollectionMap::const_iterator i = _collections.find( ns.toString() );
        if ( i == _collections.end() )
            return NULL;

        invariant( i->second->recordStore.get() );
        return i->second->recordStore.get();
    }

    IndexAccessMethod* MMAPV1DatabaseCatalogEntry::getIndex( OperationContext* txn,
                                                            const CollectionCatalogEntry* collection,
                                                            IndexCatalogEntry* entry ) {
        const string& type = entry->descriptor()->getAccessMethodName();

        string ns = collection->ns().ns();

        if ( IndexNames::TEXT == type ||
             entry->descriptor()->getInfoElement("expireAfterSeconds").isNumber() ) {
            NamespaceDetailsRSV1MetaData md( ns,
                                             _namespaceIndex.details( ns ),
                                             _getNamespaceRecordStore() );
            md.setUserFlag( txn, NamespaceDetails::Flag_UsePowerOf2Sizes );
        }

        RecordStore* rs = _getRecordStore(txn, entry->descriptor()->indexNamespace());
        invariant(rs);

        std::auto_ptr<SortedDataInterface> btree(
            getMMAPV1Interface(entry->headManager(),
                               rs,
                               entry->ordering(),
                               entry->descriptor()->indexNamespace(),
                               entry->descriptor()->version(),
                               &BtreeBasedAccessMethod::invalidateCursors));

        if (IndexNames::HASHED == type)
            return new HashAccessMethod( entry, btree.release() );

        if (IndexNames::GEO_2DSPHERE == type)
            return new S2AccessMethod( entry, btree.release() );

        if (IndexNames::TEXT == type)
            return new FTSAccessMethod( entry, btree.release() );

        if (IndexNames::GEO_HAYSTACK == type)
            return new HaystackAccessMethod( entry, btree.release() );

        if ("" == type)
            return new BtreeAccessMethod( entry, btree.release() );

        if (IndexNames::GEO_2D == type)
            return new TwoDAccessMethod( entry, btree.release() );

        log() << "Can't find index for keyPattern " << entry->descriptor()->keyPattern();
        fassertFailed(17489);
    }

    RecordStoreV1Base* MMAPV1DatabaseCatalogEntry::_getIndexRecordStore() {
        boost::mutex::scoped_lock lk( _collectionsLock );
        return _getIndexRecordStore_inlock();
    }

    RecordStoreV1Base* MMAPV1DatabaseCatalogEntry::_getIndexRecordStore_inlock() {
        NamespaceString nss( name(), "system.indexes" );
        Entry* entry = _collections[nss.toString()];
        invariant( entry );
        return entry->recordStore.get();
    }

    RecordStoreV1Base* MMAPV1DatabaseCatalogEntry::_getNamespaceRecordStore() const {
        boost::mutex::scoped_lock lk( _collectionsLock );
        return _getNamespaceRecordStore_inlock();
    }

    RecordStoreV1Base* MMAPV1DatabaseCatalogEntry::_getNamespaceRecordStore_inlock() const {
        NamespaceString nss( name(), "system.namespaces" );
        CollectionMap::const_iterator i = _collections.find( nss.toString() );
        invariant( i != _collections.end() );
        return i->second->recordStore.get();
    }

    void MMAPV1DatabaseCatalogEntry::_addNamespaceToNamespaceCollection( OperationContext* txn,
                                                                         const StringData& ns,
                                                                         const BSONObj* options ) {
        boost::mutex::scoped_lock lk( _collectionsLock );
        _addNamespaceToNamespaceCollection_inlock( txn, ns, options );
    }

    void MMAPV1DatabaseCatalogEntry::_addNamespaceToNamespaceCollection_inlock( OperationContext* txn,
                                                                                const StringData& ns,
                                                                                const BSONObj* options ) {
        if ( nsToCollectionSubstring( ns ) == "system.namespaces" ) {
            // system.namespaces holds all the others, so it is not explicitly listed in the catalog.
            return;
        }

        BSONObjBuilder b;
        b.append("name", ns);
        if ( options && !options->isEmpty() )
            b.append("options", *options);
        BSONObj obj = b.done();

        RecordStoreV1Base* rs = _getNamespaceRecordStore_inlock();
        invariant( rs );
        StatusWith<DiskLoc> loc = rs->insertRecord( txn, obj.objdata(), obj.objsize(), false );
        massertStatusOK( loc.getStatus() );
    }

    void MMAPV1DatabaseCatalogEntry::_removeNamespaceFromNamespaceCollection( OperationContext* txn,
                                                                             const StringData& ns ) {
        if ( nsToCollectionSubstring( ns ) == "system.namespaces" ) {
            // system.namespaces holds all the others, so it is not explicitly listed in the catalog.
            return;
        }

        RecordStoreV1Base* rs = _getNamespaceRecordStore();
        invariant( rs );

        scoped_ptr<RecordIterator> it( rs->getIterator(txn) );
        while ( !it->isEOF() ) {
            DiskLoc loc = it->getNext();
            BSONObj entry = it->dataFor( loc ).toBson();
            BSONElement name = entry["name"];
            if ( name.type() == String && name.String() == ns ) {
                rs->deleteRecord( txn, loc );
                break;
            }
        }
    }

    CollectionOptions MMAPV1DatabaseCatalogEntry::getCollectionOptions( OperationContext* txn,
                                                                        const StringData& ns ) const {
        if ( nsToCollectionSubstring( ns ) == "system.namespaces" ) {
            return CollectionOptions();
        }

        RecordStoreV1Base* rs = _getNamespaceRecordStore();
        invariant( rs );

        scoped_ptr<RecordIterator> it( rs->getIterator(txn) );
        while ( !it->isEOF() ) {
            DiskLoc loc = it->getNext();
            BSONObj entry = it->dataFor( loc ).toBson();
            BSONElement name = entry["name"];
            if ( name.type() == String && name.String() == ns ) {
                CollectionOptions options;
                if ( entry["options"].isABSONObj() ) {
                    Status status = options.parse( entry["options"].Obj() );
                    fassert( 18523, status );
                }
                return options;
            }
        }

        return CollectionOptions();
    }
} // namespace mongo
