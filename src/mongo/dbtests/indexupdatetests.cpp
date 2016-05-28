//@file indexupdatetests.cpp : mongo/db/index_update.{h,cpp} tests

/**
 *    Copyright (C) 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <cstdint>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d.h"
#include "mongo/dbtests/dbtests.h"

namespace IndexUpdateTests {

using std::unique_ptr;

static const char* const _ns = "unittests.indexupdate";

/**
 * Test fixture for a write locked test using collection _ns.  Includes functionality to
 * partially construct a new IndexDetails in a manner that supports proper cleanup in
 * dropCollection().
 */
class IndexBuildBase {
public:
    IndexBuildBase() : _ctx(&_txn, _ns), _client(&_txn) {
        _client.createCollection(_ns);
    }
    ~IndexBuildBase() {
        _client.dropCollection(_ns);
        getGlobalServiceContext()->unsetKillAllOperations();
    }
    Collection* collection() {
        return _ctx.getCollection();
    }

protected:
// QUERY_MIGRATION
#if 0
        /** @return IndexDetails for a new index on a:1, with the info field populated. */
        IndexDescriptor* addIndexWithInfo() {
            BSONObj indexInfo = BSON( "v" << 1 <<
                                      "key" << BSON( "a" << 1 ) <<
                                      "ns" << _ns <<
                                      "name" << "a_1" );
            int32_t lenWHdr = indexInfo.objsize() + Record::HeaderSize;
            const char* systemIndexes = "unittests.system.indexes";
            RecordId infoLoc = allocateSpaceForANewRecord( systemIndexes,
                                                          nsdetails( systemIndexes ),
                                                          lenWHdr,
                                                          false );
            Record* infoRecord = reinterpret_cast<Record*>( getDur().writingPtr( infoLoc.rec(),
                                                                                 lenWHdr ) );
            memcpy( infoRecord->data(), indexInfo.objdata(), indexInfo.objsize() );
            addRecordToRecListInExtent( infoRecord, infoLoc );

            IndexCatalog::IndexBuildBlock blk( collection()->getIndexCatalog(), "a_1", infoLoc );
            blk.success();

            return collection()->getIndexCatalog()->findIndexByName( "a_1" );
        }
#endif

    Status createIndex(const std::string& dbname, const BSONObj& indexSpec);

    bool buildIndexInterrupted(const BSONObj& key, bool allowInterruption) {
        try {
            MultiIndexBlock indexer(&_txn, collection());
            if (allowInterruption)
                indexer.allowInterruption();

            uassertStatusOK(indexer.init(key));
            uassertStatusOK(indexer.insertAllDocumentsInCollection());
            WriteUnitOfWork wunit(&_txn);
            indexer.commit();
            wunit.commit();
        } catch (const DBException& e) {
            if (ErrorCodes::isInterruption(ErrorCodes::Error(e.getCode())))
                return true;

            throw;
        }
        return false;
    }

    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _txn = *_txnPtr;
    OldClientWriteContext _ctx;
    DBDirectClient _client;
};

