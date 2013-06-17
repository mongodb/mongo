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
 */

#include "mongo/db/index_update.h"

#include "mongo/db/btree.h"
#include "mongo/db/btreecursor.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/btree_based_builder.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/sort_phase_one.h"
#include "mongo/platform/cstdint.h"

#include "mongo/dbtests/dbtests.h"

namespace IndexUpdateTests {

    static const char* const _ns = "unittests.indexupdate";
    DBDirectClient _client;
    ExternalSortComparison* _aFirstSort = BtreeBasedBuilder::getComparison(0, BSON("a" << 1));

    /**
     * Test fixture for a write locked test using collection _ns.  Includes functionality to
     * partially construct a new IndexDetails in a manner that supports proper cleanup in
     * dropCollection().
     */
    class IndexBuildBase {
    public:
        IndexBuildBase() :
            _ctx( _ns ) {
            _client.createCollection( _ns );
        }
        ~IndexBuildBase() {
            _client.dropCollection( _ns );
            killCurrentOp.reset();
        }
    protected:
        /** @return IndexDetails for a new index on a:1, with the info field populated. */
        IndexDetails& addIndexWithInfo() {
            BSONObj indexInfo = BSON( "v" << 1 <<
                                      "key" << BSON( "a" << 1 ) <<
                                      "ns" << _ns <<
                                      "name" << "a_1" );
            int32_t lenWHdr = indexInfo.objsize() + Record::HeaderSize;
            const char* systemIndexes = "unittests.system.indexes";
            DiskLoc infoLoc = allocateSpaceForANewRecord( systemIndexes,
                                                          nsdetails( systemIndexes ),
                                                          lenWHdr,
                                                          false );
            Record* infoRecord = reinterpret_cast<Record*>( getDur().writingPtr( infoLoc.rec(),
                                                                                 lenWHdr ) );
            memcpy( infoRecord->data(), indexInfo.objdata(), indexInfo.objsize() );
            addRecordToRecListInExtent( infoRecord, infoLoc );
            IndexDetails& id = nsdetails( _ns )->getNextIndexDetails( _ns );
            nsdetails( _ns )->addIndex( _ns );
            id.info.writing() = infoLoc;
            return id;
        }
    private:
        Client::WriteContext _ctx;
    };

    /** addKeysToPhaseOne() adds keys from a collection's documents to an external sorter. */
    class AddKeysToPhaseOne : public IndexBuildBase {
    public:
        void run() {
            // Add some data to the collection.
            int32_t nDocs = 130;
            for( int32_t i = 0; i < nDocs; ++i ) {
                _client.insert( _ns, BSON( "a" << i ) );
            }
            IndexDetails& id = addIndexWithInfo();
            // Create a SortPhaseOne.
            SortPhaseOne phaseOne;
            ProgressMeterHolder pm (cc().curop()->setMessage("AddKeysToPhaseOne",
                                                             "AddKeysToPhaseOne Progress",
                                                             nDocs,
                                                             nDocs));
            // Add keys to phaseOne.
            BtreeBasedBuilder::addKeysToPhaseOne( nsdetails(_ns), _ns, id, BSON( "a" << 1 ), &phaseOne, nDocs, pm.get(), true,
                                                 nsdetails(_ns)->idxNo(id) );
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
            IndexDetails& id = addIndexWithInfo();
            // Create a SortPhaseOne.
            SortPhaseOne phaseOne;
            ProgressMeterHolder pm (cc().curop()->setMessage("InterruptAddKeysToPhaseOne",
                                                             "InterruptAddKeysToPhaseOne Progress",
                                                             nDocs,
                                                             nDocs));
            // Register a request to kill the current operation.
            cc().curop()->kill();
            if ( _mayInterrupt ) {
                // Add keys to phaseOne.
                ASSERT_THROWS( BtreeBasedBuilder::addKeysToPhaseOne( nsdetails(_ns), _ns,
                                                  id,
                                                  BSON( "a" << 1 ),
                                                  &phaseOne,
                                                  nDocs,
                                                  pm.get(),
                                                  _mayInterrupt,
                                                  nsdetails(_ns)->idxNo(id) ),
                               UserException );
                // Not all keys were added to phaseOne due to the interrupt.
                ASSERT( static_cast<uint64_t>( nDocs ) > phaseOne.n );
            }
            else {
                // Add keys to phaseOne.
                BtreeBasedBuilder::addKeysToPhaseOne( nsdetails(_ns), _ns,
                                   id,
                                   BSON( "a" << 1 ),
                                   &phaseOne,
                                   nDocs,
                                   pm.get(),
                                   _mayInterrupt, nsdetails(_ns)->idxNo(id) );
                // All keys were added to phaseOne despite to the kill request, because
                // mayInterrupt == false.
                ASSERT_EQUALS( static_cast<uint64_t>( nDocs ), phaseOne.n );
            }
        }
    private:
        bool _mayInterrupt;
    };

