// namespacetests.cpp : namespace.{h,cpp} unit tests.
//

/**
 *    Copyright (C) 2008-2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/expression_keys_private.h"
#include "mongo/db/index_legacy.h"
#include "mongo/db/index_names.h"
#include "mongo/db/json.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/storage/mmap_v1/catalog/namespace.h"
#include "mongo/db/storage/mmap_v1/catalog/namespace_details.h"
#include "mongo/db/storage/mmap_v1/catalog/namespace_details_rsv1_metadata.h"
#include "mongo/db/storage/mmap_v1/extent.h"
#include "mongo/db/storage/mmap_v1/extent_manager.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_extent_manager.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_capped.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_simple.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/log.h"

namespace NamespaceTests {

using std::string;

const int MinExtentSize = 4096;

namespace MissingFieldTests {

/** A missing field is represented as null in a btree index. */
class BtreeIndexMissingField {
public:
    void run() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        BSONObj spec(BSON("key" << BSON("a" << 1)));
        ASSERT_EQUALS(jstNULL,
                      IndexLegacy::getMissingField(&txn, NULL, spec).firstElement().type());
    }
};

/** A missing field is represented as null in a 2d index. */
class TwoDIndexMissingField {
public:
    void run() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        BSONObj spec(BSON("key" << BSON("a"
                                        << "2d")));
        ASSERT_EQUALS(jstNULL,
                      IndexLegacy::getMissingField(&txn, NULL, spec).firstElement().type());
    }
};

/** A missing field is represented with the hash of null in a hashed index. */
class HashedIndexMissingField {
public:
    void run() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        BSONObj spec(BSON("key" << BSON("a"
                                        << "hashed")));
        BSONObj nullObj = BSON("a" << BSONNULL);

        // Call getKeys on the nullObj.
        BSONObjSet nullFieldKeySet;
        const CollatorInterface* collator = nullptr;
        ExpressionKeysPrivate::getHashKeys(nullObj, "a", 0, 0, false, collator, &nullFieldKeySet);
        BSONElement nullFieldFromKey = nullFieldKeySet.begin()->firstElement();

        ASSERT_EQUALS(ExpressionKeysPrivate::makeSingleHashKey(nullObj.firstElement(), 0, 0),
                      nullFieldFromKey.Long());

        BSONObj missingField = IndexLegacy::getMissingField(&txn, NULL, spec);
        ASSERT_EQUALS(NumberLong, missingField.firstElement().type());
        ASSERT_EQUALS(nullFieldFromKey, missingField.firstElement());
    }
};

/**
 * A missing field is represented with the hash of null in a hashed index.  This hash value
 * depends on the hash seed.
 */
class HashedIndexMissingFieldAlternateSeed {
public:
    void run() {
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        BSONObj spec(BSON("key" << BSON("a"
                                        << "hashed")
                                << "seed"
                                << 0x5eed));
        BSONObj nullObj = BSON("a" << BSONNULL);

        BSONObjSet nullFieldKeySet;
        const CollatorInterface* collator = nullptr;
        ExpressionKeysPrivate::getHashKeys(
            nullObj, "a", 0x5eed, 0, false, collator, &nullFieldKeySet);
        BSONElement nullFieldFromKey = nullFieldKeySet.begin()->firstElement();

        ASSERT_EQUALS(ExpressionKeysPrivate::makeSingleHashKey(nullObj.firstElement(), 0x5eed, 0),
                      nullFieldFromKey.Long());

        // Ensure that getMissingField recognizes that the seed is different (and returns
        // the right key).
        BSONObj missingField = IndexLegacy::getMissingField(&txn, NULL, spec);
        ASSERT_EQUALS(NumberLong, missingField.firstElement().type());
        ASSERT_EQUALS(nullFieldFromKey, missingField.firstElement());
    }
};

}  // namespace MissingFieldTests

namespace NamespaceDetailsTests {
#if 0   // SERVER-13640