/** addKeysToPhaseOne() adds keys from a collection's documents to an external sorter. */
// QUERY_MIGRATION
#if 0
    class AddKeysToPhaseOne : public IndexBuildBase {
    public:
        void run() {
            // Add some data to the collection.
            int32_t nDocs = 130;
            for( int32_t i = 0; i < nDocs; ++i ) {
                _client.insert( _ns, BSON( "a" << i ) );
            }

            IndexDescriptor* id = addIndexWithInfo();
            // Create a SortPhaseOne.
            SortPhaseOne phaseOne;
            ProgressMeterHolder pm (txn->setMessage("AddKeysToPhaseOne",
                                                    "AddKeysToPhaseOne Progress",
                                                    nDocs,
                                                    nDocs));
            // Add keys to phaseOne.
            BtreeBasedBuilder::addKeysToPhaseOne( collection(),
                                                  id,
                                                  BSON( "a" << 1 ),
                                                  &phaseOne,
                                                  pm.get(), true );
            // Keys for all documents were added to phaseOne.
            ASSERT_EQUALS( static_cast<uint64_t>( nDocs ), phaseOne.n );
        }
    };

    /** addKeysToPhaseOne() aborts if the current operation is killed. */
    class InterruptAddKeysToPhaseOne : public IndexBuildBase {
    public:
        InterruptAddKeysToPhaseOne( bool mayInterrupt ) :
            _mayInterrupt( mayInterrupt ) {
        }
        void run() {
            // It's necessary to index sufficient keys that a RARELY condition will be triggered.
            int32_t nDocs = 130;
            // Add some data to the collection.
            for( int32_t i = 0; i < nDocs; ++i ) {
                _client.insert( _ns, BSON( "a" << i ) );
            }
            IndexDescriptor* id = addIndexWithInfo();
            // Create a SortPhaseOne.
            SortPhaseOne phaseOne;
            ProgressMeterHolder pm (txn->setMessage("InterruptAddKeysToPhaseOne",
                                                    "InterruptAddKeysToPhaseOne Progress",
                                                    nDocs,
                                                    nDocs));
            // Register a request to kill the current operation.
            CurOp::get(txn)->kill();
            if ( _mayInterrupt ) {
                // Add keys to phaseOne.
                ASSERT_THROWS( BtreeBasedBuilder::addKeysToPhaseOne( collection(),
                                                                     id,
                                                                     BSON( "a" << 1 ),
                                                                     &phaseOne,
                                                                     pm.get(),
                                                                     _mayInterrupt ),
                               UserException );
                // Not all keys were added to phaseOne due to the interrupt.
                ASSERT( static_cast<uint64_t>( nDocs ) > phaseOne.n );
            }
            else {
                // Add keys to phaseOne.
                BtreeBasedBuilder::addKeysToPhaseOne( collection(),
                                                      id,
                                                      BSON( "a" << 1 ),
                                                      &phaseOne,
                                                      pm.get(),
                                                      _mayInterrupt );
                // All keys were added to phaseOne despite to the kill request, because
                // mayInterrupt == false.
                ASSERT_EQUALS( static_cast<uint64_t>( nDocs ), phaseOne.n );
            }
        }
    private:
        bool _mayInterrupt;
    };
#endif

// QUERY_MIGRATION
#if 0
    /** buildBottomUpPhases2And3() builds a btree from the keys in an external sorter. */
    class BuildBottomUp : public IndexBuildBase {
    public:
        void run() {
            IndexDescriptor* id = addIndexWithInfo();
            // Create a SortPhaseOne.
            SortPhaseOne phaseOne;
            phaseOne.sorter.reset( new BSONObjExternalSorter(_aFirstSort));
            // Add index keys to the phaseOne.
            int32_t nKeys = 130;
            for( int32_t i = 0; i < nKeys; ++i ) {
                phaseOne.sorter->add( BSON( "a" << i ), /* dummy disk loc */ RecordId(), false );
            }
            phaseOne.nkeys = phaseOne.n = nKeys;
            phaseOne.sorter->sort( false );
            // Set up remaining arguments.
            set<RecordId> dups;
            CurOp* op = CurOp::get(txn);
            ProgressMeterHolder pm (op->setMessage("BuildBottomUp",
                                                   "BuildBottomUp Progress",
                                                   nKeys,
                                                   nKeys));
            pm.finished();
            Timer timer;
            // The index's root has not yet been set.
            ASSERT( id->getHead().isNull() );
            // Finish building the index.
            buildBottomUpPhases2And3<V1>( true,
                                          id,
                                          *phaseOne.sorter,
                                          false,
                                          dups,
                                          op,
                                          &phaseOne,
                                          pm,
                                          timer,
                                          true );
            // The index's root is set after the build is complete.
            ASSERT( !id->getHead().isNull() );
            // Create a cursor over the index.
            unique_ptr<BtreeCursor> cursor(
                    BtreeCursor::make( nsdetails( _ns ),
                                       id->getOnDisk(),
                                       BSON( "" << -1 ),    // startKey below minimum key.
                                       BSON( "" << nKeys ), // endKey above maximum key.
                                       true,                // endKeyInclusive true.
                                       1                    // direction forward.
                                       ) );
            // Check that the keys in the index are the expected ones.
            int32_t expectedKey = 0;
            for( ; cursor->ok(); cursor->advance(), ++expectedKey ) {
                ASSERT_EQUALS( expectedKey, cursor->currKey().firstElement().number() );
            }
            ASSERT_EQUALS( nKeys, expectedKey );
        }
    };
#endif

