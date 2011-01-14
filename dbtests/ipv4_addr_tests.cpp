// ipv4_addr_tests.cpp : unit tests
//

/**
 *    Copyright (C) 2005-2011 Intrusion Inc
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Lesser General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pch.h"

#include "../util/ipv4_addr.h"

#include "dbtests.h"


namespace IPv4AddrTests {

    class Base {
        Client::Context _context;
    public:
        Base() : _context( ns() ) {
            addIndex( fromjson( "{\"a\":1}" ) );
        }
        ~Base() {
            try {
                boost::shared_ptr<Cursor> c = theDataFileMgr.findAll( ns() );
                vector< DiskLoc > toDelete;
                for(; c->ok(); c->advance() )
                    toDelete.push_back( c->currLoc() );
                for( vector< DiskLoc >::iterator i = toDelete.begin(); i != toDelete.end(); ++i )
                    theDataFileMgr.deleteRecord( ns(), i->rec(), *i, false );
            }
            catch ( ... ) {
                FAIL( "Exception while cleaning up records" );
            }
        }
    protected:
        static const char *ns() {
            return "unittests.ipv4addrtests";
        }
        static void addIndex( const BSONObj &key ) {
            BSONObjBuilder b;
            b.append( "name", key.firstElement().fieldName() );
            b.append( "ns", ns() );
            b.append( "key", key );
            BSONObj o = b.done();
            stringstream indexNs;
            indexNs << "unittests.system.indexes";
            theDataFileMgr.insert( indexNs.str().c_str(), o.objdata(), o.objsize() );
        }
        static void insert( const char *s ) {
            insert( fromjson( s ) );
        }
        static void insert( const BSONObj &o ) {
            theDataFileMgr.insert( ns(), o.objdata(), o.objsize() );
        }
        IPv4_Addr ip1;
    };

    class SimpleIPv4 : public Base {
    public:
        void run() {
        //    ASSERT( ip1.parse("192.168.1.2") );
        //    ASSERT_EQUALS( 192, ip1.m_ip[0] );
        //    ASSERT_EQUALS( 168, ip1.m_ip[1] );
        //    ASSERT_EQUALS( 1,   ip1.m_ip[2] );
        //    ASSERT_EQUALS( 2,   ip1.m_ip[3] );
        //    ASSERT_EQUALS( 32,  ip1.m_mask );
        }
    };

    class MalformedIPv4 : public Base {
    public:
        void run() {
            ASSERT( !ip1.parse("192") );
            ASSERT( !ip1.parse("192.") );
            ASSERT( !ip1.parse("192.168") );
            ASSERT( !ip1.parse("192.168.") );
            ASSERT( !ip1.parse("192.168.1") );
            ASSERT( !ip1.parse("192.168.1.") );
            ASSERT( !ip1.parse("192.168.1.2 ") );
        }
    };

    class OutOfBoundsIPv4 : public Base {
    public:
        void run() {
            ASSERT( !ip1.parse("256.1.1.1") );
            ASSERT( !ip1.parse("1.256.1.1") );
            ASSERT( !ip1.parse("1.1.256.1") );
            ASSERT( !ip1.parse("1.1.1.256") );
            ASSERT( !ip1.parse("0.0.0.0") );
        }
    };

    class ClassAIPv4 : public Base {
    public:
        void run() {
            ASSERT( ip1.parse("1/8") );
            ASSERT_EQUALS( 8,    ip1.m_mask );
            ASSERT( ip1.parse("1.2/8") );
            ASSERT( ip1.parse("1.2.3/8") );
            ASSERT( ip1.parse("1.2.3.4/8") );
            ASSERT( ip1.parse("1.2/16") );
            ASSERT_EQUALS( 16,   ip1.m_mask );
            ASSERT( ip1.parse("192.168.0.0/16") );
            ASSERT( ip1.parse("1.2.3/16") );
            ASSERT( ip1.parse("1.2.3.4/16") );
            ASSERT( ip1.parse("63/8") );
        }
    };

    class ClassAOddIPv4 : public Base {
    public:
        void run() {
            ASSERT( ip1.parse("1./8") );
            ASSERT( ip1.parse("1.2./8") );
            ASSERT( ip1.parse("1.2.3./8") );
            ASSERT( ip1.parse("1.2.3.4/32") );
        }
    };

    class MalformedClassAIPv4 : public Base {
    public:
        void run() {
            ASSERT( !ip1.parse("1.2.3./7") );
            ASSERT( !ip1.parse("1.2.3./0") );
            ASSERT( !ip1.parse("1.2.3./33") );
        }
    };

    class ClassBIPv4 : public Base {
    public:
        void run() {
            ASSERT( ip1.parse("64.2/16") );
            ASSERT( ip1.parse("64.2.3/16") );
            ASSERT( ip1.parse("64.2.3.4/16") );
            ASSERT( ip1.parse("64.2.3/24") );
            ASSERT( ip1.parse("64.2.3.4/24") );
            ASSERT( ip1.parse("191.1/16") );
        }
    };

    class ClassCIPv4 : public Base {
    public:
        void run() {
            ASSERT( ip1.parse("192.2.3/24") );
            ASSERT( ip1.parse("192.2.3.4/24") );
            ASSERT( ip1.parse("223.1.2/24") );
        }
    };

    class OtherMalformedIPv4 : public Base {
    public:
        void run() {
            ASSERT( !ip1.parse("") );
            ASSERT( !ip1.parse(" ") );
        }
    };


    class All : public Suite {
    public:
        All() : Suite( "ipv4" ) {
        }

        void setupTests() {
            add< SimpleIPv4 >();
            add< MalformedIPv4 >();
            add< OutOfBoundsIPv4 >();
            add< ClassAIPv4 >();
            add< ClassAOddIPv4 >();
            add< MalformedClassAIPv4 >();
            add< ClassBIPv4 >();
            add< ClassCIPv4 >();
            add< OtherMalformedIPv4 >();
        }
    } myall;

} // namespace IPv4AddrTests

