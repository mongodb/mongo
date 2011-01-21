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

#include "../util/ip_addr.h"

#include "dbtests.h"


namespace IPAddrTests {

    class Base {
        dblock lk;
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
            return "unittests.ipaddrtests";
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
        IP_Addr ip1;
    };

    // -------- IPv4 Tests ---------

    class SimpleIPv4 : public Base {
    public:
        void run() {
            ASSERT( ip1.parseIPv4("192.168.1.2") );
            ASSERT( 192 == ip1.getByte(0) );
            ASSERT( 168 == ip1.getByte(1) );
            ASSERT( 1   == ip1.getByte(2) );
            ASSERT( 2   == ip1.getByte(3) );
            ASSERT( 32  == ip1.getNetmask() );
        }
    };

    class MalformedIPv4 : public Base {
    public:
        void run() {
            ASSERT( !ip1.parseIPv4("192") );
            ASSERT( !ip1.parseIPv4("192.") );
            ASSERT( !ip1.parseIPv4("192.168") );
            ASSERT( !ip1.parseIPv4("192.168.") );
            ASSERT( !ip1.parseIPv4("192.168.1") );
            ASSERT( !ip1.parseIPv4("192.168.1.") );
            ASSERT( !ip1.parseIPv4("192.168.1.2 ") );
        }
    };

    class OutOfBoundsIPv4 : public Base {
    public:
        void run() {
            ASSERT( !ip1.parseIPv4("256.1.1.1") );
            ASSERT( !ip1.parseIPv4("1.256.1.1") );
            ASSERT( !ip1.parseIPv4("1.1.256.1") );
            ASSERT( !ip1.parseIPv4("1.1.1.256") );
        }
    };

    class ClassAIPv4 : public Base {
    public:
        void run() {
            ASSERT( ip1.parseIPv4("1/8") );
            ASSERT_EQUALS( 8,    ip1.getNetmask() );
            ASSERT( ip1.parseIPv4("1.2/8") );
            ASSERT( ip1.parseIPv4("1.2.3/8") );
            ASSERT( ip1.parseIPv4("1.2.3.4/8") );
            ASSERT( ip1.parseIPv4("1.2/16") );
            ASSERT_EQUALS( 16,   ip1.getNetmask() );
            ASSERT( ip1.parseIPv4("192.168.0.0/16") );
            ASSERT( ip1.parseIPv4("1.2.3/16") );
            ASSERT( ip1.parseIPv4("1.2.3.4/16") );
            ASSERT( ip1.parseIPv4("63/8") );
        }
    };

    class ClassAOddIPv4 : public Base {
    public:
        void run() {
            ASSERT( ip1.parseIPv4("0.0.0.0") );
            ASSERT( ip1.parseIPv4("1./8") );
            ASSERT( ip1.parseIPv4("1.2./8") );
            ASSERT( ip1.parseIPv4("1.2.3./8") );
            ASSERT( ip1.parseIPv4("1.2.3.4/32") );
        }
    };

    class MalformedClassAIPv4 : public Base {
    public:
        void run() {
            ASSERT( !ip1.parseIPv4("1.2.3./7") );
            ASSERT( !ip1.parseIPv4("1.2.3./0") );
            ASSERT( !ip1.parseIPv4("1.2.3./33") );
        }
    };

    class ClassBIPv4 : public Base {
    public:
        void run() {
            ASSERT( ip1.parseIPv4("64.2/16") );
            ASSERT( ip1.parseIPv4("64.2.3/16") );
            ASSERT( ip1.parseIPv4("64.2.3.4/16") );
            ASSERT( ip1.parseIPv4("64.2.3/24") );
            ASSERT( ip1.parseIPv4("64.2.3.4/24") );
            ASSERT( ip1.parseIPv4("191.1/16") );
        }
    };

    class ClassCIPv4 : public Base {
    public:
        void run() {
            ASSERT( ip1.parseIPv4("192.2.3/24") );
            ASSERT( ip1.parseIPv4("192.2.3.4/24") );
            ASSERT( ip1.parseIPv4("223.1.2/24") );
        }
    };

