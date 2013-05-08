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

#include "mongo/db/dbhelpers.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    /**
     * Unit tests related to DBHelpers
     */

    static const char * const ns = "unittests.removetests";
    static DBDirectClient client;

    // TODO: Normalize with test framework
    /** Simple test for Helpers::RemoveRange. */
    class RemoveRange {
    public:
        RemoveRange() :
                _min( 4 ), _max( 8 )
        {
        }
        void run() {
            for ( int i = 0; i < 10; ++i ) {
                client.insert( ns, BSON( "_id" << i ) );
            }

            {
                // Remove _id range [_min, _max).
                Lock::DBWrite lk( ns );
                Client::Context ctx( ns );
                KeyRange range( ns,
                                BSON( "_id" << _min ),
                                BSON( "_id" << _max ),
                                BSON( "_id" << 1 ) );
                Helpers::removeRange( range );
            }

            // Check that the expected documents remain.
            ASSERT_EQUALS( expected(), docs() );
        }
    private:
        BSONArray expected() const {
            BSONArrayBuilder bab;
            for ( int i = 0; i < _min; ++i ) {
                bab << BSON( "_id" << i );
            }
            for ( int i = _max; i < 10; ++i ) {
                bab << BSON( "_id" << i );
            }
            return bab.arr();
        }
        BSONArray docs() const {
            auto_ptr<DBClientCursor> cursor = client.query( ns,
                                                            Query().hint( BSON( "_id" << 1 ) ) );
            BSONArrayBuilder bab;
            while ( cursor->more() ) {
                bab << cursor->next();
            }
            return bab.arr();
        }
        int _min;
        int _max;
    };

    class All: public Suite {
    public:
        All() :
                Suite( "remove" )
        {
        }
        void setupTests() {
            add<RemoveRange>();
        }
    } myall;

    //
    // Tests getting disk locs for an index range
    //

    TEST(DBHelperTests, FindDiskLocs) {

        DBDirectClient client;
        // Some unique tag we can use to make sure we're pulling back the right data
        OID tag = OID::gen();
        client.remove( ns, BSONObj() );

        int numDocsInserted = 10;
        for ( int i = 0; i < numDocsInserted; ++i ) {
            client.insert( ns, BSON( "_id" << i << "tag" << tag ) );
        }

        long long maxSizeBytes = 1024 * 1024 * 1024;

        set<DiskLoc> locs;
        long long numDocsFound;
        long long estSizeBytes;
        {
            // search _id range (0, 10)
            Lock::DBRead lk( ns );
            Client::Context ctx( ns );
            KeyRange range( ns,
                            BSON( "_id" << 0 ),
                            BSON( "_id" << numDocsInserted ),
                            BSON( "_id" << 1 ) );

            Status result = Helpers::getLocsInRange( range,
                                                     maxSizeBytes,
                                                     &locs,
                                                     &numDocsFound,
                                                     &estSizeBytes );

            ASSERT_EQUALS( result, Status::OK() );
            ASSERT_EQUALS( numDocsFound, numDocsInserted );
            ASSERT_NOT_EQUALS( estSizeBytes, 0 );
            ASSERT_LESS_THAN( estSizeBytes, maxSizeBytes );

            // Make sure all the disklocs actually correspond to the right info
            for ( set<DiskLoc>::iterator it = locs.begin(); it != locs.end(); ++it ) {
                ASSERT_EQUALS( it->obj()["tag"].OID(), tag );
            }
        }
    }

    //
    // Tests index not found error getting disk locs
    //

    TEST(DBHelperTests, FindDiskLocsNoIndex) {

        DBDirectClient client;
        client.remove( ns, BSONObj() );
        client.insert( ns, BSON( "_id" << OID::gen() ) );

        long long maxSizeBytes = 1024 * 1024 * 1024;

        set<DiskLoc> locs;
        long long numDocsFound;
        long long estSizeBytes;
        {
            Lock::DBRead lk( ns );
            Client::Context ctx( ns );

            // search invalid index range
            KeyRange range( ns,
                            BSON( "badIndex" << 0 ),
                            BSON( "badIndex" << 10 ),
                            BSON( "badIndex" << 1 ) );

            Status result = Helpers::getLocsInRange( range,
                                                     maxSizeBytes,
                                                     &locs,
                                                     &numDocsFound,
                                                     &estSizeBytes );

            // Make sure we get the right error code
            ASSERT_EQUALS( result.code(), ErrorCodes::IndexNotFound );
            ASSERT_EQUALS( static_cast<long long>( locs.size() ), 0 );
            ASSERT_EQUALS( numDocsFound, 0 );
            ASSERT_EQUALS( estSizeBytes, 0 );
        }
    }

    //
    // Tests chunk too big error getting disk locs
    //

    TEST(DBHelperTests, FindDiskLocsTooBig) {

        DBDirectClient client;
        client.remove( ns, BSONObj() );

        int numDocsInserted = 10;
        for ( int i = 0; i < numDocsInserted; ++i ) {
            client.insert( ns, BSON( "_id" << i ) );
        }

        // Very small max size
        long long maxSizeBytes = 10;

        set<DiskLoc> locs;
        long long numDocsFound;
        long long estSizeBytes;
        {
            Lock::DBRead lk( ns );
            Client::Context ctx( ns );
            KeyRange range( ns,
                            BSON( "_id" << 0 ),
                            BSON( "_id" << numDocsInserted ),
                            BSON( "_id" << 1 ) );

            Status result = Helpers::getLocsInRange( range,
                                                     maxSizeBytes,
                                                     &locs,
                                                     &numDocsFound,
                                                     &estSizeBytes );

            // Make sure we get the right error code and our count and size estimates are valid
            ASSERT_EQUALS( result.code(), ErrorCodes::InvalidLength );
            ASSERT_EQUALS( numDocsFound, numDocsInserted );
            ASSERT_GREATER_THAN( estSizeBytes, maxSizeBytes );
        }
    }

} // namespace RemoveTests
