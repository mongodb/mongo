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

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    /**
     * Unit tests related to DBHelpers
     */

    static const char * const ns = "unittests.removetests";

    // TODO: Normalize with test framework
    /** Simple test for Helpers::RemoveRange. */
    class RemoveRange {
    public:
        RemoveRange() :
                _min( 4 ), _max( 8 )
        {
        }

        void run() {
            OperationContextImpl txn;
            DBDirectClient client(&txn);

            for ( int i = 0; i < 10; ++i ) {
                client.insert( ns, BSON( "_id" << i ) );
            }

            {
                // Remove _id range [_min, _max).
                Lock::DBWrite lk(txn.lockState(), ns);
                WriteUnitOfWork wunit(&txn);
                Client::Context ctx(&txn,  ns );

                KeyRange range( ns,
                                BSON( "_id" << _min ),
                                BSON( "_id" << _max ),
                                BSON( "_id" << 1 ) );
                mongo::WriteConcernOptions dummyWriteConcern;
                Helpers::removeRange(&txn, range, false, dummyWriteConcern);
                wunit.commit();
            }

            // Check that the expected documents remain.
            ASSERT_EQUALS( expected(), docs(&txn) );
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

        BSONArray docs(OperationContext* txn) const {
            DBDirectClient client(txn);
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
        OperationContextImpl txn;
        DBDirectClient client(&txn);

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
            Lock::DBRead lk(txn.lockState(), ns);

            KeyRange range( ns,
                            BSON( "_id" << 0 ),
                            BSON( "_id" << numDocsInserted ),
                            BSON( "_id" << 1 ) );

            Status result = Helpers::getLocsInRange( &txn,
                                                     range,
                                                     maxSizeBytes,
                                                     &locs,
                                                     &numDocsFound,
                                                     &estSizeBytes );

            ASSERT_EQUALS( result, Status::OK() );
            ASSERT_EQUALS( numDocsFound, numDocsInserted );
            ASSERT_NOT_EQUALS( estSizeBytes, 0 );
            ASSERT_LESS_THAN( estSizeBytes, maxSizeBytes );

            Database* db = dbHolder().get( &txn, nsToDatabase(range.ns) );
            const Collection* collection = db->getCollection(&txn, ns);

            // Make sure all the disklocs actually correspond to the right info
            for ( set<DiskLoc>::const_iterator it = locs.begin(); it != locs.end(); ++it ) {
                const BSONObj obj = collection->docFor(&txn, *it);
                ASSERT_EQUALS(obj["tag"].OID(), tag);
            }
        }
    }

    //
    // Tests index not found error getting disk locs
    //

    TEST(DBHelperTests, FindDiskLocsNoIndex) {
        OperationContextImpl txn;
        DBDirectClient client(&txn);

        client.remove( ns, BSONObj() );
        client.insert( ns, BSON( "_id" << OID::gen() ) );

        long long maxSizeBytes = 1024 * 1024 * 1024;

        set<DiskLoc> locs;
        long long numDocsFound;
        long long estSizeBytes;
        {
            Lock::DBRead lk(txn.lockState(), ns);

            // search invalid index range
            KeyRange range( ns,
                            BSON( "badIndex" << 0 ),
                            BSON( "badIndex" << 10 ),
                            BSON( "badIndex" << 1 ) );

            Status result = Helpers::getLocsInRange( &txn,
                                                     range,
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
        OperationContextImpl txn;
        DBDirectClient client(&txn);

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
            Lock::DBRead lk(txn.lockState(), ns);

            KeyRange range( ns,
                            BSON( "_id" << 0 ),
                            BSON( "_id" << numDocsInserted ),
                            BSON( "_id" << 1 ) );

            Status result = Helpers::getLocsInRange( &txn,
                                                     range,
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