    class OtherMalformedIPv4 : public Base {
    public:
        void run() {
            ASSERT( !ip1.parseIPv4("") );
            ASSERT( !ip1.parseIPv4(" ") );
        }
    };


    // -------- IPv6 Tests ---------

    class SimpleIPv6 : public Base {
    public:
        void run() {
            ASSERT( ip1.parseIPv6("::192.168.1.2") );
            ASSERT( ip1.getByte(0) == 0 );
            ASSERT( ip1.getByte(1) == 0 );
            ASSERT( ip1.getByte(2) == 0 );
            ASSERT( ip1.getByte(3) == 0 );
            ASSERT( ip1.getByte(4) == 0 );
            ASSERT( ip1.getByte(5) == 0 );
            ASSERT( ip1.getByte(6) == 0 );
            ASSERT( ip1.getByte(7) == 0 );
            ASSERT( ip1.getByte(8) == 0 );
            ASSERT( ip1.getByte(9) == 0 );
            ASSERT( ip1.getByte(10) == 0 );
            ASSERT( ip1.getByte(11) == 0 );
            ASSERT( ip1.getByte(12) == 192 );
            ASSERT( ip1.getByte(13) == 168 );
            ASSERT( ip1.getByte(14) == 1 );
            ASSERT( ip1.getByte(15) == 2 );
            ASSERT( 128  == ip1.getNetmask() );

            ASSERT( ip1.parseIPv6("1234::192.168.1.2") );
            ASSERT( ip1.getByte(0) == 0x12 );
            ASSERT( ip1.getByte(1) == 0x34 );
            ASSERT( ip1.getByte(2) == 0 );
            ASSERT( ip1.getByte(3) == 0 );
            ASSERT( ip1.getByte(4) == 0 );
            ASSERT( ip1.getByte(5) == 0 );
            ASSERT( ip1.getByte(6) == 0 );
            ASSERT( ip1.getByte(7) == 0 );
            ASSERT( ip1.getByte(8) == 0 );
            ASSERT( ip1.getByte(9) == 0 );
            ASSERT( ip1.getByte(10) == 0 );
            ASSERT( ip1.getByte(11) == 0 );
            ASSERT( ip1.getByte(12) == 192 );
            ASSERT( ip1.getByte(13) == 168 );
            ASSERT( ip1.getByte(14) == 1 );
            ASSERT( ip1.getByte(15) == 2 );
        }
    };

    class MalformedSimpleIPv6 : public Base {
    public:
        void run() {
            ASSERT( !ip1.parseIPv6("1234::192.") );
            ASSERT( !ip1.parseIPv6("::192.168") );
            ASSERT( !ip1.parseIPv6("::192.168.") );
            ASSERT( !ip1.parseIPv6("::192.168.1") );
            ASSERT( !ip1.parseIPv6("::192.168.1.") );
        }
    };

    class OutOfBoundsIPv6 : public Base {
    public:
        void run() {
            ASSERT( !ip1.parseIPv6("::255.1.1.1/129") );
            ASSERT( !ip1.parseIPv6("::ABCDE") );
            ASSERT( !ip1.parseIPv6("1:2:3:4:5:6:7:8:9") );
            ASSERT( !ip1.parseIPv6("1::2::3") );
        }
    };

