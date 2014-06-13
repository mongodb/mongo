// heap1_test.cpp

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


#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/heap1/heap1_database_catalog_entry.h"
#include "mongo/db/storage/heap1/heap1_recovery_unit.h"
#include "mongo/db/storage/record.h"
#include "mongo/db/structure/record_store.h"

#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

    class MyOperationContext : public OperationContext {
    public:
        MyOperationContext() {
            _recoveryUnit.reset( new Heap1RecoveryUnit() );
        }

        virtual ~MyOperationContext() { }

        CurOp* getCurOp() const {
            invariant(false);
            return NULL;
        }

        virtual RecoveryUnit* recoveryUnit() const {
            return _recoveryUnit.get();
        }

        virtual LockState* lockState() const {
            // TODO: Eventually, this should return an actual LockState object. For now,
            //       LockState depends on the whole world and is not necessary for testing.
            return NULL;
        }

        virtual ProgressMeter* setMessage( const char * msg,
                                           const std::string &name,
                                           unsigned long long progressMeterTotal,
                                           int secondsBetween ) {
            invariant(false);
            return NULL;
        }

        virtual void checkForInterrupt( bool heedMutex ) const { }

        virtual Status checkForInterruptNoAssert() const {
            return Status::OK();
        }

        virtual bool isPrimaryFor( const StringData& ns ) {
            return true;
        }

        virtual const char* getNS() const {
            return NULL;
        };

    private:
        boost::scoped_ptr<Heap1RecoveryUnit> _recoveryUnit;

    };

    TEST(Heap1Simple, CreateDestroy) {
        Heap1DatabaseCatalogEntry db( "foo" );
    }

    TEST(Heap1Simple, ListCollections) {
        Heap1DatabaseCatalogEntry db( "foo" );

        std::list<std::string> collections;
        db.getCollectionNamespaces( &collections );
        ASSERT_EQUALS( 0U, collections.size() );

        {
            MyOperationContext op;
            db.createCollection( &op, "foo.bar", CollectionOptions(), true );
        }

        db.getCollectionNamespaces( &collections );
        ASSERT_EQUALS( 1U, collections.size() );
        ASSERT_EQUALS( "foo.bar", collections.front() );
    }

    TEST(Heap1Simple, RecordStore1) {
        Heap1DatabaseCatalogEntry db( "foo" );

        {
            MyOperationContext op;
            db.createCollection( &op, "foo.bar", CollectionOptions(), true );
        }

        {
            MyOperationContext op;
            RecordStore* rs = db.getRecordStore( &op, "foo.bar" );
            StatusWith<DiskLoc> loc = rs->insertRecord( &op, "abc", 4, -1 );
            ASSERT_OK( loc.getStatus() );
            ASSERT_EQUALS( 1, rs->numRecords() );
            ASSERT_EQUALS( std::string( "abc" ), rs->recordFor( loc.getValue() )->data() );
        }

    }

    TEST(Heap1Simple, Drop1) {
        Heap1DatabaseCatalogEntry db( "foo" );

        std::list<std::string> collections;
        db.getCollectionNamespaces( &collections );
        ASSERT_EQUALS( 0U, collections.size() );

        {
            MyOperationContext op;
            Status status = db.createCollection( &op, "foo.bar", CollectionOptions(), true );
            ASSERT_OK( status );
        }

        db.getCollectionNamespaces( &collections );
        ASSERT_EQUALS( 1U, collections.size() );
        ASSERT_EQUALS( "foo.bar", collections.front() );

        {
            MyOperationContext op;
            Status status = db.dropCollection( &op, "foo.bar" );
            ASSERT_OK( status );
        }

        collections.clear();
        db.getCollectionNamespaces( &collections );
        ASSERT_EQUALS( 0U, collections.size() );

    }

}
