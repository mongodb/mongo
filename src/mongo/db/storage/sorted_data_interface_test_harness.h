// sorted_data_interface_test_harness.h

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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/operation_context_noop.h"

namespace mongo {

    const BSONObj key0 = BSON( "" << 0 );
    const BSONObj key1 = BSON( "" << 1 );
    const BSONObj key2 = BSON( "" << 2 );
    const BSONObj key3 = BSON( "" << 3 );
    const BSONObj key4 = BSON( "" << 4 );
    const BSONObj key5 = BSON( "" << 5 );
    const BSONObj key6 = BSON( "" << 6 );

    const BSONObj compoundKey1a = BSON( "" << 1 << "" << "a" );
    const BSONObj compoundKey1b = BSON( "" << 1 << "" << "b" );
    const BSONObj compoundKey1c = BSON( "" << 1 << "" << "c" );
    const BSONObj compoundKey1d = BSON( "" << 1 << "" << "d" );
    const BSONObj compoundKey2a = BSON( "" << 2 << "" << "a" );
    const BSONObj compoundKey2b = BSON( "" << 2 << "" << "b" );
    const BSONObj compoundKey2c = BSON( "" << 2 << "" << "c" );
    const BSONObj compoundKey3a = BSON( "" << 3 << "" << "a" );
    const BSONObj compoundKey3b = BSON( "" << 3 << "" << "b" );
    const BSONObj compoundKey3c = BSON( "" << 3 << "" << "c" );

    const DiskLoc loc1( 10, 42 );
    const DiskLoc loc2( 10, 44 );
    const DiskLoc loc3( 10, 46 );
    const DiskLoc loc4( 10, 48 );
    const DiskLoc loc5( 10, 50 );
    const DiskLoc loc6( 10, 52 );
    const DiskLoc loc7( 10, 54 );
    const DiskLoc loc8( 10, 56 );

    class RecoveryUnit;
    class SortedDataInterface;

    class HarnessHelper {
    public:
        HarnessHelper(){}
        virtual ~HarnessHelper(){}

        virtual SortedDataInterface* newSortedDataInterface() = 0;
        virtual RecoveryUnit* newRecoveryUnit() = 0;

        virtual OperationContext* newOperationContext() {
            return new OperationContextNoop( newRecoveryUnit() );
        }
    };

    HarnessHelper* newHarnessHelper();
}
