// @file oid.cpp

/*    Copyright 2009 10gen Inc.
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

#include <boost/functional/hash.hpp>

#include "mongo/platform/atomic_word.h"
#include "mongo/platform/process_id.h"
#include "mongo/platform/random.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"

#define verify MONGO_verify

BOOST_STATIC_ASSERT( sizeof(mongo::OID) == mongo::OID::kOIDSize );
BOOST_STATIC_ASSERT( sizeof(mongo::OID) == 12 );

namespace mongo {

    void OID::hash_combine(size_t &seed) const {
        boost::hash_combine(seed, x);
        boost::hash_combine(seed, y);
        boost::hash_combine(seed, z);
    }

    size_t OID::Hasher::operator() (const OID& oid) const {
        size_t seed = 0;
        oid.hash_combine(seed);
        return seed;
    }

    // machine # before folding in the process id
    OID::MachineAndPid OID::ourMachine;

    ostream& operator<<( ostream &s, const OID &o ) {
        s << o.str();
        return s;
    }

    void OID::foldInPid(OID::MachineAndPid& x) {
        unsigned p = ProcessId::getCurrent().asUInt32();
        x._pid ^= static_cast<unsigned short>(p);
        // when the pid is greater than 16 bits, let the high bits modulate the machine id field.
        unsigned short& rest = (unsigned short &) x._machineNumber[1];
        rest ^= p >> 16;
    }

    OID::MachineAndPid OID::genMachineAndPid() {
        BOOST_STATIC_ASSERT( sizeof(mongo::OID::MachineAndPid) == 5 );

        // we only call this once per process
        scoped_ptr<SecureRandom> sr( SecureRandom::create() );
        int64_t n = sr->nextInt64();
        OID::MachineAndPid x = ourMachine = reinterpret_cast<OID::MachineAndPid&>(n);
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
        verify( x != ourMachineAndPid );
        ourMachineAndPid = x;
    }

    void OID::init() {
        static AtomicUInt32 inc(
            static_cast<unsigned>(
                scoped_ptr<SecureRandom>(SecureRandom::create())->nextInt64()));

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
            int new_inc = inc.fetchAndAdd(1);
            unsigned char *T = (unsigned char *) &new_inc;
            _inc[0] = T[2];
            _inc[1] = T[1];
            _inc[2] = T[0];
        }
    }

    static AtomicUInt64 _initSequential_sequence;
    void OID::initSequential() {

        {
            unsigned t = (unsigned) time(0);
            unsigned char *T = (unsigned char *) &t;
            _time[0] = T[3]; // big endian order because we use memcmp() to compare OID's
            _time[1] = T[2];
            _time[2] = T[1];
            _time[3] = T[0];
        }
        
        {
            unsigned long long nextNumber = _initSequential_sequence.fetchAndAdd(1);
            unsigned char* numberData = reinterpret_cast<unsigned char*>(&nextNumber);
            for ( int i=0; i<8; i++ ) {
                data[4+i] = numberData[7-i];
            }
        }
    }

    void OID::init( const std::string& s ) {
        verify( s.size() == 24 );
        const char *p = s.c_str();
        for( size_t i = 0; i < kOIDSize; i++ ) {
            data[i] = fromHex(p);
            p += 2;
        }
    }

    void OID::init(Date_t date, bool max) {
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

    time_t OID::asTimeT() {
        int time;
        char* T = (char *) &time;
        T[0] = data[3];
        T[1] = data[2];
        T[2] = data[1];
        T[3] = data[0];
        return time;
    }

}
