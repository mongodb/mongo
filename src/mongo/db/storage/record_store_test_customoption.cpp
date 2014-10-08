// record_store_test_customoption.cpp

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

#include "mongo/db/storage/record_store_test_harness.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    // Verify that calling setCustomOption() with { "usePowerOf2Sizes": true }
    // returns an OK status.
    TEST( RecordStoreTestHarness, SetPowerOf2SizesOptionTrue ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                BSONObjBuilder info;
                BSONObj option = BSON( "usePowerOf2Sizes" << true );
                ASSERT_OK( rs->setCustomOption( opCtx.get(), option.firstElement(), &info ) );
            }
        }
    }

    // Verify that calling setCustomOption() with { "usePowerOf2Sizes": false }
    // returns an OK status.
    TEST( RecordStoreTestHarness, SetPowerOf2SizesOptionFalse ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                BSONObjBuilder info;
                BSONObj option = BSON( "usePowerOf2Sizes" << false );
                ASSERT_OK( rs->setCustomOption( opCtx.get(), option.firstElement(), &info ) );
            }
        }
    }

    // Verify that calling setCustomOption() with a nonexistent option
    // returns an ErrorCodes::InvalidOptions status.
    TEST( RecordStoreTestHarness, SetNonExistentOption ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<RecordStore> rs( harnessHelper->newNonCappedRecordStore() );

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                BSONObjBuilder info;
                BSONObj option = BSON( "aabacadbbcbdccdd" << false );
                ASSERT_EQUALS( ErrorCodes::InvalidOptions,
                               rs->setCustomOption( opCtx.get(), option.firstElement(), &info ) );
            }
        }
    }

} // namespace mongo
