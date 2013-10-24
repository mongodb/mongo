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
#include "mongo/db/clientcursor.h"
#include "mongo/db/index_legacy.h"
#include "mongo/db/index_update.h"
#include "mongo/db/index/2d_access_method.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/index/btree_access_method_internal.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/index/hash_access_method.h"
#include "mongo/db/index/haystack_access_method.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/s2_access_method.h"
#include "mongo/db/index_names.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/rs.h" // this is ugly
#include "mongo/db/structure/collection.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

#define INDEX_CATALOG_MAGIC 283711

    // What's the default version of our indices?
    const int DefaultIndexVersionNumber = 1;

    // -------------

    IndexCatalog::IndexCatalog( Collection* collection, NamespaceDetails* details )
        : _magic(INDEX_CATALOG_MAGIC), _collection( collection ), _details( details ),
          _descriptorCache( NamespaceDetails::NIndexesMax ),
          _accessMethodCache( NamespaceDetails::NIndexesMax ),
          _forcedBtreeAccessMethodCache( NamespaceDetails::NIndexesMax ) {
    }

    IndexCatalog::~IndexCatalog() {
        _magic = 123456;

        for ( unsigned i = 0; i < _descriptorCache.size(); i++ ) {
            delete _descriptorCache[i];
        }

        for ( unsigned i = 0; i < _accessMethodCache.size(); i++ ) {
            delete _accessMethodCache[i];
        }

    }

    void IndexCatalog::_checkMagic() const {
        dassert( _descriptorCache.capacity() == NamespaceDetails::NIndexesMax );
        dassert( _accessMethodCache.capacity() == NamespaceDetails::NIndexesMax );
        dassert( _forcedBtreeAccessMethodCache.capacity() == NamespaceDetails::NIndexesMax );

        if ( _magic == INDEX_CATALOG_MAGIC )
            return;
        log() << "IndexCatalog::_magic wrong, is : " << _magic;
        fassertFailed(17198);
    }

    bool IndexCatalog::_shouldOverridePlugin(const BSONObj& keyPattern) {
        string pluginName = IndexNames::findPluginName(keyPattern);
        bool known = IndexNames::isKnownName(pluginName);

        if (NULL == cc().database()) {
            return false;
        }

        const DataFileHeader* dfh = _collection->_database->getFile(0)->getHeader();

        if (dfh->versionMinor == PDFILE_VERSION_MINOR_24_AND_NEWER) {
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

    string IndexCatalog::_getAccessMethodName(const BSONObj& keyPattern) {
        if ( _shouldOverridePlugin(keyPattern) ) {
            return "";
        }

        return IndexNames::findPluginName(keyPattern);
    }


    // ---------------------------

    Status IndexCatalog::_upgradeDatabaseMinorVersionIfNeeded( const string& newPluginName ) {
        Database* db = _collection->_database;

        DataFileHeader* dfh = db->getFile(0)->getHeader();
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

    Status IndexCatalog::createIndex( BSONObj spec, bool mayInterrupt ) {

        // 1) add entry in system.indexes
        // 2) call into buildAnIndex?

        Status status = okToAddIndex( spec );
        if ( !status.isOK() )
            return status;

        // TODO: _id stuff from prepareToBuildIndex
        // TODO: replica stuff from prepareToBuildIndex

        spec = fixIndexSpec( spec );

        Database* db = _collection->_database;

        string pluginName = IndexNames::findPluginName( spec["key"].Obj() );
        if ( pluginName.size() ) {
            Status s = _upgradeDatabaseMinorVersionIfNeeded( pluginName );
            if ( !s.isOK() )
                return s;
        }


        Collection* systemIndexes = db->getCollection( db->_indexesName );
        if ( !systemIndexes ) {
            systemIndexes = db->createCollection( db->_indexesName, false, NULL );
            verify( systemIndexes );
        }

        StatusWith<DiskLoc> loc = systemIndexes->insertDocument( spec, false );
        if ( !loc.isOK() )
            return loc.getStatus();
        verify( !loc.getValue().isNull() );

        string idxName = spec["name"].valuestr();

        // Set curop description before setting indexBuildInProg, so that there's something
        // commands can find and kill as soon as indexBuildInProg is set. Only set this if it's a
        // killable index, so we don't overwrite commands in currentOp.
        if ( mayInterrupt ) {
            cc().curop()->setQuery( spec );
        }

        IndexBuildBlock indexBuildBlock( this, idxName, loc.getValue() );
        verify( indexBuildBlock.indexDetails() );

        try {
            buildAnIndex( _collection->ns(), _collection->details(),
                          *indexBuildBlock.indexDetails(), mayInterrupt);
            indexBuildBlock.success();
            return Status::OK();
        }
        catch (DBException& e) {
            // save our error msg string as an exception or dropIndexes will overwrite our message
            LastError *le = lastError.get();
            int savecode = 0;
            string saveerrmsg;
            if ( le ) {
                savecode = le->code;
                saveerrmsg = le->msg;
            }
            else {
                savecode = e.getCode();
                saveerrmsg = e.what();
            }

            verify(le && !saveerrmsg.empty());
            setLastError(savecode,saveerrmsg.c_str());
            throw; // XXX
        }


    }

    IndexCatalog::IndexBuildBlock::IndexBuildBlock( IndexCatalog* catalog,
                                                    const StringData& indexName,
                                                    const DiskLoc& loc )
        : _catalog( catalog ),
          _ns( _catalog->_collection->ns().ns() ),
          _indexName( indexName.toString() ),
          _indexDetails( NULL ) {

        _nsd = _catalog->_collection->details();

        verify( catalog );
        verify( _nsd );
        verify( _catalog->_collection->ok() );
        verify( !loc.isNull() );

        _indexDetails = &_nsd->getNextIndexDetails( _ns.c_str() );

        try {
            // we don't want to kill a half formed IndexDetails, so be carefule
            log() << "creating index with info @ " << loc;
            getDur().writingDiskLoc( _indexDetails->info ) = loc;
        }
        catch ( DBException& e ) {
            log() << "got exception trying to assign loc to IndexDetails" << e;
            _indexDetails = NULL;
            return;
        }

        try {
            getDur().writingInt( _nsd->_indexBuildsInProgress ) += 1;
        }
        catch ( DBException& e ) {
            log() << "got exception trying to incrementStats _indexBuildsInProgress: " << e;
            _indexDetails = NULL;
            return;
        }

    }

    IndexCatalog::IndexBuildBlock::~IndexBuildBlock() {
        if ( !_indexDetails ) {
            // taken care of already
            return;
        }

        fassert( 17204, _catalog->_collection->ok() );

        int idxNo = _nsd->findIndexByName( _indexName, true );
        fassert( 17205, idxNo >= 0 );

        _catalog->_dropIndex( idxNo );

        _indexDetails = NULL;
    }

    void IndexCatalog::IndexBuildBlock::success() {

        fassert( 17206, _indexDetails );
        _indexDetails = NULL;

        fassert( 17207, _catalog->_collection->ok() );

        int idxNo = _nsd->findIndexByName( _indexName, true );
        fassert( 17202, idxNo >= 0 );

        // Make sure the newly created index is relocated to nIndexes, if it isn't already there
        if ( idxNo != _nsd->getCompletedIndexCount() ) {
            log() << "switching indexes at position " << idxNo << " and "
                  << _nsd->getCompletedIndexCount() << endl;

            int toIdxNo = _nsd->getCompletedIndexCount();

            _nsd->swapIndex( _ns.c_str(), idxNo, toIdxNo );

            IndexDescriptor* a = _catalog->_descriptorCache[idxNo];
            IndexAccessMethod* b = _catalog->_accessMethodCache[idxNo];
            BtreeBasedAccessMethod* c = _catalog->_forcedBtreeAccessMethodCache[idxNo];

            _catalog->_descriptorCache[idxNo] = _catalog->_descriptorCache[toIdxNo];
            _catalog->_accessMethodCache[idxNo] = _catalog->_accessMethodCache[toIdxNo];
            _catalog->_forcedBtreeAccessMethodCache[idxNo] = _catalog->_forcedBtreeAccessMethodCache[toIdxNo];

            _catalog->_descriptorCache[toIdxNo] = a;
            _catalog->_accessMethodCache[toIdxNo] = b;
            _catalog->_forcedBtreeAccessMethodCache[toIdxNo] = c;

            idxNo = _nsd->getCompletedIndexCount();
        }

        getDur().writingInt( _nsd->_indexBuildsInProgress ) -= 1;
        getDur().writingInt( _nsd->_nIndexes ) += 1;

        _catalog->_collection->infoCache()->addedIndex();

        IndexLegacy::postBuildHook( _nsd, _nsd->idx( idxNo ) );
    }



    Status IndexCatalog::okToAddIndex( const BSONObj& spec ) const {

        const NamespaceString& nss = _collection->ns();


        if ( nss.isSystemDotIndexes() )
            return Status( ErrorCodes::CannotCreateIndex,
                           "cannot create indexes on the system.indexes collection" );

        // logical name of the index
        const char *name = spec.getStringField("name");
        if ( !name[0] )
            return Status( ErrorCodes::CannotCreateIndex, "no index name specified" );

        string indexNamespace = IndexDetails::indexNamespaceFromObj(spec);
        if ( indexNamespace.length() > 128 )
            return Status( ErrorCodes::CannotCreateIndex,
                           str::stream() << "namespace name generated from index name \"" <<
                           indexNamespace << "\" is too long (128 char max)" );

        BSONObj key = spec.getObjectField("key");
        if ( key.objsize() > 2048 )
            return Status( ErrorCodes::CannotCreateIndex, "index key pattern too large" );

        if ( key.isEmpty() )
            return Status( ErrorCodes::CannotCreateIndex, "index key is empty" );

        if( !validKeyPattern(key) ) {
            return Status( ErrorCodes::CannotCreateIndex,
                           str::stream() << "bad index key pattern " << key );
        }

        {
            // Check both existing and in-progress indexes (2nd param = true)
            const int idx = _details->findIndexByName(name, true);
            if (idx >= 0) {
                // index already exists.
                const IndexDetails& indexSpec( _details->idx(idx) );
                BSONObj existingKeyPattern(indexSpec.keyPattern());

                if ( !existingKeyPattern.equal( key ) )
                    return Status( ErrorCodes::CannotCreateIndex,
                                   str::stream() << "Trying to create an index "
                                   << "with same name " << name
                                   << " with different key spec " << key
                                   << " vs existing spec " << existingKeyPattern );

                if ( !indexSpec.areIndexOptionsEquivalent( spec ) )
                    return Status( ErrorCodes::CannotCreateIndex,
                                   str::stream() << "Index with name: " << name
                                   << " already exists with different options" );

                // Index already exists with the same options, so no need to build a new
                // one (not an error). Most likely requested by a client using ensureIndex.
                return Status( ErrorCodes::IndexAlreadyExists, name );
            }
        }

        {
            // Check both existing and in-progress indexes (2nd param = true)
            const int idx = _details->findIndexByKeyPattern(key, true);
            if (idx >= 0) {
                LOG(2) << "index already exists with diff name " << name
                        << ' ' << key << endl;

                const IndexDetails& indexSpec(_details->idx(idx));
                if ( !indexSpec.areIndexOptionsEquivalent( spec ) )
                    return Status( ErrorCodes::CannotCreateIndex,
                                   str::stream() << "Index with pattern: " << key
                                   << " already exists with different options" );

                return Status( ErrorCodes::IndexAlreadyExists, name );
            }
        }

        if ( _details->getTotalIndexCount() >= NamespaceDetails::NIndexesMax ) {
            string s = str::stream() << "add index fails, too many indexes for "
                                     << _collection->ns().ns() << " key:" << key.toString();
            log() << s;
            return Status( ErrorCodes::CannotCreateIndex, s );
        }

        string pluginName = IndexNames::findPluginName( key );
        if ( pluginName.size() ) {
            if ( !IndexNames::isKnownName( pluginName ) )
                return Status( ErrorCodes::CannotCreateIndex,
                               str::stream() << "Unknown index plugin '" << pluginName << "' "
                               << "in index "<< key );

        }

        return Status::OK();
    }

    Status IndexCatalog::dropAllIndexes( bool includingIdIndex ) {
        BackgroundOperation::assertNoBgOpInProgForNs( _collection->ns().ns() );

        // there may be pointers pointing at keys in the btree(s).  kill them.
        // TODO: can this can only clear cursors on this index?
        ClientCursor::invalidate( _collection->ns().ns() );

        // make sure nothing in progress
        verify( numIndexesTotal() == numIndexesReady() );

        LOG(4) << "  d->nIndexes was " << numIndexesTotal() << std::endl;

        IndexDetails *idIndex = 0;

        for ( int i = 0; i < numIndexesTotal(); i++ ) {

            if ( !includingIdIndex && _details->idx(i).isIdIndex() ) {
                idIndex = &_details->idx(i);
                continue;
            }

            Status s = dropIndex( i );
            if ( !s.isOK() )
                return s;
            i--;
        }

        if ( idIndex ) {
            verify( numIndexesTotal() == 1 );
        }
        else {
            verify( numIndexesTotal() == 0 );
        }

        _assureSysIndexesEmptied( idIndex );

        return Status::OK();
    }

    Status IndexCatalog::dropIndex( IndexDescriptor* desc ) {
        return dropIndex( desc->getIndexNumber() );
    }

    Status IndexCatalog::dropIndex( int idxNo ) {

        /**
         * IndexState in order
         *  <db>.system.indexes
         *    NamespaceDetails
         *      <db>.system.ns
         */

        // ----- SANITY CHECKS -------------

        verify( idxNo >= 0 );
        verify( idxNo < numIndexesReady() );
        verify( numIndexesReady() == numIndexesTotal() );

        // ------ CLEAR CACHES, ETC -----------

        BackgroundOperation::assertNoBgOpInProgForNs( _collection->ns().ns() );

        return _dropIndex( idxNo );
    }

    Status IndexCatalog::_dropIndex( int idxNo ) {
        verify( idxNo < numIndexesTotal() );
        verify( idxNo >= 0 );

        _checkMagic();
        // there may be pointers pointing at keys in the btree(s).  kill them.
        // TODO: can this can only clear cursors on this index?
        ClientCursor::invalidate( _collection->ns().ns() );

        // wipe out stats
        _collection->infoCache()->reset();

        string indexNamespace = _details->idx( idxNo ).indexNamespace();
        string indexName = _details->idx( idxNo ).indexName();

        // delete my entries first so we don't have invalid pointers lying around
        delete _descriptorCache[idxNo];
        delete _accessMethodCache[idxNo];
        delete _forcedBtreeAccessMethodCache[idxNo];

        _descriptorCache[idxNo] = NULL;
        _accessMethodCache[idxNo] = NULL;
        _forcedBtreeAccessMethodCache[idxNo] = NULL;

        // --------- START REAL WORK ----------

        audit::logDropIndex( currentClient.get(), indexNamespace, _collection->ns().ns() );

        try {
            _details->clearSystemFlag( NamespaceDetails::Flag_HaveIdIndex );

            // ****   this is the first disk change ****

            // data + system.namespaces
            Status status = _collection->_database->_dropNS( indexNamespace );
            if ( !status.isOK() ) {
                LOG(2) << "IndexDetails::kill(): couldn't drop index " << indexNamespace;
            }

            // all info in the .ns file
            _details->_removeIndexFromMe( idxNo );

            // remove from system.indexes
            int n = _removeFromSystemIndexes( indexName );
            wassert( n == 1 );

        }
        catch ( std::exception& ) {
            // this is bad, and we don't really know state
            // going to leak to make sure things are safe

            log() << "error dropping index: " << indexNamespace
                  << " going to leak some memory to be safe";

            _descriptorCache.clear();
            _accessMethodCache.clear();
            _forcedBtreeAccessMethodCache.clear();

            _collection->_database->_clearCollectionCache( indexNamespace );

            throw;
        }

        _collection->_database->_clearCollectionCache( indexNamespace );

        // now that is really gone can fix arrays

        unsigned idxNoUnsigned = static_cast<unsigned>( idxNo );

        if ( idxNoUnsigned < _descriptorCache.size() )
            _descriptorCache.erase( _descriptorCache.begin() + idxNo );
        if ( idxNoUnsigned < _accessMethodCache.size() )
            _accessMethodCache.erase( _accessMethodCache.begin() + idxNo );
        if ( idxNoUnsigned < _forcedBtreeAccessMethodCache.size() )
            _forcedBtreeAccessMethodCache.erase( _forcedBtreeAccessMethodCache.begin() + idxNo );

        return Status::OK();
    }

    int IndexCatalog::_removeFromSystemIndexes( const StringData& indexName ) {
        BSONObjBuilder b;
        b.append( "ns", _collection->ns() );
        b.append( "name", indexName );
        BSONObj cond = b.done(); // e.g.: { name: "ts_1", ns: "foo.coll" }
        return static_cast<int>( deleteObjects( _collection->_database->_indexesName,
                                                cond, false, false, true ) );
    }

    int IndexCatalog::_assureSysIndexesEmptied( IndexDetails* idIndex ) {
        BSONObjBuilder b;
        b.append("ns", _collection->ns() );
        if ( idIndex ) {
            b.append("name", BSON( "$ne" << idIndex->indexName().c_str() ));
        }
        BSONObj cond = b.done();
        int n = static_cast<int>( deleteObjects( _collection->_database->_indexesName,
                                                 cond, false, false, true) );
        if( n ) {
            warning() << "assureSysIndexesEmptied cleaned up " << n << " entries";
        }
        return n;
    }

    BSONObj IndexCatalog::prepOneUnfinishedIndex() {
        verify( _details->_indexBuildsInProgress > 0 );

        // details.info is always a valid system.indexes entry because DataFileMgr::insert journals
        // creating the index doc and then insert_makeIndex durably assigns its DiskLoc to info.
        // indexBuildsInProgress is set after that, so if it is set, info must be set.
        int offset = numIndexesTotal() - 1;

        BSONObj info = _details->idx(offset).info.obj().getOwned();

        Status s = _dropIndex( offset );

        massert( 17200,
                 str::stream() << "failed to to dropIndex in prepOneUnfinishedIndex: " << s.toString(),
                 s.isOK() );

        return info;
    }

    Status IndexCatalog::blowAwayInProgressIndexEntries() {
        while ( numIndexesInProgress() > 0 ) {
            Status s = dropIndex( numIndexesTotal() - 1 );
            if ( !s.isOK() )
                return s;
        }
        return Status::OK();
    }

    // ---------------------------

    int IndexCatalog::numIndexesTotal() const {
        return _details->getTotalIndexCount();
    }

    int IndexCatalog::numIndexesReady() const {
        return _details->getCompletedIndexCount();
    }

    IndexDescriptor* IndexCatalog::getDescriptor( int idxNo ) {
        _checkMagic();

        if ( _descriptorCache[idxNo] )
            return _descriptorCache[idxNo];

        IndexDetails* id = &_details->idx(idxNo);

        if ( static_cast<unsigned>( idxNo ) >= _descriptorCache.size() )
            _descriptorCache.resize( idxNo + 1 );

        _descriptorCache[idxNo] = new IndexDescriptor(_details, idxNo, id, id->info.obj());

        return _descriptorCache[idxNo];
    }

    IndexAccessMethod* IndexCatalog::getIndex( IndexDescriptor* desc ) {
        _checkMagic();
        int idxNo = desc->getIndexNumber();

        if ( _accessMethodCache[idxNo] ) {
            return _accessMethodCache[idxNo];
        }

        IndexAccessMethod* newlyCreated = 0;

        string type = _getAccessMethodName(desc->keyPattern());

        if (IndexNames::HASHED == type) {
            newlyCreated = new HashAccessMethod(desc);
        }
        else if (IndexNames::GEO_2DSPHERE == type) {
            newlyCreated = new S2AccessMethod(desc);
        }
        else if (IndexNames::TEXT == type || IndexNames::TEXT_INTERNAL == type) {
            newlyCreated = new FTSAccessMethod(desc);
        }
        else if (IndexNames::GEO_HAYSTACK == type) {
            newlyCreated =  new HaystackAccessMethod(desc);
        }
        else if ("" == type) {
            newlyCreated =  new BtreeAccessMethod(desc);
        }
        else if (IndexNames::GEO_2D == type) {
            newlyCreated = new TwoDAccessMethod(desc);
        }
        else {
            log() << "Can't find index for keypattern " << desc->keyPattern();
            verify(0);
            return NULL;
        }

        _accessMethodCache[idxNo] = newlyCreated;

        return newlyCreated;
    }

    // ---------------------------

    Status IndexCatalog::_indexRecord( int idxNo, const BSONObj& obj, const DiskLoc &loc ) {
        IndexDescriptor* desc = getDescriptor( idxNo );
        verify(desc);
        IndexAccessMethod* iam = getIndex( desc );
        verify(iam);

        InsertDeleteOptions options;
        options.logIfError = false;
        options.dupsAllowed =
            ignoreUniqueIndex( desc->getOnDisk() ) ||
            ( !KeyPattern::isIdKeyPattern(desc->keyPattern()) && !desc->unique() );

        int64_t inserted;
        return iam->insert(obj, loc, options, &inserted);
    }

    Status IndexCatalog::_unindexRecord( int idxNo, const BSONObj& obj, const DiskLoc &loc, bool logIfError ) {
        IndexDescriptor* desc = getDescriptor( idxNo );
        verify( desc );
        IndexAccessMethod* iam = getIndex( desc );
        verify( iam );

        InsertDeleteOptions options;
        options.logIfError = logIfError;

        int64_t removed;
        Status status = iam->remove(obj, loc, options, &removed);

        if ( !status.isOK() ) {
            problem() << "Couldn't unindex record " << obj.toString()
                      << " status: " << status.toString();
        }

        return Status::OK();
    }


    void IndexCatalog::indexRecord( const BSONObj& obj, const DiskLoc &loc ) {

        for ( int i = 0; i < numIndexesTotal(); i++ ) {
            try {
                Status s = _indexRecord( i, obj, loc );
                uassert(s.location(), s.reason(), s.isOK() );
            }
            catch ( AssertionException& ae ) {

                LOG(2) << "IndexCatalog::indexRecord failed: " << ae;

                for ( int j = 0; j <= i; j++ ) {
                    try {
                        _unindexRecord( j, obj, loc, false );
                    }
                    catch ( DBException& e ) {
                        LOG(1) << "IndexCatalog::indexRecord rollback failed: " << e;
                    }
                }

                throw;
            }
        }

    }

    void IndexCatalog::unindexRecord( const BSONObj& obj, const DiskLoc& loc, bool noWarn ) {
        int numIndices = numIndexesTotal();

        for (int i = 0; i < numIndices; i++) {
            // If i >= d->nIndexes, it's a background index, and we DO NOT want to log anything.
            bool logIfError = ( i < numIndexesTotal() ) ? !noWarn : false;
            _unindexRecord( i, obj, loc, logIfError );
        }

    }

    Status IndexCatalog::checkNoIndexConflicts( const BSONObj &obj ) {
        for ( int idxNo = 0; idxNo < numIndexesTotal(); idxNo++ ) {
            if( !_details->idx(idxNo).unique() )
                continue;

            IndexDetails& idx = _details->idx(idxNo);
            if (ignoreUniqueIndex(idx)) // XXX(ERH)
                continue;

            IndexDescriptor* descriptor = getDescriptor( idxNo );
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


    bool IndexCatalog::validKeyPattern( const BSONObj& kp ) {
        BSONObjIterator i(kp);
        while( i.more() ) {
            BSONElement e = i.next();
            if( e.type() == Object || e.type() == Array )
                return false;
        }
        return true;
    }

    BSONObj IndexCatalog::fixIndexSpec( const BSONObj& spec ) {
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

        {
            // stripping _id
            BSONObjIterator i(o);
            while ( i.more() ) {
                BSONElement e = i.next();
                string s = e.fieldName();
                if ( s != "_id" && s != "v" && s != "unique" )
                    b.append(e);
            }
        }

        return b.obj();
    }
}
