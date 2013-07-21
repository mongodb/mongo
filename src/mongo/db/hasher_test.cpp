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
 */

/** Unit tests for BSONElementHasher. */

/** These tests form test vectors for hashed indexes. */
/** All of the values in these tests ve been determined experimentally. */

#include "mongo/db/hasher.h"
#include "mongo/db/jsobj.h"
#include "mongo/bson/bsontypes.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

    // Helper method.
    long long hashIt( const BSONObj& o ) {
        return BSONElementHasher::hash64( o.firstElement(), 0 );
    }


    TEST( BSONElementHasher, HashIntOrLongOrDouble ) {
        BSONObj o = BSON( "check" << 42 );
        ASSERT_EQUALS( hashIt( o ), -944302157085130861LL );
        o = BSON( "check" << 42.123 );
        ASSERT_EQUALS( hashIt( o ), -944302157085130861LL );
        o = BSON( "check" << (long long) 42 );
        ASSERT_EQUALS( hashIt( o ), -944302157085130861LL );

        o = BSON( "check" << 0 );
        ASSERT_EQUALS( hashIt( o ), 4854801880128277513LL );
        o = BSON( "check" << 0.456 );
        ASSERT_EQUALS( hashIt( o ), 4854801880128277513LL );
        o = BSON( "check" << (long long) 0 );
        ASSERT_EQUALS( hashIt( o ), 4854801880128277513LL );
        // NAN is treated as zero.
        o = BSON( "check" << std::numeric_limits<double>::signaling_NaN() );
        ASSERT_EQUALS( hashIt( o ), 4854801880128277513LL );
        o = BSON( "check" << std::numeric_limits<double>::quiet_NaN() );
        ASSERT_EQUALS( hashIt( o ), 4854801880128277513LL );

        o = BSON( "check" << 1 );
        ASSERT_EQUALS( hashIt( o ), 5902408780260971510LL );
        o = BSON( "check" << 1.987 );
        ASSERT_EQUALS( hashIt( o ), 5902408780260971510LL );
        o = BSON( "check" << (long long) 1 );
        ASSERT_EQUALS( hashIt( o ), 5902408780260971510LL );

        o = BSON( "check" << -1 );
        ASSERT_EQUALS( hashIt( o ), 1140205862565771219LL );
        o = BSON( "check" << -1.789 );
        ASSERT_EQUALS( hashIt( o ), 1140205862565771219LL );
        o = BSON( "check" << (long long) -1 );
        ASSERT_EQUALS( hashIt( o ), 1140205862565771219LL );

        o = BSON( "check" << std::numeric_limits<int>::min() );
        ASSERT_EQUALS( hashIt( o ), 6165898260261354870LL );
        o = BSON( "check" << (double) std::numeric_limits<int>::min() );
        ASSERT_EQUALS( hashIt( o ), 6165898260261354870LL );
        o = BSON( "check" << (long long) std::numeric_limits<int>::min() );
        ASSERT_EQUALS( hashIt( o ), 6165898260261354870LL );

        o = BSON( "check" << std::numeric_limits<int>::max() );
        ASSERT_EQUALS( hashIt( o ), 1143184177162245883LL );
        o = BSON( "check" << (double) std::numeric_limits<int>::max() );
        ASSERT_EQUALS( hashIt( o ), 1143184177162245883LL );
        o = BSON( "check" << (long long) std::numeric_limits<int>::max() );
        ASSERT_EQUALS( hashIt( o ), 1143184177162245883LL );

        // Large/small double values.
        ASSERT( std::numeric_limits<long long>::max() < std::numeric_limits<double>::max() );
        o = BSON( "check" << std::numeric_limits<double>::max() );
        ASSERT_EQUALS( hashIt( o ), 921523596458303250LL );
        o = BSON( "check" << std::numeric_limits<long long>::max() ); // 9223372036854775807
        ASSERT_EQUALS( hashIt( o ), 921523596458303250LL );

        // Have to create our own small double.
        // std::numeric_limits<double>::lowest() - Not available until C++11
        // std::numeric_limits<double>::min() - Closest positive value to zero, not most negative. 
        double smallDouble = - std::numeric_limits<double>::max();
        ASSERT( smallDouble < static_cast<double>( std::numeric_limits<long long>::min() ) ); 
        o = BSON( "check" << smallDouble );
        ASSERT_EQUALS( hashIt( o ), 4532067210535695462LL );
        o = BSON( "check" << std::numeric_limits<long long>::min() ); // -9223372036854775808
        ASSERT_EQUALS( hashIt( o ), 4532067210535695462LL );
    }

    TEST( BSONElementHasher, HashMinKey ) {
        BSONObj o = BSON( "check" << MINKEY );
        ASSERT_EQUALS( hashIt( o ), 7961148599568647290LL );
    }

    TEST( BSONElementHasher, HashMaxKey ) {
        BSONObj o = BSON( "check" << MAXKEY );
        ASSERT_EQUALS( hashIt( o ), 5504842513779440750LL );
    }

    TEST( BSONElementHasher, HashUndefined ) {
        BSONObj o = BSON( "check" << BSONUndefined );
        ASSERT_EQUALS( hashIt( o ), 40158834000849533LL );
    }

    TEST( BSONElementHasher, HashString ) {
        BSONObj o = BSON( "check" << "abc" );
        ASSERT_EQUALS( hashIt( o ), 8478485326885698097LL );
        o = BSON( "check" << BSONSymbol( "abc" ) );
        ASSERT_EQUALS( hashIt( o ), 8478485326885698097LL );

        o = BSON( "check" << "" );
        ASSERT_EQUALS( hashIt( o ), 2049396243249673340LL );
        o = BSON( "check" << BSONSymbol( "" ) );
        ASSERT_EQUALS( hashIt( o ), 2049396243249673340LL );
    }

    TEST( BSONElementHasher, HashObject ) {
        BSONObj o = BSON( "check" << BSON( "a" << "abc" << "b" << 123LL ) );
        ASSERT_EQUALS( hashIt( o ), 4771603801758380216LL );

        o = BSON( "check" << BSONObj() );
        ASSERT_EQUALS( hashIt( o ), 7980500913326740417LL );
    }

    TEST( BSONElementHasher, HashArray ) {
        BSONObj o = BSON( "check" << BSON_ARRAY( "bar" << "baz" << "qux" ) );
        ASSERT_EQUALS( hashIt( o ), -2938911267422831539LL );

        o = BSON( "check" << BSONArray() );
        ASSERT_EQUALS( hashIt( o ), 8849948234993459283LL );
    }

    TEST( BSONElementHasher, HashBinary ) {
        uint8_t bytes[] = { 0, 1, 2, 3, 4, 6 };
        BSONObj o = BSON( "check" << BSONBinData( bytes, 6, BinDataGeneral ) );
        ASSERT_EQUALS( hashIt( o ), 7252465090394235301LL );

        o = BSON( "check" << BSONBinData( bytes, 6, bdtCustom ) );
        ASSERT_EQUALS( hashIt( o ), 5736670452907618262LL );

        uint8_t uuidBytes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
        o = BSON( "check" << BSONBinData( uuidBytes, 16, newUUID ) );
        ASSERT_EQUALS( hashIt( o ), 6084661258071355978LL );
    }

    TEST( BSONElementHasher, HashObjectId ) {
        BSONObj o = BSON( "check" << OID( "010203040506070809101112" ) );
        ASSERT_EQUALS( hashIt( o ), -5588663249627035708LL );

        o = BSON( "check" << OID( "000000000000000000000000" ) );
        ASSERT_EQUALS( hashIt( o ), -4293118519463489418LL );
    }

    TEST( BSONElementHasher, HashBoolean ) {
        BSONObj o = BSON( "check" << true );
        ASSERT_EQUALS( hashIt( o ), 6405873908747105701LL );

        o = BSON( "check" << false );
        ASSERT_EQUALS( hashIt( o ), 6289544573401934092LL );
    }

    TEST( BSONElementHasher, HashTimeStamp ) {
        BSONObjBuilder builder1;
        BSONObjBuilder builder2;

        BSONObj o = BSON( "check" << Date_t( 0x5566778811223344LL ) );
        ASSERT_EQUALS( hashIt( o ), 4476222765095560467LL );
        o = builder1.appendTimestamp( "check", 0x55667788LL * 1000LL, 0x11223344LL ).done();
        ASSERT_EQUALS( hashIt( o ), 4476222765095560467LL );

        o = BSON( "check" << Date_t( 0 ) );
        ASSERT_EQUALS( hashIt( o ), -1178696894582842035LL );
        o = builder2.appendTimestamp( "check", 0 ).done();
        ASSERT_EQUALS( hashIt( o ), -1178696894582842035LL );
    }

    TEST( BSONElementHasher, HashRegEx ) {
        BSONObj o = BSON( "check" << BSONRegEx( "mongodb" ) );
        ASSERT_EQUALS( hashIt( o ), -7275792090268217043LL );

        o = BSON( "check" << BSONRegEx( ".*", "i" ) );
        ASSERT_EQUALS( hashIt( o ), 7095855029187981886LL );
    }

    TEST( BSONElementHasher, HashDBRef ) {
        BSONObj o = BSON( "check" << BSONDBRef( "c", OID( "010203040506070809101112" ) ) );
        ASSERT_EQUALS( hashIt( o ), 940175826736461384LL );

        o = BSON( "check" << BSONDBRef( "db.c", OID( "010203040506070809101112" ) ) );
        ASSERT_EQUALS( hashIt( o ), 2426768198104018194LL );
    }

    TEST( BSONElementHasher, HashCode ) {
        BSONObj o = BSON( "check" << BSONCode( "func f() { return 1; }" ) );
        ASSERT_EQUALS( hashIt( o ), 6861638109178014270LL );
    }

    TEST( BSONElementHasher, HashCodeWScope ) {
        BSONObj o = BSON( "check" << BSONCodeWScope( "func f() { return 1; }", BSON( "c" << true ) ) );
        ASSERT_EQUALS( hashIt( o ), 501342939894575968LL );
    }

} // namespace
} // namespace mongo