    class Base {
        const char *ns_;
        Lock::GlobalWrite lk;
        OldClientContext _context;
    public:
        Base( const char *ns = "unittests.NamespaceDetailsTests" ) : ns_( ns ) , _context( ns ) {}
        virtual ~Base() {
            const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext(); OperationContext& txn = *txnPtr;
            if ( !nsd() )
                return;
            _context.db()->dropCollection( &txn, ns() );
        }
    protected:
        void create() {
            Lock::GlobalWrite lk;
            const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext(); OperationContext& txn = *txnPtr;
            ASSERT( userCreateNS( &txn, db(), ns(), fromjson( spec() ), false ).isOK() );
        }
        virtual string spec() const = 0;
        int nRecords() const {
            int count = 0;
            const Extent* ext;
            for ( RecordId extLoc = nsd()->firstExtent();
                    !extLoc.isNull();
                    extLoc = ext->xnext) {
                ext = extentManager()->getExtent(extLoc);
                int fileNo = ext->firstRecord.a();
                if ( fileNo == -1 )
                    continue;
                for ( int recOfs = ext->firstRecord.getOfs(); recOfs != RecordId::NullOfs;
                      recOfs = recordStore()->recordFor(RecordId(fileNo, recOfs))->nextOfs() ) {
                    ++count;
                }
            }
            ASSERT_EQUALS( count, nsd()->numRecords() );
            return count;
        }
        int nExtents() const {
            int count = 0;
            for ( RecordId extLoc = nsd()->firstExtent();
                    !extLoc.isNull();
                    extLoc = extentManager()->getExtent(extLoc)->xnext ) {
                ++count;
            }
            return count;
        }
        const char *ns() const {
            return ns_;
        }
        const NamespaceDetails *nsd() const {
            Collection* c = collection();
            if ( !c )
                return NULL;
            return c->detailsDeprecated();
        }
        const RecordStore* recordStore() const {
            Collection* c = collection();
            if ( !c )
                return NULL;
            return c->getRecordStore();
        }
        Database* db() const {
            return _context.db();
        }
        const ExtentManager* extentManager() const {
            return db()->getExtentManager();
        }
        Collection* collection() const {
            return db()->getCollection( ns() );
        }

        static BSONObj bigObj() {
            BSONObjBuilder b;
            b.appendOID("_id", 0, true);
            string as( 187, 'a' );
            b.append( "a", as );
            return b.obj();
        }

    };

    class Create : public Base {
    public:
        void run() {
            create();
            ASSERT( nsd() );
            ASSERT_EQUALS( 0, nRecords() );
            ASSERT( nsd()->firstExtent() == nsd()->capExtent() );
            RecordId initial = RecordId();
            initial.setInvalid();
            ASSERT( initial == nsd()->capFirstNewRecord() );
        }
        virtual string spec() const { return "{\"capped\":true,\"size\":512,\"$nExtents\":1}"; }
    };

    class SingleAlloc : public Base {
    public:
        void run() {
            const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext(); OperationContext& txn = *txnPtr;
            create();
            BSONObj b = bigObj();
            ASSERT( collection()->insertDocument( &txn, b, true ).isOK() );
            ASSERT_EQUALS( 1, nRecords() );
        }
        virtual string spec() const { return "{\"capped\":true,\"size\":512,\"$nExtents\":1}"; }
    };

    class Realloc : public Base {
    public:
        void run() {
            const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext(); OperationContext& txn = *txnPtr;
            create();

            const int N = 20;
            const int Q = 16; // these constants depend on the size of the bson object, the extent
                              // size allocated by the system too
            RecordId l[ N ];
            for ( int i = 0; i < N; ++i ) {
                BSONObj b = bigObj();
                StatusWith<RecordId> status =
                ASSERT( collection()->insertDocument( &txn, b, true ).isOK() );
                l[ i ] = status.getValue();
                ASSERT( !l[ i ].isNull() );
                ASSERT( nRecords() <= Q );
                //ASSERT_EQUALS( 1 + i % 2, nRecords() );
                if ( i >= 16 )
                    ASSERT( l[ i ] == l[ i - Q] );
            }
        }
        virtual string spec() const { return "{\"capped\":true,\"size\":512,\"$nExtents\":1}"; }
    };

    class TwoExtent : public Base {
    public:
        void run() {
            const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext(); OperationContext& txn = *txnPtr;
            create();
            ASSERT_EQUALS( 2, nExtents() );

            RecordId l[ 8 ];
            for ( int i = 0; i < 8; ++i ) {
                StatusWith<RecordId> status =
                ASSERT( collection()->insertDocument( &txn, bigObj(), true ).isOK() );
                l[ i ] = status.getValue();
                ASSERT( !l[ i ].isNull() );
                //ASSERT_EQUALS( i < 2 ? i + 1 : 3 + i % 2, nRecords() );
                //if ( i > 3 )
                //    ASSERT( l[ i ] == l[ i - 4 ] );
            }
            ASSERT( nRecords() == 8 );

            // Too big
            BSONObjBuilder bob;
            bob.appendOID( "_id", NULL, true );
            bob.append( "a", string( MinExtentSize + 500, 'a' ) ); // min extent size is now 4096
            BSONObj bigger = bob.done();
            ASSERT( !collection()->insertDocument( &txn, bigger, false ).isOK() );
            ASSERT_EQUALS( 0, nRecords() );
        }
    private:
        virtual string spec() const {
            return "{\"capped\":true,\"size\":512,\"$nExtents\":2}";
        }
    };


