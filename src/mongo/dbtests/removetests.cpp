// removetests.cpp : unit tests relating to removing documents
//

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

#include "../db/dbhelpers.h"
#include "mongo/client/dbclientcursor.h"

#include "dbtests.h"

namespace RemoveTests {

    static const char * const ns = "unittests.removetests";
    static DBDirectClient client;
    
    /** Simple test for Helpers::RemoveRange. */
    class RemoveRange {
    public:
        RemoveRange() :
        _min( 4 ),
        _max( 8 ) {
        }
        void run() {
            for( int i = 0; i < 10; ++i ) {
                client.insert( ns, BSON( "_id" << i ) );
            }
            
            {
                // Remove _id range [_min, _max).
                Lock::DBWrite lk(ns);
                Client::Context ctx( ns );
                Helpers::removeRange( ns,
                                      BSON( "_id" << _min ),
                                      BSON( "_id" << _max ),
                                      BSON( "_id" << 1 ) );
            }

            // Check that the expected documents remain.
            ASSERT_EQUALS( expected(), docs() );
        }
    private:
        BSONArray expected() const {
            BSONArrayBuilder bab;
            for( int i = 0; i < _min; ++i ) {
                bab << BSON( "_id" << i );
            }
            for( int i = _max; i < 10; ++i ) {
                bab << BSON( "_id" << i );
            }
            return bab.arr();
        }
        BSONArray docs() const {
            auto_ptr<DBClientCursor> cursor =
                    client.query( ns, Query().hint( BSON( "_id" << 1 ) ) );
            BSONArrayBuilder bab;
            while( cursor->more() ) {
                bab << cursor->next();
            }
            return bab.arr();
        }
        int _min;
        int _max;
    };
    
    class ModifiedRangeBoundTest{
    public:
        void run(){

            BSONObj bound = BSON( "a" << 55 );
            BSONObj longBound = BSON("a" << 55 << "b" << 66);

            //test keyPattern shorter than bound, should fail
            {
                BSONObj keyPat = BSONObj();
                ASSERT_THROWS( Helpers::modifiedRangeBound( bound , keyPat , 1 ) ,
                               MsgAssertionException );

            }

            //test keyPattern doesn't match bound, should fail
            {
                BSONObj keyPat = BSON( "b" << 1 );
                ASSERT_THROWS( Helpers::modifiedRangeBound( bound , keyPat , 1 ) ,
                               MsgAssertionException );
            }
            {
                BSONObj keyPat = BSON( "a" << 1 << "c" << 1);
                ASSERT_THROWS( Helpers::modifiedRangeBound( longBound , keyPat , 1 ) ,
                               MsgAssertionException );

            }

            //test keyPattern same as bound
            {
                BSONObj keyPat = BSON( "a" << 1 );
                BSONObj newB = Helpers::modifiedRangeBound( bound , keyPat , -1 );
                ASSERT_EQUALS( newB , BSON("" << 55) );
            }
            {
                BSONObj keyPat = BSON( "a" << 1 );
                BSONObj newB = Helpers::modifiedRangeBound( bound , keyPat , 1 );
                ASSERT_EQUALS( newB , BSON("" << 55) );
            }

            //test keyPattern longer than bound, simple
            {
                BSONObj keyPat = BSON( "a" << 1 << "b" << 1);
                BSONObj newB = Helpers::modifiedRangeBound( bound , keyPat , -1 );
                ASSERT_EQUALS( newB , BSON("" << 55 << "" << MINKEY ) );
            }
            {
                BSONObj keyPat = BSON( "a" << 1 << "b" << 1);
                BSONObj newB = Helpers::modifiedRangeBound( bound , keyPat , 1 );
                ASSERT_EQUALS( newB , BSON("" << 55 << "" << MAXKEY ) );
            }

            //test keyPattern longer than bound, more complex pattern directions
            {
                BSONObj keyPat = BSON( "a" << 1 << "b" << -1);
                BSONObj newB = Helpers::modifiedRangeBound( bound , keyPat , -1 );
                ASSERT_EQUALS( newB , BSON("" << 55 << "" << MAXKEY ) );
            }
            {
                BSONObj keyPat = BSON( "a" << 1 << "b" << -1);
                BSONObj newB = Helpers::modifiedRangeBound( bound , keyPat , 1 );
                ASSERT_EQUALS( newB , BSON("" << 55 << "" << MINKEY ) );
            }
            {

                BSONObj keyPat = BSON( "a" << 1 << "b" << -1 << "c" << 1 );
                BSONObj newB = Helpers::modifiedRangeBound( bound , keyPat , -1 );
                ASSERT_EQUALS( newB , BSON("" << 55 << "" << MAXKEY << "" << MINKEY ) );
            }
            {
                BSONObj keyPat = BSON( "a" << 1 << "b" << -1 << "c" << 1 );
                BSONObj newB = Helpers::modifiedRangeBound( bound , keyPat , 1 );
                ASSERT_EQUALS( newB , BSON("" << 55 << "" << MINKEY << "" << MAXKEY ) );
            }

        }
    };

    class All : public Suite {
    public:
        All() : Suite( "remove" ) {
        }
        void setupTests() {
            add<ModifiedRangeBoundTest>();
            add<RemoveRange>();
        }
    } myall;
    
} // namespace RemoveTests