// QUERY_MIGRATION
#if 0
    /** buildBottomUpPhases2And3() aborts if the current operation is interrupted. */
    class InterruptBuildBottomUp : public IndexBuildBase {
    public:
        InterruptBuildBottomUp( bool mayInterrupt ) :
            _mayInterrupt( mayInterrupt ) {
        }
        void run() {
            IndexDescriptor* id = addIndexWithInfo();
            // Create a SortPhaseOne.
            SortPhaseOne phaseOne;
            phaseOne.sorter.reset(new BSONObjExternalSorter(_aFirstSort));
            // It's necessary to index sufficient keys that a RARELY condition will be triggered,
            // but few enough keys that the btree builder will not create an internal node and check
            // for an interrupt internally (which would cause this test to pass spuriously).
            int32_t nKeys = 130;
            // Add index keys to the phaseOne.
            for( int32_t i = 0; i < nKeys; ++i ) {
                phaseOne.sorter->add( BSON( "a" << i ), /* dummy disk loc */ RecordId(), false );
            }
            phaseOne.nkeys = phaseOne.n = nKeys;
            phaseOne.sorter->sort( false );
            // Set up remaining arguments.
            set<RecordId> dups;
            CurOp* op = CurOp::get(txn);
            ProgressMeterHolder pm (op->setMessage("InterruptBuildBottomUp",
                                                   "InterruptBuildBottomUp Progress",
                                                   nKeys,
                                                   nKeys));
            pm.finished();
            Timer timer;
            // The index's root has not yet been set.
            ASSERT( id->getHead().isNull() );
            // Register a request to kill the current operation.
            CurOp::get(txn)->kill();
            if ( _mayInterrupt ) {
                // The build is aborted due to the kill request.
                ASSERT_THROWS
                        ( buildBottomUpPhases2And3<V1>( true,
                                                        id,
                                                        *phaseOne.sorter,
                                                        false,
                                                        dups,
                                                        op,
                                                        &phaseOne,
                                                        pm,
                                                        timer,
                                                        _mayInterrupt ),
                          UserException );
                // The root of the index is not set because the build did not complete.
                ASSERT( id->getHead().isNull() );
            }
            else {
                // The build is aborted despite the kill request because mayInterrupt == false.
                buildBottomUpPhases2And3<V1>( true,
                                              id,
                                              *phaseOne.sorter,
                                              false,
                                              dups,
                                              op,
                                              &phaseOne,
                                              pm,
                                              timer,
                                              _mayInterrupt );
                // The index's root is set after the build is complete.
                ASSERT( !id->getHead().isNull() );
            }
        }
    private:
        bool _mayInterrupt;
    };
#endif
/** Index creation ignores unique constraints when told to. */
template <bool background>
class InsertBuildIgnoreUnique : public IndexBuildBase {
public:
    void run() {
        // Create a new collection.
        Database* db = _ctx.db();
        Collection* coll;
        {
            WriteUnitOfWork wunit(&_txn);
            db->dropCollection(&_txn, _ns);
            coll = db->createCollection(&_txn, _ns);

            OpDebug* const nullOpDebug = nullptr;
            coll->insertDocument(&_txn,
                                 BSON("_id" << 1 << "a"
                                            << "dup"),
                                 nullOpDebug,
                                 true);
            coll->insertDocument(&_txn,
                                 BSON("_id" << 2 << "a"
                                            << "dup"),
                                 nullOpDebug,
                                 true);
            wunit.commit();
        }

        MultiIndexBlock indexer(&_txn, coll);
        indexer.allowBackgroundBuilding();
        indexer.allowInterruption();
        indexer.ignoreUniqueConstraint();

        const BSONObj spec = BSON("name"
                                  << "a"
                                  << "ns"
                                  << coll->ns().ns()
                                  << "key"
                                  << BSON("a" << 1)
                                  << "unique"
                                  << true
                                  << "background"
                                  << background);

        ASSERT_OK(indexer.init(spec));
        ASSERT_OK(indexer.insertAllDocumentsInCollection());

        WriteUnitOfWork wunit(&_txn);
        indexer.commit();
        wunit.commit();
    }
};