    BSONObj docForRecordSize( int size ) {
        BSONObjBuilder b;
        b.append( "_id", 5 );
        b.append( "x", string( size - Record::HeaderSize - 22, 'x' ) );
        BSONObj x = b.obj();
        ASSERT_EQUALS( Record::HeaderSize + x.objsize(), size );
        return x;
    }

    /**
     * alloc() does not quantize records in capped collections.
     * NB: this actually tests that the code in Database::createCollection doesn't set
     * PowerOf2Sizes for capped collections.
     */
    class AllocCappedNotQuantized : public Base {
    public:
        void run() {
            const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext(); OperationContext& txn = *txnPtr;
            create();
            ASSERT( nsd()->isCapped() );
            ASSERT( !nsd()->isUserFlagSet( NamespaceDetails::Flag_UsePowerOf2Sizes ) );

            StatusWith<RecordId> result =
                collection()->insertDocument( &txn, docForRecordSize( 300 ), false );
            ASSERT( result.isOK() );
            Record* record = collection()->getRecordStore()->recordFor( result.getValue() );
            // Check that no quantization is performed.
            ASSERT_EQUALS( 300, record->lengthWithHeaders() );
        }
        virtual string spec() const { return "{capped:true,size:2048}"; }
    };


    /* test  NamespaceDetails::cappedTruncateAfter(const char *ns, RecordId loc)
    */
    class TruncateCapped : public Base {
        virtual string spec() const {
            return "{\"capped\":true,\"size\":512,\"$nExtents\":2}";
        }
        void pass(int p) {
            const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext(); OperationContext& txn = *txnPtr;
            create();
            ASSERT_EQUALS( 2, nExtents() );

            BSONObj b = bigObj();

            int N = MinExtentSize / b.objsize() * nExtents() + 5;
            int T = N - 4;

            RecordId truncAt;
            //RecordId l[ 8 ];
            for ( int i = 0; i < N; ++i ) {
                BSONObj bb = bigObj();
                StatusWith<RecordId> status = collection()->insertDocument( &txn, bb, true );
                ASSERT( status.isOK() );
                RecordId a = status.getValue();
                if( T == i )
                    truncAt = a;
                ASSERT( !a.isNull() );
                /*ASSERT_EQUALS( i < 2 ? i + 1 : 3 + i % 2, nRecords() );
                if ( i > 3 )
                    ASSERT( l[ i ] == l[ i - 4 ] );*/
            }
            ASSERT( nRecords() < N );

            RecordId last, first;
            {
                unique_ptr<Runner> runner(InternalPlanner::collectionScan(&txn,
                                                                        ns(),
                                                                        collection(),
                                                                        InternalPlanner::BACKWARD));
                runner->getNext(NULL, &last);
                ASSERT( !last.isNull() );
            }
            {
                unique_ptr<Runner> runner(InternalPlanner::collectionScan(&txn,
                                                                        ns(),
                                                                        collection(),
                                                                        InternalPlanner::FORWARD));
                runner->getNext(NULL, &first);
                ASSERT( !first.isNull() );
                ASSERT( first != last ) ;
            }

            collection()->temp_cappedTruncateAfter(&txn, truncAt, false);
            ASSERT_EQUALS( collection()->numRecords() , 28u );

            {
                RecordId loc;
                unique_ptr<Runner> runner(InternalPlanner::collectionScan(&txn,
                                                                        ns(),
                                                                        collection(),
                                                                        InternalPlanner::FORWARD));
                runner->getNext(NULL, &loc);
                ASSERT( first == loc);
            }
            {
                unique_ptr<Runner> runner(InternalPlanner::collectionScan(&txn,
                                                                        ns(),
                                                                        collection(),
                                                                        InternalPlanner::BACKWARD));
                RecordId loc;
                runner->getNext(NULL, &loc);
                ASSERT( last != loc );
                ASSERT( !last.isNull() );
            }

            // Too big
            BSONObjBuilder bob;
            bob.appendOID("_id", 0, true);
            bob.append( "a", string( MinExtentSize + 300, 'a' ) );
            BSONObj bigger = bob.done();
            ASSERT( !collection()->insertDocument( &txn, bigger, true ).isOK() );
            ASSERT_EQUALS( 0, nRecords() );
        }
    public:
        void run() {
//                log() << "******** NOT RUNNING TruncateCapped test yet ************" << endl;
            pass(0);
        }
    };
#endif  // SERVER-13640
#if 0   // XXXXXX - once RecordStore is clean, we can put this back
        class Migrate : public Base {
        public:
            void run() {
                create();
                nsd()->deletedListEntry( 2 ) = nsd()->cappedListOfAllDeletedRecords().drec()->
                        nextDeleted().drec()->nextDeleted();
                nsd()->cappedListOfAllDeletedRecords().drec()->nextDeleted().drec()->
                        nextDeleted().writing() = RecordId();
                nsd()->cappedLastDelRecLastExtent().Null();
                NamespaceDetails *d = nsd();

                zero( &d->capExtent() );
                zero( &d->capFirstNewRecord() );

                // this has a side effect of called NamespaceDetails::cappedCheckMigrate
                db()->namespaceIndex().details( ns() );

                ASSERT( nsd()->firstExtent() == nsd()->capExtent() );
                ASSERT( nsd()->capExtent().getOfs() != 0 );
                ASSERT( !nsd()->capFirstNewRecord().isValid() );
                int nDeleted = 0;
                for ( RecordId i = nsd()->cappedListOfAllDeletedRecords();
                        !i.isNull(); i = i.drec()->nextDeleted(), ++nDeleted );
                ASSERT_EQUALS( 10, nDeleted );
                ASSERT( nsd()->cappedLastDelRecLastExtent().isNull() );
            }
        private:
            static void zero( RecordId *d ) {
                memset( d, 0, sizeof( RecordId ) );
            }
            virtual string spec() const {
                return "{\"capped\":true,\"size\":512,\"$nExtents\":10}";
            }
        };
#endif

// This isn't a particularly useful test, and because it doesn't clean up
// after itself, /tmp/unittest needs to be cleared after running.
//        class BigCollection : public Base {
//        public:
//            BigCollection() : Base( "NamespaceDetailsTests_BigCollection" ) {}
//            void run() {
//                create();
//                ASSERT_EQUALS( 2, nExtents() );
//            }
//        private:
//            virtual string spec() const {
//                // NOTE 256 added to size in _userCreateNS()
//                long long big = DataFile::maxSize() - DataFileHeader::HeaderSize;
//                stringstream ss;
//                ss << "{\"capped\":true,\"size\":" << big << "}";
//                return ss.str();
//            }
//        };

#if 0   // SERVER-13640
        class SwapIndexEntriesTest : public Base {
        public:
            void run() {
                create();
                NamespaceDetails *nsd = collection()->detailsWritable();

                const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext(); OperationContext& txn = *txnPtr;
                // Set 2 & 54 as multikey
                nsd->setIndexIsMultikey(&txn, 2, true);
                nsd->setIndexIsMultikey(&txn, 54, true);
                ASSERT(nsd->isMultikey(2));
                ASSERT(nsd->isMultikey(54));

                // Flip 2 & 47
                nsd->setIndexIsMultikey(&txn, 2, false);
                nsd->setIndexIsMultikey(&txn, 47, true);
                ASSERT(!nsd->isMultikey(2));
                ASSERT(nsd->isMultikey(47));

                // Reset entries that are already true
                nsd->setIndexIsMultikey(&txn, 54, true);
                nsd->setIndexIsMultikey(&txn, 47, true);
                ASSERT(nsd->isMultikey(54));
                ASSERT(nsd->isMultikey(47));

                // Two non-multi-key
                nsd->setIndexIsMultikey(&txn, 2, false);
                nsd->setIndexIsMultikey(&txn, 43, false);
                ASSERT(!nsd->isMultikey(2));
                ASSERT(nsd->isMultikey(54));
                ASSERT(nsd->isMultikey(47));
                ASSERT(!nsd->isMultikey(43));
            }
            virtual string spec() const { return "{\"capped\":true,\"size\":512,\"$nExtents\":1}"; }
        };
#endif  // SERVER-13640
}  // namespace NamespaceDetailsTests

