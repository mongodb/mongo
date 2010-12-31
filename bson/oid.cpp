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

BOOST_STATIC_ASSERT( sizeof(mongo::OID) == 12 );

namespace mongo {

    // machine # before folding in the process id
    OID::MachineAndPid OID::ourMachine;

    unsigned OID::ourPid() { 
        unsigned pid;
#if defined(_WIN32)
        pid = (unsigned short) GetCurrentProcessId();
#elif defined(__linux__)
        pid = (unsigned short) getpid();
#else
        pid = (unsigned short) security.getNonce();
#endif
        return pid;
    }

    void OID::foldInPid(OID::MachineAndPid& x) { 
        unsigned p = ourPid();
        x._pid ^= (unsigned short) p;
        // when the pid is greater than 16 bits, let the high bits modulate the machine id field.
        unsigned short& rest = (unsigned short &) x._machineNumber[1];
        rest ^= p >> 16;
    }

    OID::MachineAndPid OID::genMachineAndPid() { 
        BOOST_STATIC_ASSERT( sizeof(mongo::OID::MachineAndPid) == 5 );

        // this is not called often, so the following is not expensive, and gives us some 
        // testing that nonce generation is working right and that our OIDs are (perhaps) ok.
        {
            nonce a = security.getNonce();
            nonce b = security.getNonce();
            nonce c = security.getNonce();
            assert( !(a==b && b==c) );
        }

        unsigned long long n = security.getNonce();
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
        return (unsigned&) x[0];
    }

    void OID::justForked() {
        MachineAndPid x = ourMachine;
        // we let the random # for machine go into all 5 bytes of MachineAndPid, and then 
        // xor in the pid into _pid.  this reduces the probability of collisions.
        foldInPid(x);
        ourMachineAndPid = genMachineAndPid();
        assert( x != ourMachineAndPid );
        ourMachineAndPid = x;
    }
    
    void OID::init() {
        static AtomicUInt inc = (unsigned) security.getNonce();

        {
            unsigned t = (unsigned) time(0);
            unsigned char *T = (unsigned char *) &t;
            _time[0] = T[3]; // big endian order because we use memcmp() to compare OID's 
            _time[1] = T[2];
            _time[2] = T[1];
            _time[3] = T[0];
        }

        _machineAndPid = ourMachineAndPid;

        {
            int new_inc = inc++;
            unsigned char *T = (unsigned char *) &new_inc;
            _inc[0] = T[2];
            _inc[1] = T[1];
            _inc[2] = T[0];
        }
    }

    void OID::init( string s ){
        assert( s.size() == 24 );
        const char *p = s.c_str();
        for( int i = 0; i < 12; i++ ) {
            data[i] = fromHex(p);
            p += 2;
        }
    }

    void OID::init(Date_t date, bool max){
        int time = (int) (date / 1000);
        char* T = (char *) &time;
        data[0] = T[3];
        data[1] = T[2];
        data[2] = T[1];
        data[3] = T[0];

        if (max)
            *(long long*)(data + 4) = 0xFFFFFFFFFFFFFFFFll;
        else
            *(long long*)(data + 4) = 0x0000000000000000ll;
    }

    time_t OID::asTimeT(){
        int time;
        char* T = (char *) &time;
        T[0] = data[3];
        T[1] = data[2];
        T[2] = data[1];
        T[3] = data[0];
        return time;
    }

}