/** Index creation enforces unique constraints unless told not to. */
template <bool background>
class InsertBuildEnforceUnique : public IndexBuildBase {
public:
    void run() {
        // Create a new collection.
        Database* db = _ctx.db();
        Collection* coll;
        {
            WriteUnitOfWork wunit(&_txn);
            db->dropCollection(&_txn, _ns);
            coll = db->createCollection(&_txn, _ns);

            OpDebug* const nullOpDebug = nullptr;
            coll->insertDocument(&_txn,
                                 BSON("_id" << 1 << "a"
                                            << "dup"),
                                 nullOpDebug,
                                 true);
            coll->insertDocument(&_txn,
                                 BSON("_id" << 2 << "a"
                                            << "dup"),
                                 nullOpDebug,
                                 true);
            wunit.commit();
        }

        MultiIndexBlock indexer(&_txn, coll);
        indexer.allowBackgroundBuilding();
        indexer.allowInterruption();
        // indexer.ignoreUniqueConstraint(); // not calling this

        const BSONObj spec = BSON("name"
                                  << "a"
                                  << "ns"
                                  << coll->ns().ns()
                                  << "key"
                                  << BSON("a" << 1)
                                  << "unique"
                                  << true
                                  << "background"
                                  << background);

        ASSERT_OK(indexer.init(spec));
        const Status status = indexer.insertAllDocumentsInCollection();
        ASSERT_EQUALS(status.code(), ErrorCodes::DuplicateKey);
    }
};

/** Index creation fills a passed-in set of dups rather than failing. */
template <bool background>
class InsertBuildFillDups : public IndexBuildBase {
public:
    void run() {
        // Create a new collection.
        Database* db = _ctx.db();
        Collection* coll;
        RecordId loc1;
        RecordId loc2;
        {
            WriteUnitOfWork wunit(&_txn);
            db->dropCollection(&_txn, _ns);
            coll = db->createCollection(&_txn, _ns);

            OpDebug* const nullOpDebug = nullptr;
            ASSERT_OK(coll->insertDocument(&_txn,
                                           BSON("_id" << 1 << "a"
                                                      << "dup"),
                                           nullOpDebug,
                                           true));
            ASSERT_OK(coll->insertDocument(&_txn,
                                           BSON("_id" << 2 << "a"
                                                      << "dup"),
                                           nullOpDebug,
                                           true));
            wunit.commit();
        }

        MultiIndexBlock indexer(&_txn, coll);
        indexer.allowBackgroundBuilding();
        indexer.allowInterruption();
        // indexer.ignoreUniqueConstraint(); // not calling this

        const BSONObj spec = BSON("name"
                                  << "a"
                                  << "ns"
                                  << coll->ns().ns()
                                  << "key"
                                  << BSON("a" << 1)
                                  << "unique"
                                  << true
                                  << "background"
                                  << background);

        ASSERT_OK(indexer.init(spec));

        std::set<RecordId> dups;
        ASSERT_OK(indexer.insertAllDocumentsInCollection(&dups));

        // either loc1 or loc2 should be in dups but not both.
        ASSERT_EQUALS(dups.size(), 1U);
        for (auto recordId : dups) {
            ASSERT_NOT_EQUALS(recordId, RecordId());
            BSONObj obj = coll->docFor(&_txn, recordId).value();
            int id = obj["_id"].Int();
            ASSERT(id == 1 || id == 2);
        }
    }
};

/** Index creation is killed if mayInterrupt is true. */
class InsertBuildIndexInterrupt : public IndexBuildBase {
public:
    void run() {
        // Create a new collection.
        Database* db = _ctx.db();
        Collection* coll;
        {
            WriteUnitOfWork wunit(&_txn);
            db->dropCollection(&_txn, _ns);
            coll = db->createCollection(&_txn, _ns);
            // Drop all indexes including id index.
            coll->getIndexCatalog()->dropAllIndexes(&_txn, true);
            // Insert some documents with enforceQuota=true.
            int32_t nDocs = 1000;
            OpDebug* const nullOpDebug = nullptr;
            for (int32_t i = 0; i < nDocs; ++i) {
                coll->insertDocument(&_txn, BSON("a" << i), nullOpDebug, true);
            }
            wunit.commit();
        }
        // Request an interrupt.
        getGlobalServiceContext()->setKillAllOperations();
        BSONObj indexInfo = BSON("key" << BSON("a" << 1) << "ns" << _ns << "name"
                                       << "a_1");
        // The call is interrupted because mayInterrupt == true.
        ASSERT_TRUE(buildIndexInterrupted(indexInfo, true));
        // only want to interrupt the index build
        getGlobalServiceContext()->unsetKillAllOperations();
        // The new index is not listed in the index catalog because the index build failed.
        ASSERT(!coll->getIndexCatalog()->findIndexByName(&_txn, "a_1"));
    }
};