    class ZeroCompressionIPv6 : public Base {
    public:
        void run() {
            ASSERT( ip1.parseIPv6("1234::") );
            ASSERT( ip1.getByte(0) == 0x12 );
            ASSERT( ip1.getByte(1) == 0x34 );
            ASSERT( ip1.getByte(2) == 0 );
            ASSERT( ip1.getByte(3) == 0 );
            ASSERT( ip1.getByte(4) == 0 );
            ASSERT( ip1.getByte(5) == 0 );
            ASSERT( ip1.getByte(6) == 0 );
            ASSERT( ip1.getByte(7) == 0 );
            ASSERT( ip1.getByte(8) == 0 );
            ASSERT( ip1.getByte(9) == 0 );
            ASSERT( ip1.getByte(10) == 0 );
            ASSERT( ip1.getByte(11) == 0 );
            ASSERT( ip1.getByte(12) == 0 );
            ASSERT( ip1.getByte(13) == 0 );
            ASSERT( ip1.getByte(14) == 0 );
            ASSERT( ip1.getByte(15) == 0 );

            ASSERT( ip1.parseIPv6("::1234") );
            ASSERT( ip1.getByte(0) == 0 );
            ASSERT( ip1.getByte(1) == 0 );
            ASSERT( ip1.getByte(2) == 0 );
            ASSERT( ip1.getByte(3) == 0 );
            ASSERT( ip1.getByte(4) == 0 );
            ASSERT( ip1.getByte(5) == 0 );
            ASSERT( ip1.getByte(6) == 0 );
            ASSERT( ip1.getByte(7) == 0 );
            ASSERT( ip1.getByte(8) == 0 );
            ASSERT( ip1.getByte(9) == 0 );
            ASSERT( ip1.getByte(10) == 0 );
            ASSERT( ip1.getByte(11) == 0 );
            ASSERT( ip1.getByte(12) == 0 );
            ASSERT( ip1.getByte(13) == 0 );
            ASSERT( ip1.getByte(14) == 0x12 );
            ASSERT( ip1.getByte(15) == 0x34 );
        }
    };

    class NetmaskIPv6 : public Base {
    public:
        void run() {
            ASSERT( ip1.parseIPv6("1::/8") );
            ASSERT( ip1.getByte(0) == 0 );
            ASSERT( ip1.getByte(1) == 1 );
            ASSERT( ip1.getByte(2) == 0 );
            ASSERT( ip1.getByte(3) == 0 );
            ASSERT( ip1.getByte(4) == 0 );
            ASSERT( ip1.getByte(5) == 0 );
            ASSERT( ip1.getByte(6) == 0 );
            ASSERT( ip1.getByte(7) == 0 );
            ASSERT( ip1.getByte(8) == 0 );
            ASSERT( ip1.getByte(9) == 0 );
            ASSERT( ip1.getByte(10) == 0 );
            ASSERT( ip1.getByte(11) == 0 );
            ASSERT( ip1.getByte(12) == 0 );
            ASSERT( ip1.getByte(13) == 0 );
            ASSERT( ip1.getByte(14) == 0 );
            ASSERT( ip1.getByte(15) == 0 );
            ASSERT( ip1.getNetmask() == 8 );

            ASSERT( ip1.parseIPv6("1234:5678:9abc:4321::/64") );
            ASSERT( ip1.getByte(0) == 0x12 );
            ASSERT( ip1.getByte(1) == 0x34 );
            ASSERT( ip1.getByte(2) == 0x56 );
            ASSERT( ip1.getByte(3) == 0x78 );
            ASSERT( ip1.getByte(4) == 0x9a );
            ASSERT( ip1.getByte(5) == 0xbc );
            ASSERT( ip1.getByte(6) == 0x43 );
            ASSERT( ip1.getByte(7) == 0x21 );
            ASSERT( ip1.getByte(8) == 0 );
            ASSERT( ip1.getByte(9) == 0 );
            ASSERT( ip1.getByte(10) == 0 );
            ASSERT( ip1.getByte(11) == 0 );
            ASSERT( ip1.getByte(12) == 0 );
            ASSERT( ip1.getByte(13) == 0 );
            ASSERT( ip1.getByte(14) == 0 );
            ASSERT( ip1.getByte(15) == 0 );
            ASSERT( ip1.getNetmask() == 64 );

            ASSERT( ip1.parseIPv6("1234:5678:9abc:4321:C000::/72") );
            ASSERT( ip1.getByte(0) == 0x12 );
            ASSERT( ip1.getByte(1) == 0x34 );
            ASSERT( ip1.getByte(2) == 0x56 );
            ASSERT( ip1.getByte(3) == 0x78 );
            ASSERT( ip1.getByte(4) == 0x9a );
            ASSERT( ip1.getByte(5) == 0xbc );
            ASSERT( ip1.getByte(6) == 0x43 );
            ASSERT( ip1.getByte(7) == 0x21 );
            ASSERT( ip1.getByte(8) == 0xc0 );
            ASSERT( ip1.getByte(9) == 0 );
            ASSERT( ip1.getByte(10) == 0 );
            ASSERT( ip1.getByte(11) == 0 );
            ASSERT( ip1.getByte(12) == 0 );
            ASSERT( ip1.getByte(13) == 0 );
            ASSERT( ip1.getByte(14) == 0 );
            ASSERT( ip1.getByte(15) == 0 );
            ASSERT( ip1.getNetmask() == 72 );
        }
    };