    /** buildBottomUpPhases2And3() builds a btree from the keys in an external sorter. */
    class BuildBottomUp : public IndexBuildBase {
    public:
        void run() {
            IndexDetails& id = addIndexWithInfo();
            // Create a SortPhaseOne.
            SortPhaseOne phaseOne;
            phaseOne.sorter.reset( new BSONObjExternalSorter(_aFirstSort));
            // Add index keys to the phaseOne.
            int32_t nKeys = 130;
            for( int32_t i = 0; i < nKeys; ++i ) {
                phaseOne.sorter->add( BSON( "a" << i ), /* dummy disk loc */ DiskLoc(), false );
            }
            phaseOne.nkeys = phaseOne.n = nKeys;
            phaseOne.sorter->sort( false );
            // Set up remaining arguments.
            set<DiskLoc> dups;
            CurOp* op = cc().curop();
            ProgressMeterHolder pm (op->setMessage("BuildBottomUp",
                                                   "BuildBottomUp Progress",
                                                   nKeys,
                                                   nKeys));
            pm.finished();
            Timer timer;
            // The index's root has not yet been set.
            ASSERT( id.head.isNull() );
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
            ASSERT( !id.head.isNull() );
            // Create a cursor over the index.
            scoped_ptr<BtreeCursor> cursor(
                    BtreeCursor::make( nsdetails( _ns ),
                                       id,
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

    /** buildBottomUpPhases2And3() aborts if the current operation is interrupted. */
    class InterruptBuildBottomUp : public IndexBuildBase {
    public:
        InterruptBuildBottomUp( bool mayInterrupt ) :
            _mayInterrupt( mayInterrupt ) {
        }
        void run() {
            IndexDetails& id = addIndexWithInfo();
            // Create a SortPhaseOne.
            SortPhaseOne phaseOne;
            phaseOne.sorter.reset(new BSONObjExternalSorter(_aFirstSort));
            // It's necessary to index sufficient keys that a RARELY condition will be triggered,
            // but few enough keys that the btree builder will not create an internal node and check
            // for an interrupt internally (which would cause this test to pass spuriously).
            int32_t nKeys = 130;
            // Add index keys to the phaseOne.
            for( int32_t i = 0; i < nKeys; ++i ) {
                phaseOne.sorter->add( BSON( "a" << i ), /* dummy disk loc */ DiskLoc(), false );
            }
            phaseOne.nkeys = phaseOne.n = nKeys;
            phaseOne.sorter->sort( false );
            // Set up remaining arguments.
            set<DiskLoc> dups;
            CurOp* op = cc().curop();
            ProgressMeterHolder pm (op->setMessage("InterruptBuildBottomUp",
                                                   "InterruptBuildBottomUp Progress",
                                                   nKeys,
                                                   nKeys));
            pm.finished();
            Timer timer;
            // The index's root has not yet been set.
            ASSERT( id.head.isNull() );
            // Register a request to kill the current operation.
            cc().curop()->kill();
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
                ASSERT( id.head.isNull() );
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
                ASSERT( !id.head.isNull() );
            }
        }
    private:
        bool _mayInterrupt;
    };

    /** doDropDups() deletes the duplicate documents in the provided set. */
    class DoDropDups : public IndexBuildBase {
    public:
        void run() {
            // Insert some documents.
            int32_t nDocs = 1000;
            for( int32_t i = 0; i < nDocs; ++i ) {
                _client.insert( _ns, BSON( "a" << ( i / 4 ) ) );
            }
            // Find the documents that are dups.
            set<DiskLoc> dups;
            int32_t last = -1;
            for( boost::shared_ptr<Cursor> cursor = theDataFileMgr.findAll( _ns );
                 cursor->ok();
                 cursor->advance() ) {
                int32_t currA = cursor->current()[ "a" ].Int();
                if ( currA == last ) {
                    dups.insert( cursor->currLoc() );
                }
                last = currA;
            }
            // Check the expected number of dups.
            ASSERT_EQUALS( static_cast<uint32_t>( nDocs / 4 * 3 ), dups.size() );
            // Drop the dups.
            BtreeBasedBuilder::doDropDups( _ns, nsdetails( _ns ), dups, true );
            // Check that the expected number of documents remain.
            ASSERT_EQUALS( static_cast<uint32_t>( nDocs / 4 ), _client.count( _ns ) );
        }
    };

    /** doDropDups() aborts if the current operation is interrupted. */
    class InterruptDoDropDups : public IndexBuildBase {
    public:
        InterruptDoDropDups( bool mayInterrupt ) :
            _mayInterrupt( mayInterrupt ) {
        }
        void run() {
            // Insert some documents.
            int32_t nDocs = 1000;
            for( int32_t i = 0; i < nDocs; ++i ) {
                _client.insert( _ns, BSON( "a" << ( i / 4 ) ) );
            }
            // Find the documents that are dups.
            set<DiskLoc> dups;
            int32_t last = -1;
            for( boost::shared_ptr<Cursor> cursor = theDataFileMgr.findAll( _ns );
                 cursor->ok();
                 cursor->advance() ) {
                int32_t currA = cursor->current()[ "a" ].Int();
                if ( currA == last ) {
                    dups.insert( cursor->currLoc() );
                }
                last = currA;
            }
            // Check the expected number of dups.  There must be enough to trigger a RARELY
            // condition when deleting them.
            ASSERT_EQUALS( static_cast<uint32_t>( nDocs / 4 * 3 ), dups.size() );
            // Kill the current operation.
            cc().curop()->kill();
            if ( _mayInterrupt ) {
                // doDropDups() aborts.
                ASSERT_THROWS( BtreeBasedBuilder::doDropDups( _ns, nsdetails( _ns ), dups, _mayInterrupt ),
                               UserException );
                // Not all dups are dropped.
                ASSERT( static_cast<uint32_t>( nDocs / 4 ) < _client.count( _ns ) );
            }
            else {
                // doDropDups() succeeds.
                BtreeBasedBuilder::doDropDups( _ns, nsdetails( _ns ), dups, _mayInterrupt );
                // The expected number of documents were dropped.
                ASSERT_EQUALS( static_cast<uint32_t>( nDocs / 4 ), _client.count( _ns ) );
            }
        }
    private:
        bool _mayInterrupt;
    };

    /** DataFileMgr::insertWithObjMod is killed if mayInterrupt is true. */
    class InsertBuildIndexInterrupt : public IndexBuildBase {
    public:
        void run() {
            // Insert some documents.
            int32_t nDocs = 1000;
            for( int32_t i = 0; i < nDocs; ++i ) {
                _client.insert( _ns, BSON( "a" << i ) );
            }
            // Initialize curop.
            cc().curop()->reset();
            // Request an interrupt.
            killCurrentOp.killAll();
            BSONObj indexInfo = BSON( "key" << BSON( "a" << 1 ) << "ns" << _ns << "name" << "a_1" );
            // The call is interrupted because mayInterrupt == true.
            ASSERT_THROWS( theDataFileMgr.insertWithObjMod( "unittests.system.indexes",
                                                            indexInfo,
                                                            true ),
                           UserException );
            // The new index is not listed in system.indexes because the index build failed.
            ASSERT_EQUALS( 0U,
                           _client.count( "unittests.system.indexes",
                                          BSON( "ns" << _ns << "name" << "a_1" ) ) );
        }
    };

    /** DataFileMgr::insertWithObjMod is not killed if mayInterrupt is false. */
    class InsertBuildIndexInterruptDisallowed : public IndexBuildBase {
    public:
        void run() {
            // Insert some documents.
            int32_t nDocs = 1000;
            for( int32_t i = 0; i < nDocs; ++i ) {
                _client.insert( _ns, BSON( "a" << i ) );
            }
            // Initialize curop.
            cc().curop()->reset();
            // Request an interrupt.
            killCurrentOp.killAll();
            BSONObj indexInfo = BSON( "key" << BSON( "a" << 1 ) << "ns" << _ns << "name" << "a_1" );
            // The call is not interrupted because mayInterrupt == false.
            theDataFileMgr.insertWithObjMod( "unittests.system.indexes", indexInfo, false );
            // The new index is listed in system.indexes because the index build completed.
            ASSERT_EQUALS( 1U,
                           _client.count( "unittests.system.indexes",
                                          BSON( "ns" << _ns << "name" << "a_1" ) ) );
        }
    };

    /** DataFileMgr::insertWithObjMod is killed when building the _id index. */
    class InsertBuildIdIndexInterrupt : public IndexBuildBase {
    public:
        void run() {
            // Recreate the collection as capped, without an _id index.
            _client.dropCollection( _ns );
            BSONObj info;
            ASSERT( _client.runCommand( "unittests",
                                        BSON( "create" << "indexupdate" <<
                                              "capped" << true <<
                                              "size" << ( 10 * 1024 ) <<
                                              "autoIndexId" << false ),
                                        info ) );
            // Insert some documents.
            int32_t nDocs = 1000;
            for( int32_t i = 0; i < nDocs; ++i ) {
                _client.insert( _ns, BSON( "_id" << i ) );
            }
            // Initialize curop.
            cc().curop()->reset();
            // Request an interrupt.
            killCurrentOp.killAll();
            BSONObj indexInfo = BSON( "key" << BSON( "_id" << 1 ) <<
                                      "ns" << _ns <<
                                      "name" << "_id" );
            // The call is interrupted because mayInterrupt == true.
            ASSERT_THROWS( theDataFileMgr.insertWithObjMod( "unittests.system.indexes",
                                                            indexInfo,
                                                            true ),
                           UserException );
            // The new index is not listed in system.indexes because the index build failed.
            ASSERT_EQUALS( 0U, _client.count( "unittests.system.indexes", BSON( "ns" << _ns ) ) );
        }
    };

    /**
     * DataFileMgr::insertWithObjMod is not killed when building the _id index if mayInterrupt is
     * false.
     */
    class InsertBuildIdIndexInterruptDisallowed : public IndexBuildBase {
    public:
        void run() {
            // Recreate the collection as capped, without an _id index.
            _client.dropCollection( _ns );
            BSONObj info;
            ASSERT( _client.runCommand( "unittests",
                                        BSON( "create" << "indexupdate" <<
                                              "capped" << true <<
                                              "size" << ( 10 * 1024 ) <<
                                              "autoIndexId" << false ),
                                        info ) );
            // Insert some documents.
            int32_t nDocs = 1000;
            for( int32_t i = 0; i < nDocs; ++i ) {
                _client.insert( _ns, BSON( "_id" << i ) );
            }
            // Initialize curop.
            cc().curop()->reset();
            // Request an interrupt.
            killCurrentOp.killAll();
            BSONObj indexInfo = BSON( "key" << BSON( "_id" << 1 ) <<
                                      "ns" << _ns <<
                                      "name" << "_id" );
            // The call is not interrupted because mayInterrupt == false.
            theDataFileMgr.insertWithObjMod( "unittests.system.indexes", indexInfo, false );
            // The new index is listed in system.indexes because the index build succeeded.
            ASSERT_EQUALS( 1U, _client.count( "unittests.system.indexes", BSON( "ns" << _ns ) ) );
        }
    };

    /** DBDirectClient::ensureIndex() is not interrupted. */
    class DirectClientEnsureIndexInterruptDisallowed : public IndexBuildBase {
    public:
        void run() {
            // Insert some documents.
            int32_t nDocs = 1000;
            for( int32_t i = 0; i < nDocs; ++i ) {
                _client.insert( _ns, BSON( "a" << i ) );
            }
            // Initialize curop.
            cc().curop()->reset();
            // Request an interrupt.  killAll() rather than kill() is required because the direct
            // client will build the index using a new opid.
            killCurrentOp.killAll();
            // The call is not interrupted.
            _client.ensureIndex( _ns, BSON( "a" << 1 ) );
            // The new index is listed in system.indexes because the index build completed.
            ASSERT_EQUALS( 1U,
                           _client.count( "unittests.system.indexes",
                                          BSON( "ns" << _ns << "name" << "a_1" ) ) );
        }
    };

    /** Helpers::ensureIndex() is not interrupted. */
    class HelpersEnsureIndexInterruptDisallowed : public IndexBuildBase {
    public:
        void run() {
            // Insert some documents.
            int32_t nDocs = 1000;
            for( int32_t i = 0; i < nDocs; ++i ) {
                _client.insert( _ns, BSON( "a" << i ) );
            }
            // Initialize curop.
            cc().curop()->reset();
            // Request an interrupt.
            killCurrentOp.killAll();
            // The call is not interrupted.
            Helpers::ensureIndex( _ns, BSON( "a" << 1 ), false, "a_1" );
            // The new index is listed in system.indexes because the index build completed.
            ASSERT_EQUALS( 1U,
                           _client.count( "unittests.system.indexes",
                                          BSON( "ns" << _ns << "name" << "a_1" ) ) );
        }
    };

    class IndexBuildInProgressTest : public IndexBuildBase {
    public:
        void run() {
            // _id_ is at 0, so nIndexes == 1
            NamespaceDetails::IndexBuildBlock* a = halfAddIndex("a");
            NamespaceDetails::IndexBuildBlock* b = halfAddIndex("b");
            NamespaceDetails::IndexBuildBlock* c = halfAddIndex("c");
            NamespaceDetails::IndexBuildBlock* d = halfAddIndex("d");
            int offset = IndexBuildsInProgress::get(_ns, "b_1");
            ASSERT_EQUALS(2, offset);

            IndexBuildsInProgress::remove(_ns, offset);
            delete b;

            ASSERT_EQUALS(2, IndexBuildsInProgress::get(_ns, "c_1"));
            ASSERT_EQUALS(3, IndexBuildsInProgress::get(_ns, "d_1"));

            offset = IndexBuildsInProgress::get(_ns, "d_1");
            IndexBuildsInProgress::remove(_ns, offset);
            delete d;

            ASSERT_EQUALS(2, IndexBuildsInProgress::get(_ns, "c_1"));
            ASSERT_THROWS(IndexBuildsInProgress::get(_ns, "d_1"), MsgAssertionException);

            offset = IndexBuildsInProgress::get(_ns, "a_1");
            IndexBuildsInProgress::remove(_ns, offset);
            delete a;

            ASSERT_EQUALS(1, IndexBuildsInProgress::get(_ns, "c_1"));
            delete c;
        }

    private:
        NamespaceDetails::IndexBuildBlock* halfAddIndex(const std::string& key) {
            string name = key + "_1";
            BSONObj indexInfo = BSON( "v" << 1 <<
                                      "key" << BSON( key << 1 ) <<
                                      "ns" << _ns <<
                                      "name" << name );
            int32_t lenWHdr = indexInfo.objsize() + Record::HeaderSize;
            const char* systemIndexes = "unittests.system.indexes";
            DiskLoc infoLoc = allocateSpaceForANewRecord( systemIndexes,
                                                          nsdetails( systemIndexes ),
                                                          lenWHdr,
                                                          false );
            Record* infoRecord = reinterpret_cast<Record*>( getDur().writingPtr( infoLoc.rec(),
                                                                                 lenWHdr ) );
            memcpy( infoRecord->data(), indexInfo.objdata(), indexInfo.objsize() );
            addRecordToRecListInExtent( infoRecord, infoLoc );
            IndexDetails& id = nsdetails( _ns )->getNextIndexDetails( _ns );
            id.info = infoLoc;
            return new NamespaceDetails::IndexBuildBlock( _ns, name );
        }
    };

    /**
     * Fixture class that has a basic compound index.
     */
    class SimpleCompoundIndex: public IndexBuildBase {
    public:
        SimpleCompoundIndex() {
            _client.insert("unittests.system.indexes",
                    BSON("name" << "x"
                         << "ns" << _ns
                         << "key" << BSON("x" << 1 << "y" << 1)));
        }
    };

    class SameSpecDifferentOption: public SimpleCompoundIndex {
    public:
        void run() {
            _client.insert("unittests.system.indexes",
                    BSON("name" << "x"
                         << "ns" << _ns
                         << "unique" << true
                         << "key" << BSON("x" << 1 << "y" << 1)));
            // Cannot have same key spec with an option different from the existing one.
            ASSERT_NOT_EQUALS(_client.getLastError(), "");
        }
    };

    class SameSpecSameOptions: public SimpleCompoundIndex {
    public:
        void run() {
            _client.insert("unittests.system.indexes",
                    BSON("name" << "x"
                         << "ns" << _ns
                         << "key" << BSON("x" << 1 << "y" << 1)));
            // It is okay to try to create an index with the exact same specs (will be
            // ignored, but should not raise an error).
            ASSERT_EQUALS(_client.getLastError(), "");
        }
    };

    class DifferentSpecSameName: public SimpleCompoundIndex {
    public:
        void run() {
            _client.insert("unittests.system.indexes",
                    BSON("name" << "x"
                         << "ns" << _ns
                         << "key" << BSON("y" << 1 << "x" << 1)));
            // Cannot create a different index with the same name as the existing one.
            ASSERT_NOT_EQUALS(_client.getLastError(), "");
        }
    };

    /**
     * Fixture class for indexes with complex options.
     */
    class ComplexIndex: public IndexBuildBase {
    public:
        ComplexIndex() {
            _client.insert("unittests.system.indexes",
                    BSON("name" << "super"
                         << "ns" << _ns
                         << "unique" << 1
                         << "dropDups" << true
                         << "sparse" << true
                         << "expireAfterSeconds" << 3600
                         << "key" << BSON("superIdx" << "2d")));
        }
    };

    class SameSpecSameOptionDifferentOrder: public ComplexIndex {
    public:
        void run() {
            // Exactly the same specs with the existing one, only
            // specified in a different order than the original.
            _client.insert("unittests.system.indexes",
                    BSON("name" << "super2"
                         << "ns" << _ns
                         << "expireAfterSeconds" << 3600
                         << "sparse" << true
                         << "unique" << 1
                         << "dropDups" << true
                         << "key" << BSON("superIdx" << "2d")));
            ASSERT_EQUALS(_client.getLastError(), "");
        }
    };

    // The following tests tries to create an index with almost the same
    // specs as the original, except for one option.

    class SameSpecDifferentUnique: public ComplexIndex {
    public:
        void run() {
            _client.insert("unittests.system.indexes",
                    BSON("name" << "super2"
                         << "ns" << _ns
                         << "unique" << false
                         << "dropDups" << true
                         << "sparse" << true
                         << "expireAfterSeconds" << 3600
                         << "key" << BSON("superIdx" << "2d")));
            ASSERT_NOT_EQUALS(_client.getLastError(), "");
        }
    };

    class SameSpecDifferentDropDups: public ComplexIndex {
    public:
        void run() {
            _client.insert("unittests.system.indexes",
                    BSON("name" << "super2"
                         << "ns" << _ns
                         << "unique" << 1
                         << "dropDups" << false
                         << "sparse" << true
                         << "expireAfterSeconds" << 3600
                         << "key" << BSON("superIdx" << "2d")));
            ASSERT_NOT_EQUALS(_client.getLastError(), "");
        }
    };

    class SameSpecDifferentSparse: public ComplexIndex {
    public:
        void run() {
            _client.insert("unittests.system.indexes",
                    BSON("name" << "super2"
                         << "ns" << _ns
                         << "unique" << 1
                         << "dropDups" << true
                         << "sparse" << false
                         << "background" << true
                         << "expireAfterSeconds" << 3600
                         << "key" << BSON("superIdx" << "2d")));
            ASSERT_NOT_EQUALS(_client.getLastError(), "");
        }
    };

    class SameSpecDifferentTTL: public ComplexIndex {
    public:
        void run() {
            _client.insert("unittests.system.indexes",
                    BSON("name" << "super2"
                         << "ns" << _ns
                         << "unique" << 1
                         << "dropDups" << true
                         << "sparse" << true
                         << "expireAfterSeconds" << 2400
                         << "key" << BSON("superIdx" << "2d")));
            ASSERT_NOT_EQUALS(_client.getLastError(), "");
        }
    };

    class IndexUpdateTests : public Suite {
    public:
        IndexUpdateTests() :
            Suite( "indexupdate" ) {
        }

        void setupTests() {
            add<AddKeysToPhaseOne>();
            add<InterruptAddKeysToPhaseOne>( false );
            add<InterruptAddKeysToPhaseOne>( true );
            add<BuildBottomUp>();
            add<InterruptBuildBottomUp>( false );
            add<InterruptBuildBottomUp>( true );
            add<DoDropDups>();
            add<InterruptDoDropDups>( false );
            add<InterruptDoDropDups>( true );
            add<InsertBuildIndexInterrupt>();
            add<InsertBuildIndexInterruptDisallowed>();
            add<InsertBuildIdIndexInterrupt>();
            add<InsertBuildIdIndexInterruptDisallowed>();
            add<DirectClientEnsureIndexInterruptDisallowed>();
            add<HelpersEnsureIndexInterruptDisallowed>();
            add<IndexBuildInProgressTest>();
            add<SameSpecDifferentOption>();
            add<SameSpecSameOptions>();
            add<DifferentSpecSameName>();
            add<SameSpecSameOptionDifferentOrder>();
            add<SameSpecDifferentUnique>();
            add<SameSpecDifferentDropDups>();
            add<SameSpecDifferentSparse>();
            add<SameSpecDifferentTTL>();
        }
    } indexUpdateTests;

} // namespace IndexUpdateTests
