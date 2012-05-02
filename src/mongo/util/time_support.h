// @file time_support.h

/*    Copyright 2010 10gen Inc.
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

#include "mongo/platform/basic.h"

#include <cstdio> // sscanf
#include <ctime>
#include <string>
#include <iostream>
#include <boost/thread/thread.hpp>
#include <boost/thread/tss.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/thread/xtime.hpp>
#include "../bson/util/misc.h"  // Date_t

namespace mongo {

    using std::string;

    inline void time_t_to_Struct(time_t t, struct tm * buf , bool local = false ) {
#if defined(_WIN32)
        if ( local )
            localtime_s( buf , &t );
        else
            gmtime_s(buf, &t);
#else
        if ( local )
            localtime_r(&t, buf);
        else
            gmtime_r(&t, buf);
#endif
    }

    // uses ISO 8601 dates without trailing Z
    // colonsOk should be false when creating filenames
    inline string terseCurrentTime(bool colonsOk=true) {
        struct tm t;
        time_t_to_Struct( time(0) , &t );

        const char* fmt = (colonsOk ? "%Y-%m-%dT%H:%M:%S" : "%Y-%m-%dT%H-%M-%S");
        char buf[32];
        verify(strftime(buf, sizeof(buf), fmt, &t) == 19);
        return buf;
    }

    inline string timeToISOString(time_t time) {
        struct tm t;
        time_t_to_Struct( time, &t );

        const char* fmt = "%Y-%m-%dT%H:%M:%SZ";
        char buf[32];
        verify(strftime(buf, sizeof(buf), fmt, &t) == 20);
        return buf;
    }

    inline boost::gregorian::date currentDate() {
        boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
        return now.date();
    }

    // parses time of day in "hh:mm" format assuming 'hh' is 00-23
    inline bool toPointInTime( const string& str , boost::posix_time::ptime* timeOfDay ) {
        int hh = 0;
        int mm = 0;
        if ( 2 != sscanf( str.c_str() , "%d:%d" , &hh , &mm ) ) {
            return false;
        }

        // verify that time is well formed
        if ( ( hh / 24 ) || ( mm / 60 ) ) {
            return false;
        }

        boost::posix_time::ptime res( currentDate() , boost::posix_time::hours( hh ) + boost::posix_time::minutes( mm ) );
        *timeOfDay = res;
        return true;
    }

#if defined(_WIN32)
    inline void sleepsecs(int s) {
        // todo : add an assert here that we are not locked in d.dbMutex.  there may be debugging things where we 
        //        are but otherwise it's quite likely that would be a mistake.
        Sleep(s*1000);
    }
    inline void sleepmillis(long long s) {
        verify( s <= 0xffffffff );
        Sleep((DWORD) s);
    }
    inline void sleepmicros(long long s) {
        if ( s <= 0 )
            return;
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        xt.sec += (int)( s / 1000000 );
        xt.nsec += (int)(( s % 1000000 ) * 1000);
        if ( xt.nsec >= 1000000000 ) {
            xt.nsec -= 1000000000;
            xt.sec++;
        }
        boost::thread::sleep(xt);
    }
#elif defined(__sunos__)
    inline void sleepsecs(int s) {
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        xt.sec += s;
        boost::thread::sleep(xt);
    }
    inline void sleepmillis(long long s) {
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        xt.sec += (int)( s / 1000 );
        xt.nsec += (int)(( s % 1000 ) * 1000000);
        if ( xt.nsec >= 1000000000 ) {
            xt.nsec -= 1000000000;
            xt.sec++;
        }
        boost::thread::sleep(xt);
    }
    inline void sleepmicros(long long s) {
        if ( s <= 0 )
            return;
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        xt.sec += (int)( s / 1000000 );
        xt.nsec += (int)(( s % 1000000 ) * 1000);
        if ( xt.nsec >= 1000000000 ) {
            xt.nsec -= 1000000000;
            xt.sec++;
        }
        boost::thread::sleep(xt);
    }
#else
    inline void sleepsecs(int s) {
        struct timespec t;
        t.tv_sec = s;
        t.tv_nsec = 0;
        if ( nanosleep( &t , 0 ) ) {
            std::cout << "nanosleep failed" << std::endl;
        }
    }
    inline void sleepmicros(long long s) {
        if ( s <= 0 )
            return;
        struct timespec t;
        t.tv_sec = (int)(s / 1000000);
        t.tv_nsec = 1000 * ( s % 1000000 );
        struct timespec out;
        if ( nanosleep( &t , &out ) ) {
            std::cout << "nanosleep failed" << std::endl;
        }
    }
    inline void sleepmillis(long long s) {
        sleepmicros( s * 1000 );
    }
#endif

    extern long long jsTime_virtual_skew;
    extern boost::thread_specific_ptr<long long> jsTime_virtual_thread_skew;

    // DO NOT TOUCH except for testing
    inline void jsTimeVirtualSkew( long long skew ){
        jsTime_virtual_skew = skew;
    }
    inline long long getJSTimeVirtualSkew(){
        return jsTime_virtual_skew;
    }

    inline void jsTimeVirtualThreadSkew( long long skew ){
        jsTime_virtual_thread_skew.reset(new long long(skew));
    }
    inline long long getJSTimeVirtualThreadSkew(){
        if(jsTime_virtual_thread_skew.get()){
            return *(jsTime_virtual_thread_skew.get());
        }
        else return 0;
    }

    /** Date_t is milliseconds since epoch */
    inline Date_t jsTime();

    /** warning this will wrap */
    inline unsigned curTimeMicros();

    inline unsigned long long curTimeMicros64();
