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

#pragma once

#include <boost/thread/condition.hpp>
#include <iostream>
#include <sstream>

#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/time_support.h"

namespace mongo {

    struct ClockSkewException : public DBException {
        ClockSkewException() : DBException( "clock skew exception" , 20001 ) {}
    };

    /* replsets used to use RSOpTime.
       M/S uses OpTime.
       But this is useable from both.
       */
    typedef unsigned long long ReplTime;

    /* Operation sequence #.  A combination of current second plus an ordinal value.
     */
#pragma pack(4)
    class OpTime {
        unsigned i; // ordinal comes first so we can do a single 64 bit compare on little endian
        unsigned secs;
    public:
        unsigned getSecs() const {
            return secs;
        }
        unsigned getInc() const {
            return i;
        }

        OpTime(Date_t date) {
            reinterpret_cast<unsigned long long&>(*this) = date.millis;
            dassert( (int)secs >= 0 );
        }
        OpTime(ReplTime x) {
            reinterpret_cast<unsigned long long&>(*this) = x;
            dassert( (int)secs >= 0 );
        }
        OpTime(unsigned a, unsigned b) {
            secs = a;
            i = b;
            dassert( (int)secs >= 0 );
        }
        OpTime( const OpTime& other ) { 
            secs = other.secs;
            i = other.i;
            dassert( (int)secs >= 0 );
        }
        OpTime() {
            secs = 0;
            i = 0;
        }

        // Maximum OpTime value.
        static OpTime max();

        /* We store OpTime's in the database as BSON Date datatype -- we needed some sort of
         64 bit "container" for these values.  While these are not really "Dates", that seems a
         better choice for now than say, Number, which is floating point.  Note the BinData type
         is perhaps the cleanest choice, lacking a true unsigned64 datatype, but BinData has 5
         bytes of overhead.
         */
        unsigned long long asDate() const {
            return reinterpret_cast<const unsigned long long*>(&i)[0];
        }
        long long asLL() const {
            return reinterpret_cast<const long long*>(&i)[0];
        }

        bool isNull() const { return secs == 0; }

        std::string toStringLong() const {
            std::stringstream ss;
            ss << time_t_to_String_short(secs) << ' ';
            ss << std::hex << secs << ':' << i;
            return ss.str();
        }

        std::string toStringPretty() const {
            std::stringstream ss;
            ss << time_t_to_String_short(secs) << ':' << std::hex << i;
            return ss.str();
        }

        std::string toString() const {
            std::stringstream ss;
            ss << std::hex << secs << ':' << i;
            return ss.str();
        }

        bool operator==(const OpTime& r) const {
            return i == r.i && secs == r.secs;
        }
        bool operator!=(const OpTime& r) const {
            return !(*this == r);
        }
        bool operator<(const OpTime& r) const {
            if ( secs != r.secs )
                return secs < r.secs;
            return i < r.i;
        }
        bool operator<=(const OpTime& r) const {
            return *this < r || *this == r;
        }
        bool operator>(const OpTime& r) const {
            return !(*this <= r);
        }
        bool operator>=(const OpTime& r) const {
            return !(*this < r);
        }
    };
#pragma pack()

} // namespace mongo
