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
        static OpTime last;
        static OpTime skewed();
    public:
        static void setLast(const Date_t &date) {
            mutex::scoped_lock lk(m);
            last = OpTime(date);
            notifier.notify_all();
        }
        static void setLast(const OpTime &new_last) {
            mutex::scoped_lock lk(m);
            last = new_last;
            notifier.notify_all();
        }

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
        // it isn't generally safe to not be locked for this. so use now(). some tests use this.
        static OpTime _now();

        static mongo::mutex m;

        static OpTime now(const mongo::mutex::scoped_lock&);

        static OpTime getLast(const mongo::mutex::scoped_lock&);

        // Waits for global OpTime to be different from *this
        void waitForDifferent(unsigned millis);

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

        string toStringLong() const {
            std::stringstream ss;
            ss << time_t_to_String_short(secs) << ' ';
            ss << std::hex << secs << ':' << i;
            return ss.str();
        }

        string toStringPretty() const {
            std::stringstream ss;
            ss << time_t_to_String_short(secs) << ':' << std::hex << i;
            return ss.str();
        }

        string toString() const {
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
    private:
        static boost::condition notifier;
    };
#pragma pack()

} // namespace mongo