#ifdef _WIN32 // no gettimeofday on windows
    inline unsigned long long curTimeMillis64() {
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        return ((unsigned long long)xt.sec) * 1000 + xt.nsec / 1000000;
    }
    inline Date_t jsTime() {
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        unsigned long long t = xt.nsec / 1000000;
        return ((unsigned long long) xt.sec * 1000) + t + getJSTimeVirtualSkew() + getJSTimeVirtualThreadSkew();
    }
    inline unsigned long long curTimeMicros64() {
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        unsigned long long t = xt.nsec / 1000;
        return (((unsigned long long) xt.sec) * 1000000) + t;
    }    
    inline unsigned curTimeMicros() {
        boost::xtime xt;
        boost::xtime_get(&xt, boost::TIME_UTC);
        unsigned t = xt.nsec / 1000;
        unsigned secs = xt.sec % 1024;
        return secs*1000000 + t;
    }
#else
#  include <sys/time.h>
    inline unsigned long long curTimeMillis64() {
        timeval tv;
        gettimeofday(&tv, NULL);
        return ((unsigned long long)tv.tv_sec) * 1000 + tv.tv_usec / 1000;
    }
    inline Date_t jsTime() {
        timeval tv;
        gettimeofday(&tv, NULL);
        unsigned long long t = tv.tv_usec / 1000;
        return ((unsigned long long) tv.tv_sec * 1000) + t + getJSTimeVirtualSkew() + getJSTimeVirtualThreadSkew();
    }
    inline unsigned long long curTimeMicros64() {
        timeval tv;
        gettimeofday(&tv, NULL);
        return (((unsigned long long) tv.tv_sec) * 1000*1000) + tv.tv_usec;
    }
    inline unsigned curTimeMicros() {
        timeval tv;
        gettimeofday(&tv, NULL);
        unsigned secs = tv.tv_sec % 1024;
        return secs*1000*1000 + tv.tv_usec;
    }
#endif


    // these are so that if you use one of them compilation will fail
    char *asctime(const struct tm *tm);
    char *ctime(const time_t *timep);
    struct tm *gmtime(const time_t *timep);
    struct tm *localtime(const time_t *timep);

}  // namespace mongo
