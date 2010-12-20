/** @file perftests.cpp.cpp : unit tests relating to performance

          The idea herein is tests that run fast and can be part of the normal CI suite.  So no tests herein that take 
          a long time to run.  Obviously we need those too, but they will be separate.

          These tests use DBDirectClient; they are a bit white-boxish.
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
#include "../db/query.h"
#include "../db/db.h"
#include "../db/instance.h"
#include "../db/json.h"
#include "../db/lasterror.h"
#include "../db/update.h"
#include "../util/timer.h"
#include "dbtests.h"

namespace PerfTests {

    class ClientBase {
    public:
        // NOTE: Not bothering to backup the old error record.
        ClientBase() {
            mongo::lastError.reset( new LastError() );
        }
        virtual ~ClientBase() {
            mongo::lastError.release();
        }
    protected:
        static void insert( const char *ns, BSONObj o ) {
            client_.insert( ns, o );
        }
        static void update( const char *ns, BSONObj q, BSONObj o, bool upsert = 0 ) {
            client_.update( ns, Query( q ), o, upsert );
        }
        static bool error() {
            return !client_.getPrevError().getField( "err" ).isNull();
        }
        DBDirectClient &client() const { return client_; }
    private:
        static DBDirectClient client_;
    };
    DBDirectClient ClientBase::client_;

    class B : public ClientBase 
    { 
    protected:
        const char *ns() { return "perftest.abc"; }
        virtual void prep() = 0;
        virtual void timed() = 0;
        virtual void post() { }
        virtual string name() = 0;
        virtual unsigned long long expectation() = 0;
    public:
        void run() { 
            prep();

            Timer t;
            unsigned long long n = 0;
            while( 1 ) { 
                unsigned i;
                for( i = 0; i < 10; i++ )
                    timed();
                n += i;
                if( t.millis() > 2000 ) 
                    break;
            }
            int ms = t.millis();
            cout << n << ' ' << ms << "ms" << " expect:" << expectation() << endl;

            if( n < expectation() ) { 
#if !defined(_DEBUG)
                cout << "test " << name() << " seems slow n:" << n << " expect greater than:" << expectation() << endl;
                assert(false);
#endif
            }
        }
    };

    class InsertDup : public B { 
        const BSONObj o;
    public:
        InsertDup() : o( BSON("_id" << 1) ) { }
        string name() { 
            return "InsertDup"; 
        }
        void prep() { 
            client().insert( ns(), o );
        }
        void timed() {
            client().insert( ns(), o );
        }
        void post() {
            assert( client().count(ns()) == 1 );
        }
        unsigned long long expectation() { return 1000; }
    };
        
    class Update : public B { 
    public:
        string name() { return "update"; }
        void prep() { }
        void timed() {
            client().insert( ns(), fromjson( "{'_id':0}" ) );
        }
        unsigned long long expectation() { return 1000; }
    };
        
    class All : public Suite {
    public:
        All() : Suite( "perf" ) {
        }
        void setupTests(){
            add< InsertDup >();
            add< Update >();
        }
    } myall;
}