namespace DatabaseTests {

class RollbackCreateCollection {
public:
    void run() {
        const string dbName = "rollback_create_collection";
        const string committedName = dbName + ".committed";
        const string rolledBackName = dbName + ".rolled_back";

        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;

        ScopedTransaction transaction(&txn, MODE_IX);
        Lock::DBLock lk(txn.lockState(), dbName, MODE_X);

        bool justCreated;
        Database* db = dbHolder().openDb(&txn, dbName, &justCreated);
        ASSERT(justCreated);

        Collection* committedColl;
        {
            WriteUnitOfWork wunit(&txn);
            ASSERT_FALSE(db->getCollection(committedName));
            committedColl = db->createCollection(&txn, committedName);
            ASSERT_EQUALS(db->getCollection(committedName), committedColl);
            wunit.commit();
        }

        ASSERT_EQUALS(db->getCollection(committedName), committedColl);

        {
            WriteUnitOfWork wunit(&txn);
            ASSERT_FALSE(db->getCollection(rolledBackName));
            Collection* rolledBackColl = db->createCollection(&txn, rolledBackName);
            ASSERT_EQUALS(db->getCollection(rolledBackName), rolledBackColl);
            // not committing so creation should be rolled back
        }

        // The rolledBackCollection creation should have been rolled back
        ASSERT_FALSE(db->getCollection(rolledBackName));

        // The committedCollection should not have been affected by the rollback. Holders
        // of the original Collection pointer should still be valid.
        ASSERT_EQUALS(db->getCollection(committedName), committedColl);
    }
};

class RollbackDropCollection {
public:
    void run() {
        const string dbName = "rollback_drop_collection";
        const string droppedName = dbName + ".dropped";
        const string rolledBackName = dbName + ".rolled_back";

        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;

        ScopedTransaction transaction(&txn, MODE_IX);
        Lock::DBLock lk(txn.lockState(), dbName, MODE_X);

        bool justCreated;
        Database* db = dbHolder().openDb(&txn, dbName, &justCreated);
        ASSERT(justCreated);

        {
            WriteUnitOfWork wunit(&txn);
            ASSERT_FALSE(db->getCollection(droppedName));
            Collection* droppedColl;
            droppedColl = db->createCollection(&txn, droppedName);
            ASSERT_EQUALS(db->getCollection(droppedName), droppedColl);
            db->dropCollection(&txn, droppedName);
            wunit.commit();
        }

        //  Should have been really dropped
        ASSERT_FALSE(db->getCollection(droppedName));

        {
            WriteUnitOfWork wunit(&txn);
            ASSERT_FALSE(db->getCollection(rolledBackName));
            Collection* rolledBackColl = db->createCollection(&txn, rolledBackName);
            wunit.commit();
            ASSERT_EQUALS(db->getCollection(rolledBackName), rolledBackColl);
            db->dropCollection(&txn, rolledBackName);
            // not committing so dropping should be rolled back
        }

        // The rolledBackCollection dropping should have been rolled back.
        // Original Collection pointers are no longer valid.
        ASSERT(db->getCollection(rolledBackName));

        // The droppedCollection should not have been restored by the rollback.
        ASSERT_FALSE(db->getCollection(droppedName));
    }
};
}  // namespace DatabaseTests

class All : public Suite {
public:
    All() : Suite("namespace") {}

    void setupTests() {
        add<MissingFieldTests::BtreeIndexMissingField>();
        add<MissingFieldTests::TwoDIndexMissingField>();
        add<MissingFieldTests::HashedIndexMissingField>();
        add<MissingFieldTests::HashedIndexMissingFieldAlternateSeed>();

// add< NamespaceDetailsTests::Create >();
// add< NamespaceDetailsTests::SingleAlloc >();
// add< NamespaceDetailsTests::Realloc >();
// add< NamespaceDetailsTests::AllocCappedNotQuantized >();
// add< NamespaceDetailsTests::TwoExtent >();
// add< NamespaceDetailsTests::TruncateCapped >();
// add< NamespaceDetailsTests::Migrate >();
// add< NamespaceDetailsTests::SwapIndexEntriesTest >();
//            add< NamespaceDetailsTests::BigCollection >();

#if 0
            // until ROLLBACK_ENABLED
            add< DatabaseTests::RollbackCreateCollection >();
            add< DatabaseTests::RollbackDropCollection >();
#endif
    }
};

SuiteInstance<All> myall;

}  // namespace NamespaceTests
