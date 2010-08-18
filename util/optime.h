// optime.h - OpTime class

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

#include "../db/concurrency.h"

namespace mongo {
    void exitCleanly( ExitCode code );
    
    struct ClockSkewException : public DBException {
        ClockSkewException() : DBException( "clock skew exception" , 20001 ){}
    };

    /* replsets use RSOpTime.
       M/S uses OpTime.
       But this is useable from both.
       */
    typedef unsigned long long ReplTime;

    /* Operation sequence #.  A combination of current second plus an ordinal value.
     */
#pragma pack(4)
    class OpTime {
        unsigned i;
        unsigned secs;
        static OpTime last;
    public:
        static void setLast(const Date_t &date) {
            last = OpTime(date);
        }
        unsigned getSecs() const {
            return secs;
        }
        OpTime(Date_t date) {
            reinterpret_cast<unsigned long long&>(*this) = date.millis;
        }
        OpTime(ReplTime x) {
            reinterpret_cast<unsigned long long&>(*this) = x;
        }
        OpTime(unsigned a, unsigned b) {
            secs = a;
            i = b;
        }
        OpTime() {
            secs = 0;
            i = 0;
        }
        static OpTime now() {
            unsigned t = (unsigned) time(0);
//            DEV assertInWriteLock();
            if ( t < last.secs ){
                bool toLog = false;
                ONCE toLog = true;
                RARELY toLog = true;
                if ( last.i & 0x80000000 )
                    toLog = true;
                if ( toLog )
                    log() << "clock skew detected  prev: " << last.secs << " now: " << t << " trying to handle..." << endl;
                if ( last.i & 0x80000000 ) {
                    log() << "ERROR Large clock skew detected, shutting down" << endl;
                    throw ClockSkewException();
                }
                t = last.secs;
            }
            if ( last.secs == t ) {
                last.i++;
                return last;
            }
            last = OpTime(t, 1);
            return last;            
        }
        
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
            char buf[64];
            time_t_to_String(secs, buf);
            stringstream ss;
            ss << time_t_to_String_short(secs) << ' ';
            ss << hex << secs << ':' << i;
            return ss.str();
        }
        
        string toStringPretty() const {
            stringstream ss;
            ss << time_t_to_String_short(secs) << ':' << hex << i;
            return ss.str();
        }
        
        string toString() const {
            stringstream ss;
            ss << hex << secs << ':' << i;
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
