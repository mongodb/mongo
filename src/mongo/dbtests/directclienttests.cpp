/** @file directclienttests.cpp
*/

/**
 *    Copyright (C) 2008 10gen Inc.
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

#include "pch.h"
#include "../db/db.h"
#include "../db/instance.h"
#include "../db/json.h"
#include "../db/lasterror.h"
#include "../util/timer.h"
#include "dbtests.h"

namespace DirectClientTests {

    class ClientBase {
    public:
        // NOTE: Not bothering to backup the old error record.
        ClientBase() {  mongo::lastError.reset( new LastError() );  }
        virtual ~ClientBase() { }
    protected:
        static bool error() {
            return !_client.getPrevError().getField( "err" ).isNull();
        }
        DBDirectClient &client() const { return _client; }
    private:
        static DBDirectClient _client;
    };
    DBDirectClient ClientBase::_client;

    const char *ns = "a.b";

    class Capped : public ClientBase {
    public:
        virtual void run() {
            for( int pass=0; pass < 3; pass++ ) {
                client().createCollection(ns, 1024 * 1024, true, 999);
                for( int j =0; j < pass*3; j++ )
                    client().insert(ns, BSON("x" << j));

                // test truncation of a capped collection
                if( pass ) {
                    BSONObj info;
                    BSONObj cmd = BSON( "captrunc" << "b" << "n" << 1 << "inc" << true );
                    //cout << cmd.toString() << endl;
                    bool ok = client().runCommand("a", cmd, info);
                    //cout << info.toString() << endl;
                    verify(ok);
                }

                verify( client().dropCollection(ns) );
            }
        }
    };

    class InsertMany : ClientBase {
    public:
        virtual void run(){
            vector<BSONObj> objs;
            objs.push_back(BSON("_id" << 1));
            objs.push_back(BSON("_id" << 1));
            objs.push_back(BSON("_id" << 2));


            client().dropCollection(ns);
            client().insert(ns, objs);
            ASSERT_EQUALS(client().getLastErrorDetailed()["code"].numberInt(), 11000);
            ASSERT_EQUALS((int)client().count(ns), 1);

            client().dropCollection(ns);
            client().insert(ns, objs, InsertOption_ContinueOnError);
            ASSERT_EQUALS(client().getLastErrorDetailed()["code"].numberInt(), 11000);
            ASSERT_EQUALS((int)client().count(ns), 2);
        }

    };

    class All : public Suite {
    public:
        All() : Suite( "directclient" ) {
        }
        void setupTests() {
            add< Capped >();
            add< InsertMany >();
        }
    } myall;
}
