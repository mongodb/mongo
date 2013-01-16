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

/** Unit tests for DiskLoc. */

#include "mongo/db/diskloc.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

    TEST( DiskLoc, HashEqual ) {
        DiskLoc locA( 1, 2 );
        DiskLoc locB;
        locB.set( 1, 2 );
        ASSERT_EQUALS( locA, locB );
        DiskLoc::Hasher hasher;
        ASSERT_EQUALS( hasher( locA ), hasher( locB ) );
    }

    TEST( DiskLoc, HashNotEqual ) {
        DiskLoc original( 1, 2 );
        DiskLoc diffFile( 10, 2 );
        DiskLoc diffOfs( 1, 20 );
        DiskLoc diffBoth( 10, 20 );
        ASSERT_NOT_EQUALS( original, diffFile );
        ASSERT_NOT_EQUALS( original, diffOfs );
        ASSERT_NOT_EQUALS( original, diffBoth );
        
        // Unequal DiskLocs need not produce unequal hashes.  But unequal hashes are likely, and
        // assumed here for sanity checking of the custom hash implementation.
        DiskLoc::Hasher hasher;
        ASSERT_NOT_EQUALS( hasher( original ), hasher( diffFile ) );
        ASSERT_NOT_EQUALS( hasher( original ), hasher( diffOfs ) );
        ASSERT_NOT_EQUALS( hasher( original ), hasher( diffBoth ) );
    }
    
} // namespace
} // namespace mongo
