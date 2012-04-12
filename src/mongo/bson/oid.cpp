// @file oid.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "pch.h"
#include "oid.h"
#include "util/atomic_int.h"
#include "../db/nonce.h"
#include "bsonobjbuilder.h"
#include <boost/functional/hash.hpp>
#define verify MONGO_verify

BOOST_STATIC_ASSERT( sizeof(mongo::OID) == 12 );

namespace mongo {

	void OID::hash_combine(size_t &seed) const {
	    boost::hash_combine(seed, x);
	    boost::hash_combine(seed, y);
	    boost::hash_combine(seed, z);
	}

    // machine # before folding in the process id
    OID::MachineAndPid OID::ourMachine;

    ostream& operator<<( ostream &s, const OID &o ) {
        s << o.str();
        return s;
    }

    unsigned OID::ourPid() {
        unsigned pid;
#if defined(_WIN32)
        pid = (unsigned short) GetCurrentProcessId();
#elif defined(__linux__) || defined(__APPLE__) || defined(__sunos__)
        pid = (unsigned short) getpid();
#else
        pid = (unsigned short) Security::getNonce();
#endif
        return pid;
    }

    void OID::foldInPid(OID::MachineAndPid& x) {
        unsigned p = ourPid();
        x._pid ^= (unsigned short) p;
        // when the pid is greater than 16 bits, let the high bits modulate the machine id field.
         little<unsigned short>& rest = little<unsigned short>::ref (&x._machineNumber[1]);
        rest ^= p >> 16;
    }

    OID::MachineAndPid OID::genMachineAndPid() {
        BOOST_STATIC_ASSERT( sizeof(mongo::OID::MachineAndPid) == 5 );

        // this is not called often, so the following is not expensive, and gives us some
        // testing that nonce generation is working right and that our OIDs are (perhaps) ok.
        {
            nonce64 a = Security::getNonceDuringInit();
            nonce64 b = Security::getNonceDuringInit();
            nonce64 c = Security::getNonceDuringInit();
            verify( !(a==b && b==c) );
        }

        unsigned long long n = Security::getNonceDuringInit();
        OID::MachineAndPid x = ourMachine = (OID::MachineAndPid&) n;
        foldInPid(x);
        return x;
    }

    // after folding in the process id
    OID::MachineAndPid OID::ourMachineAndPid = OID::genMachineAndPid();

    void OID::regenMachineId() {
        ourMachineAndPid = genMachineAndPid();
    }

    inline bool OID::MachineAndPid::operator!=(const OID::MachineAndPid& rhs) const {
        return _pid != rhs._pid || _machineNumber != rhs._machineNumber;
    }

    unsigned OID::getMachineId() {
        unsigned char x[4];
        x[0] = ourMachineAndPid._machineNumber[0];
        x[1] = ourMachineAndPid._machineNumber[1];
        x[2] = ourMachineAndPid._machineNumber[2];
        x[3] = 0;
        return little<unsigned>::ref( x );
    }

    void OID::justForked() {
        MachineAndPid x = ourMachine;
        // we let the random # for machine go into all 5 bytes of MachineAndPid, and then
        // xor in the pid into _pid.  this reduces the probability of collisions.
        foldInPid(x);
        ourMachineAndPid = genMachineAndPid();
        verify( x != ourMachineAndPid );
        ourMachineAndPid = x;
    }

    void OID::init() {
        static AtomicUInt inc = (unsigned) Security::getNonce();

        big<unsigned>::ref( _time ) = time( 0 );

        _machineAndPid = ourMachineAndPid;

        {
            // 24 bits big endian
            unsigned int new_inc = inc++;
            _inc[0] = new_inc >> 16;
            _inc[1] = new_inc >>  8;
            _inc[2] = new_inc;
        }
    }

    void OID::init( string s ) {
        verify( s.size() == 24 );
        const char *p = s.c_str();
        for( int i = 0; i < 12; i++ ) {
            data[i] = fromHex(p);
            p += 2;
        }
    }

    void OID::init(Date_t date, bool max) {
        int time = (int) (date / 1000);
        big<unsigned>::ref( data ) = time;
        
        if (max)
            little<long long>::ref(data + 4) = 0xFFFFFFFFFFFFFFFFll;
        else
            little<long long>::ref(data + 4) = 0x0000000000000000ll;
    }

    time_t OID::asTimeT() {
        return big<int>::ref( data );
    }

    const string BSONObjBuilder::numStrs[] = {
        "0",  "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",  "9",
        "10", "11", "12", "13", "14", "15", "16", "17", "18", "19",
        "20", "21", "22", "23", "24", "25", "26", "27", "28", "29",
        "30", "31", "32", "33", "34", "35", "36", "37", "38", "39",
        "40", "41", "42", "43", "44", "45", "46", "47", "48", "49",
        "50", "51", "52", "53", "54", "55", "56", "57", "58", "59",
        "60", "61", "62", "63", "64", "65", "66", "67", "68", "69",
        "70", "71", "72", "73", "74", "75", "76", "77", "78", "79",
        "80", "81", "82", "83", "84", "85", "86", "87", "88", "89",
        "90", "91", "92", "93", "94", "95", "96", "97", "98", "99",
    };

    // This is to ensure that BSONObjBuilder doesn't try to use numStrs before the strings have been constructed
    // I've tested just making numStrs a char[][], but the overhead of constructing the strings each time was too high
    // numStrsReady will be 0 until after numStrs is initialized because it is a static variable
    bool BSONObjBuilder::numStrsReady = (numStrs[0].size() > 0);

}