/** Index creation is not killed if mayInterrupt is false. */
class InsertBuildIndexInterruptDisallowed : public IndexBuildBase {
public:
    void run() {
        // Create a new collection.
        Database* db = _ctx.db();
        Collection* coll;
        {
            WriteUnitOfWork wunit(&_txn);
            db->dropCollection(&_txn, _ns);
            coll = db->createCollection(&_txn, _ns);
            coll->getIndexCatalog()->dropAllIndexes(&_txn, true);
            // Insert some documents.
            int32_t nDocs = 1000;
            OpDebug* const nullOpDebug = nullptr;
            for (int32_t i = 0; i < nDocs; ++i) {
                coll->insertDocument(&_txn, BSON("a" << i), nullOpDebug, true);
            }
            wunit.commit();
        }
        // Request an interrupt.
        getGlobalServiceContext()->setKillAllOperations();
        BSONObj indexInfo = BSON("key" << BSON("a" << 1) << "ns" << _ns << "name"
                                       << "a_1");
        // The call is not interrupted because mayInterrupt == false.
        ASSERT_FALSE(buildIndexInterrupted(indexInfo, false));
        // only want to interrupt the index build
        getGlobalServiceContext()->unsetKillAllOperations();
        // The new index is listed in the index catalog because the index build completed.
        ASSERT(coll->getIndexCatalog()->findIndexByName(&_txn, "a_1"));
    }
};

/** Index creation is killed when building the _id index. */
class InsertBuildIdIndexInterrupt : public IndexBuildBase {
public:
    void run() {
        // Recreate the collection as capped, without an _id index.
        Database* db = _ctx.db();
        Collection* coll;
        {
            WriteUnitOfWork wunit(&_txn);
            db->dropCollection(&_txn, _ns);
            CollectionOptions options;
            options.capped = true;
            options.cappedSize = 10 * 1024;
            coll = db->createCollection(&_txn, _ns, options);
            coll->getIndexCatalog()->dropAllIndexes(&_txn, true);
            // Insert some documents.
            int32_t nDocs = 1000;
            OpDebug* const nullOpDebug = nullptr;
            for (int32_t i = 0; i < nDocs; ++i) {
                coll->insertDocument(&_txn, BSON("_id" << i), nullOpDebug, true);
            }
            wunit.commit();
        }
        // Request an interrupt.
        getGlobalServiceContext()->setKillAllOperations();
        BSONObj indexInfo = BSON("key" << BSON("_id" << 1) << "ns" << _ns << "name"
                                       << "_id_");
        // The call is interrupted because mayInterrupt == true.
        ASSERT_TRUE(buildIndexInterrupted(indexInfo, true));
        // only want to interrupt the index build
        getGlobalServiceContext()->unsetKillAllOperations();
        // The new index is not listed in the index catalog because the index build failed.
        ASSERT(!coll->getIndexCatalog()->findIndexByName(&_txn, "_id_"));
    }
};

/** Index creation is not killed when building the _id index if mayInterrupt is false. */
class InsertBuildIdIndexInterruptDisallowed : public IndexBuildBase {
public:
    void run() {
        // Recreate the collection as capped, without an _id index.
        Database* db = _ctx.db();
        Collection* coll;
        {
            WriteUnitOfWork wunit(&_txn);
            db->dropCollection(&_txn, _ns);
            CollectionOptions options;
            options.capped = true;
            options.cappedSize = 10 * 1024;
            coll = db->createCollection(&_txn, _ns, options);
            coll->getIndexCatalog()->dropAllIndexes(&_txn, true);
            // Insert some documents.
            int32_t nDocs = 1000;
            OpDebug* const nullOpDebug = nullptr;
            for (int32_t i = 0; i < nDocs; ++i) {
                coll->insertDocument(&_txn, BSON("_id" << i), nullOpDebug, true);
            }
            wunit.commit();
        }
        // Request an interrupt.
        getGlobalServiceContext()->setKillAllOperations();
        BSONObj indexInfo = BSON("key" << BSON("_id" << 1) << "ns" << _ns << "name"
                                       << "_id_");
        // The call is not interrupted because mayInterrupt == false.
        ASSERT_FALSE(buildIndexInterrupted(indexInfo, false));
        // only want to interrupt the index build
        getGlobalServiceContext()->unsetKillAllOperations();
        // The new index is listed in the index catalog because the index build succeeded.
        ASSERT(coll->getIndexCatalog()->findIndexByName(&_txn, "_id_"));
    }
};

