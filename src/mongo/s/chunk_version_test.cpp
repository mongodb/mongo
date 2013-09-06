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

#include "mongo/pch.h"

#include "mongo/db/jsobj.h"
#include "mongo/s/chunk_version.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

    /**
     * Tests parsing of BSON for versions.  In version 2.2, this parsing is meant to be very
     * flexible so different formats can be tried and enforced later.
     *
     * Formats are:
     *
     * A) { vFieldName : <TSTYPE>, [ vFieldNameEpoch : <OID> ], ... }
     * B) { fieldName : [ <TSTYPE>, <OID> ], ... }
     *
     * vFieldName is a specifyable name - usually "version" (default) or "lastmod".  <TSTYPE> is a
     * type convertible to Timestamp, ideally Timestamp but also numeric.
     * <OID> is a value of type OID.
     *
     */

    TEST(Compatibility, LegacyFormatA) {
        BSONObjBuilder versionObjB;
        versionObjB.appendTimestamp( "testVersion",
                                     ChunkVersion( 1, 1, OID() ).toLong() );
        versionObjB.append( "testVersionEpoch", OID::gen() );
        BSONObj versionObj = versionObjB.obj();

        ChunkVersion parsed =
            ChunkVersion::fromBSON( versionObj[ "testVersion" ] );

        ASSERT( ChunkVersion::canParseBSON( versionObj[ "testVersion" ] ) );
        ASSERT( parsed.majorVersion() == 1 );
        ASSERT( parsed.minorVersion() == 1 );
        ASSERT( ! parsed.epoch().isSet() );

        parsed = ChunkVersion::fromBSON( versionObj, "testVersion" );

        ASSERT( ChunkVersion::canParseBSON( versionObj, "testVersion" ) );
        ASSERT( parsed.majorVersion() == 1 );
        ASSERT( parsed.minorVersion() == 1 );
        ASSERT( parsed.epoch().isSet() );
    }

    TEST(Compatibility, SubArrayFormatB) {
        BSONObjBuilder tsObjB;
        tsObjB.appendTimestamp( "ts", ChunkVersion( 1, 1, OID() ).toLong() );
        BSONObj tsObj = tsObjB.obj();

        BSONObjBuilder versionObjB;
        BSONArrayBuilder subArrB( versionObjB.subarrayStart( "testVersion" ) );
        // Append this weird way so we're sure we get a timestamp type
        subArrB.append( tsObj.firstElement() );
        subArrB.append( OID::gen() );
        subArrB.done();
        BSONObj versionObj = versionObjB.obj();

        ChunkVersion parsed =
            ChunkVersion::fromBSON( versionObj[ "testVersion" ] );

        ASSERT( ChunkVersion::canParseBSON( versionObj[ "testVersion" ] ) );
        ASSERT( ChunkVersion::canParseBSON( BSONArray( versionObj[ "testVersion" ].Obj() ) ) );
        ASSERT( parsed.majorVersion() == 1 );
        ASSERT( parsed.minorVersion() == 1 );
        ASSERT( parsed.epoch().isSet() );
    }

} // unnamed namespace
} // namespace mongo