    class AllZeroesIPv6 : public Base {
    public:
        void run() {
            ASSERT( ip1.parseIPv6("::") );
            ASSERT( ip1.getByte(0) == 0 );
            ASSERT( ip1.getByte(1) == 0 );
            ASSERT( ip1.getByte(2) == 0 );
            ASSERT( ip1.getByte(3) == 0 );
            ASSERT( ip1.getByte(4) == 0 );
            ASSERT( ip1.getByte(5) == 0 );
            ASSERT( ip1.getByte(6) == 0 );
            ASSERT( ip1.getByte(7) == 0 );
            ASSERT( ip1.getByte(8) == 0 );
            ASSERT( ip1.getByte(9) == 0 );
            ASSERT( ip1.getByte(10) == 0 );
            ASSERT( ip1.getByte(11) == 0 );
            ASSERT( ip1.getByte(12) == 0 );
            ASSERT( ip1.getByte(13) == 0 );
            ASSERT( ip1.getByte(14) == 0 );
            ASSERT( ip1.getByte(15) == 0 );
            ASSERT( ip1.getNetmask() == 128 );
        }
    };

    class MiddleZeroesIPv6 : public Base {
    public:
        void run() {
            ASSERT( ip1.parseIPv6("fe80::222:19ff:fe5d:a133") );
            ASSERT( ip1.getByte(0) == 0xfe );
            ASSERT( ip1.getByte(1) == 0x80 );
            ASSERT( ip1.getByte(2) == 0 );
            ASSERT( ip1.getByte(3) == 0 );
            ASSERT( ip1.getByte(4) == 0 );
            ASSERT( ip1.getByte(5) == 0 );
            ASSERT( ip1.getByte(6) == 0 );
            ASSERT( ip1.getByte(7) == 0 );
            ASSERT( ip1.getByte(8) == 0x02 );
            ASSERT( ip1.getByte(9) == 0x22 );
            ASSERT( ip1.getByte(10) == 0x19 );
            ASSERT( ip1.getByte(11) == 0xff );
            ASSERT( ip1.getByte(12) == 0xfe );
            ASSERT( ip1.getByte(13) == 0x5d );
            ASSERT( ip1.getByte(14) == 0xa1 );
            ASSERT( ip1.getByte(15) == 0x33 );
            ASSERT( ip1.getNetmask() == 128 );
        }
    };

    class OtherInvalidIPv6 : public Base {
    public:
        void run() {
            ASSERT( !ip1.parseIPv6("") );
            ASSERT( !ip1.parseIPv6(" ") );
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "ip_addr" ) {
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
            add< SimpleIPv6 >();
            add< MalformedSimpleIPv6 >();
            add< OutOfBoundsIPv6 >();
            add< ZeroCompressionIPv6 >();
            add< NetmaskIPv6 >();
            add< AllZeroesIPv6 >();
            add< MiddleZeroesIPv6 >();
            add< OtherInvalidIPv6 >();
        }
    } myall;

} // namespace IPAddrTests