/** Helpers::ensureIndex() is not interrupted. */
class HelpersEnsureIndexInterruptDisallowed : public IndexBuildBase {
public:
    void run() {
        // Insert some documents.
        int32_t nDocs = 1000;
        for (int32_t i = 0; i < nDocs; ++i) {
            _client.insert(_ns, BSON("a" << i));
        }
        // Start with just _id
        ASSERT_EQUALS(1U, _client.getIndexSpecs(_ns).size());
        // Request an interrupt.
        getGlobalServiceContext()->setKillAllOperations();
        // The call is not interrupted.
        Helpers::ensureIndex(&_txn, collection(), BSON("a" << 1), false, "a_1");
        // only want to interrupt the index build
        getGlobalServiceContext()->unsetKillAllOperations();
        // The new index is listed in getIndexSpecs because the index build completed.
        ASSERT_EQUALS(2U, _client.getIndexSpecs(_ns).size());
    }
};
// QUERY_MIGRATION
#if 0
    class IndexBuildInProgressTest : public IndexBuildBase {
    public:
        void run() {

            NamespaceDetails* nsd = nsdetails( _ns );

            // _id_ is at 0, so nIndexes == 1
            IndexCatalog::IndexBuildBlock* a = halfAddIndex("a");
            IndexCatalog::IndexBuildBlock* b = halfAddIndex("b");
            IndexCatalog::IndexBuildBlock* c = halfAddIndex("c");
            IndexCatalog::IndexBuildBlock* d = halfAddIndex("d");
            int offset = nsd->_catalogFindIndexByName( "b_1", true );
            ASSERT_EQUALS(2, offset);

            delete b;

            ASSERT_EQUALS(2, nsd->_catalogFindIndexByName( "c_1", true ) );
            ASSERT_EQUALS(3, nsd->_catalogFindIndexByName( "d_1", true ) );

            offset = nsd->_catalogFindIndexByName( "d_1", true );
            delete d;

            ASSERT_EQUALS(2, nsd->_catalogFindIndexByName( "c_1", true ) );
            ASSERT( nsd->_catalogFindIndexByName( "d_1", true ) < 0 );

            offset = nsd->_catalogFindIndexByName( "a_1", true );
            delete a;

            ASSERT_EQUALS(1, nsd->_catalogFindIndexByName( "c_1", true ));
            delete c;
        }

    private:
        IndexCatalog::IndexBuildBlock* halfAddIndex(const std::string& key) {
            string name = key + "_1";
            BSONObj indexInfo = BSON( "v" << 1 <<
                                      "key" << BSON( key << 1 ) <<
                                      "ns" << _ns <<
                                      "name" << name );
            int32_t lenWHdr = indexInfo.objsize() + Record::HeaderSize;
            const char* systemIndexes = "unittests.system.indexes";
            RecordId infoLoc = allocateSpaceForANewRecord( systemIndexes,
                                                          nsdetails( systemIndexes ),
                                                          lenWHdr,
                                                          false );
            Record* infoRecord = reinterpret_cast<Record*>( getDur().writingPtr( infoLoc.rec(),
                                                                                 lenWHdr ) );
            memcpy( infoRecord->data(), indexInfo.objdata(), indexInfo.objsize() );
            addRecordToRecListInExtent( infoRecord, infoLoc );

            return new IndexCatalog::IndexBuildBlock( _ctx.getCollection()->getIndexCatalog(),
                                                     name,
                                                     infoLoc );
        }
    };
#endif

Status IndexBuildBase::createIndex(const std::string& dbname, const BSONObj& indexSpec) {
    MultiIndexBlock indexer(&_txn, collection());
    Status status = indexer.init(indexSpec);
    if (status == ErrorCodes::IndexAlreadyExists) {
        return Status::OK();
    }
    if (!status.isOK()) {
        return status;
    }
    status = indexer.insertAllDocumentsInCollection();
    if (!status.isOK()) {
        return status;
    }
    WriteUnitOfWork wunit(&_txn);
    indexer.commit();
    wunit.commit();
    return Status::OK();
}

