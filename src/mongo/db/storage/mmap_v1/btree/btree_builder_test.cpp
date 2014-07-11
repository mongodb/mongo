// btree_builder_test.cpp : Btree builder unit test

/**
 *    Copyright (C) 2014 MongoDB
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

// This file contains simple tests to check the Btree builder logic,
// including handling of interruptions.

#include "mongo/db/instance.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/mmap_v1/btree/btree_test_help.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    class MockOperationContextKillable : public OperationContextNoop {
    public:
        MockOperationContextKillable()
            : _killPending(false) {
        }

        virtual void checkForInterrupt(bool heedMutex = true) const {
            if (_killPending) {
                throw UserException(ErrorCodes::Interrupted, "interrupted");
            }
        }

        virtual void kill() {
           _killPending = true;
        }

    private:
       bool _killPending;
    };

    /**
     * Builder::commit() is interrupted if there is a request to kill the current operation.
     */
    template<class OnDiskFormat>
    class InterruptCommit {
    public:
        typedef typename BtreeLogic<OnDiskFormat>::Builder Builder;

        InterruptCommit( bool mayInterrupt ) :
            _mayInterrupt( mayInterrupt ),
            _helper(BSON( "a" << 1 )) {
        }

        void run() {
            // Create a btree builder.
            MockOperationContextKillable txn;
            Builder* builder = _helper.btree.newBuilder(&txn, false);

            // Add some keys to the builder, in order.  We need enough keys to build an internal
            // node in order to check for an interrupt.
            int32_t nKeys = 1000;
            for( int32_t i = 0; i < nKeys; ++i ) {
                BSONObj key = BSON( "a" << i );
                builder->addKey( key, /* dummy location */ DiskLoc() );
            }

            // The root of the index has not yet been set.
            ASSERT( _helper.headManager.getHead().isNull() );
            // Register a request to kill the current operation.
            txn.kill();
            if ( _mayInterrupt ) {
                // Call commit on the builder, which will be aborted due to the kill request.
                ASSERT_THROWS( builder->commit( _mayInterrupt ), UserException );
                // The root of the index is not set because commit() did not complete.
                ASSERT( _helper.headManager.getHead().isNull() );
            }
            else {
                // Call commit on the builder, which will not be aborted because mayInterrupt is
                // false.
                builder->commit( _mayInterrupt );
                // The root of the index is set because commit() completed.
                ASSERT( !_helper.headManager.getHead().isNull() );
            }
        }

    private:
        bool _mayInterrupt;
        BtreeLogicTestHelper<OnDiskFormat> _helper;
    };


    //
    // TEST SUITE DEFINITION
    //

    template<class OnDiskFormat>
    class BtreeBuilderTestSuite : public unittest::Suite {
    public:
        BtreeBuilderTestSuite(const std::string& name) : Suite(name) {

        }

        void setupTests() {

            add< InterruptCommit<OnDiskFormat> >( false );
            add< InterruptCommit<OnDiskFormat> >( true );
        }
    };

    // Test suite for both V0 and V1
    static BtreeBuilderTestSuite<BtreeLayoutV0> SUITE_V0("BtreeBuilderTests V0");
    static BtreeBuilderTestSuite<BtreeLayoutV1> SUITE_V1("BtreeBuilderTests V1");
} // namespace mongo