/**
 * Fixture class that has a basic compound index.
 */
class SimpleCompoundIndex : public IndexBuildBase {
public:
    SimpleCompoundIndex() {
        ASSERT_OK(createIndex("unittest",
                              BSON("name"
                                   << "x"
                                   << "ns"
                                   << _ns
                                   << "key"
                                   << BSON("x" << 1 << "y" << 1))));
    }
};

class SameSpecDifferentOption : public SimpleCompoundIndex {
public:
    void run() {
        // Cannot have same key spec with an option different from the existing one.
        ASSERT_EQUALS(ErrorCodes::IndexOptionsConflict,
                      createIndex("unittest",
                                  BSON("name"
                                       << "x"
                                       << "ns"
                                       << _ns
                                       << "unique"
                                       << true
                                       << "key"
                                       << BSON("x" << 1 << "y" << 1))));
    }
};

class SameSpecSameOptions : public SimpleCompoundIndex {
public:
    void run() {
        ASSERT_OK(createIndex("unittest",
                              BSON("name"
                                   << "x"
                                   << "ns"
                                   << _ns
                                   << "key"
                                   << BSON("x" << 1 << "y" << 1))));
    }
};

class DifferentSpecSameName : public SimpleCompoundIndex {
public:
    void run() {
        // Cannot create a different index with the same name as the existing one.
        ASSERT_EQUALS(ErrorCodes::IndexKeySpecsConflict,
                      createIndex("unittest",
                                  BSON("name"
                                       << "x"
                                       << "ns"
                                       << _ns
                                       << "key"
                                       << BSON("y" << 1 << "x" << 1))));
    }
};

/**
 * Fixture class for indexes with complex options.
 */
class ComplexIndex : public IndexBuildBase {
public:
    ComplexIndex() {
        ASSERT_OK(createIndex("unittests",
                              BSON("name"
                                   << "super"
                                   << "ns"
                                   << _ns
                                   << "unique"
                                   << 1
                                   << "sparse"
                                   << true
                                   << "expireAfterSeconds"
                                   << 3600
                                   << "key"
                                   << BSON("superIdx"
                                           << "2d"))));
    }
};

class SameSpecSameOptionDifferentOrder : public ComplexIndex {
public:
    void run() {
        // Exactly the same specs with the existing one, only
        // specified in a different order than the original.
        ASSERT_OK(createIndex("unittests",
                              BSON("name"
                                   << "super2"
                                   << "ns"
                                   << _ns
                                   << "expireAfterSeconds"
                                   << 3600
                                   << "sparse"
                                   << true
                                   << "unique"
                                   << 1
                                   << "key"
                                   << BSON("superIdx"
                                           << "2d"))));
    }
};

// The following tests tries to create an index with almost the same
// specs as the original, except for one option.

class SameSpecDifferentUnique : public ComplexIndex {
public:
    void run() {
        ASSERT_EQUALS(ErrorCodes::IndexOptionsConflict,
                      createIndex("unittest",
                                  BSON("name"
                                       << "super2"
                                       << "ns"
                                       << _ns
                                       << "unique"
                                       << false
                                       << "sparse"
                                       << true
                                       << "expireAfterSeconds"
                                       << 3600
                                       << "key"
                                       << BSON("superIdx"
                                               << "2d"))));
    }
};

class SameSpecDifferentSparse : public ComplexIndex {
public:
    void run() {
        ASSERT_EQUALS(ErrorCodes::IndexOptionsConflict,
                      createIndex("unittest",
                                  BSON("name"
                                       << "super2"
                                       << "ns"
                                       << _ns
                                       << "unique"
                                       << 1
                                       << "sparse"
                                       << false
                                       << "background"
                                       << true
                                       << "expireAfterSeconds"
                                       << 3600
                                       << "key"
                                       << BSON("superIdx"
                                               << "2d"))));
    }
};

class SameSpecDifferentTTL : public ComplexIndex {
public:
    void run() {
        ASSERT_EQUALS(ErrorCodes::IndexOptionsConflict,
                      createIndex("unittest",
                                  BSON("name"
                                       << "super2"
                                       << "ns"
                                       << _ns
                                       << "unique"
                                       << 1
                                       << "sparse"
                                       << true
                                       << "expireAfterSeconds"
                                       << 2400
                                       << "key"
                                       << BSON("superIdx"
                                               << "2d"))));
    }
};

class StorageEngineOptions : public IndexBuildBase {
public:
    void run() {
        // "storageEngine" field has to be an object if present.
        ASSERT_NOT_OK(createIndex("unittest", _createSpec(12345)));

        // 'storageEngine' must not be empty.
        ASSERT_NOT_OK(createIndex("unittest", _createSpec(BSONObj())));

        // Every field under "storageEngine" must match a registered storage engine.
        ASSERT_NOT_OK(createIndex("unittest", _createSpec(BSON("unknownEngine" << BSONObj()))));

        // Testing with 'wiredTiger' because the registered storage engine factory
        // supports custom index options under 'storageEngine'.
        const std::string storageEngineName = "wiredTiger";

        // Run 'wiredTiger' tests if the storage engine is supported.
        if (getGlobalServiceContext()->isRegisteredStorageEngine(storageEngineName)) {
            // Every field under "storageEngine" has to be an object.
            ASSERT_NOT_OK(createIndex("unittest", _createSpec(BSON(storageEngineName << 1))));

            // Storage engine options must pass validation by the storage engine factory.
            // For 'wiredTiger', embedded document must contain 'configString'.
            ASSERT_NOT_OK(createIndex(
                "unittest", _createSpec(BSON(storageEngineName << BSON("unknown" << 1)))));

            // Configuration string for 'wiredTiger' must be a string.
            ASSERT_NOT_OK(createIndex(
                "unittest", _createSpec(BSON(storageEngineName << BSON("configString" << 1)))));

            // Valid 'wiredTiger' configuration.
            ASSERT_OK(createIndex(
                "unittest",
                _createSpec(BSON(storageEngineName << BSON("configString"
                                                           << "block_compressor=zlib")))));
        }
    }

protected:
    template <typename T>
    BSONObj _createSpec(T storageEngineValue) {
        return BSON("name"
                    << "super2"
                    << "ns"
                    << _ns
                    << "key"
                    << BSON("a" << 1)
                    << "storageEngine"
                    << storageEngineValue);
    }
};

class IndexCatatalogFixIndexKey {
public:
    void run() {
        ASSERT_EQUALS(BSON("x" << 1), IndexCatalog::fixIndexKey(BSON("x" << 1)));

        ASSERT_EQUALS(BSON("_id" << 1), IndexCatalog::fixIndexKey(BSON("_id" << 1)));

        ASSERT_EQUALS(BSON("_id" << 1), IndexCatalog::fixIndexKey(BSON("_id" << true)));
    }
};

class IndexUpdateTests : public Suite {
public:
    IndexUpdateTests() : Suite("indexupdate") {}

    void setupTests() {
        // add<AddKeysToPhaseOne>();
        // add<InterruptAddKeysToPhaseOne>( false );
        // add<InterruptAddKeysToPhaseOne>( true );
        // QUERY_MIGRATION
        // add<BuildBottomUp>();
        // add<InterruptBuildBottomUp>( false );
        // add<InterruptBuildBottomUp>( true );
        add<InsertBuildIgnoreUnique<true>>();
        add<InsertBuildIgnoreUnique<false>>();
        add<InsertBuildEnforceUnique<true>>();
        add<InsertBuildEnforceUnique<false>>();
        add<InsertBuildFillDups<true>>();
        add<InsertBuildFillDups<false>>();
        add<InsertBuildIndexInterrupt>();
        add<InsertBuildIndexInterruptDisallowed>();
        add<InsertBuildIdIndexInterrupt>();
        add<InsertBuildIdIndexInterruptDisallowed>();
        add<HelpersEnsureIndexInterruptDisallowed>();
        // add<IndexBuildInProgressTest>();
        add<SameSpecDifferentOption>();
        add<SameSpecSameOptions>();
        add<DifferentSpecSameName>();
        add<SameSpecSameOptionDifferentOrder>();
        add<SameSpecDifferentUnique>();
        add<SameSpecDifferentSparse>();
        add<SameSpecDifferentTTL>();
        add<StorageEngineOptions>();

        add<IndexCatatalogFixIndexKey>();
    }
} indexUpdateTests;

}  // namespace IndexUpdateTests
