// @file timer.h

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

#include "time_support.h"

namespace mongo {

#if !defined(_WIN32)

    /**
     *  simple scoped timer
     */
    class Timer /*copyable*/ {
    public:
        Timer() { reset(); }
        int seconds() const { return (int)(micros() / 1000000); }
        int millis() const { return (int)(micros() / 1000); }
        int minutes() const { return seconds() / 60; }
        

        /** gets time interval and resets at the same time.  this way we can call curTimeMicros
              once instead of twice if one wanted millis() and then reset().
            @return time in millis
        */
        int millisReset() { 
            unsigned long long now = curTimeMicros64();
            int m = (int)((now-old)/1000);
            old = now;
            return m;
        }

        // note: dubious that the resolution is as anywhere near as high as ethod name implies!
        unsigned long long micros() const {
            unsigned long long n = curTimeMicros64();
            return n - old;
        }
        unsigned long long micros(unsigned long long & n) const { // returns cur time in addition to timer result
            n = curTimeMicros64();
            return n - old;
        }

        void reset() { old = curTimeMicros64(); }
    private:
        unsigned long long old;
    };

#else

    class Timer /*copyable*/ {
    public:
        Timer() { reset(); }

        int seconds() const { 
            int s = static_cast<int>((now() - old) / countsPerSecond);
            return s;
        }

        int millis() const { 
            return (int)
                    ((now() - old) * 1000.0 / countsPerSecond);
        }

        int minutes() const { return seconds() / 60; }
        
        /** gets time interval and resets at the same time.  this way we can call curTimeMicros
              once instead of twice if one wanted millis() and then reset().
            @return time in millis
        */
        int millisReset() { 
            unsigned long long nw = now();
            int m = static_cast<int>((nw - old) * 1000.0 / countsPerSecond);
            old = nw;
            return m;
       } 

        void reset() { 
            old = now();
        }            

        unsigned long long micros() const {
            return (unsigned long long)
                    ((now() - old) * 1000000.0 / countsPerSecond);
        }

        static unsigned long long countsPerSecond;

    private:
        unsigned long long now() const {
            LARGE_INTEGER i;
            QueryPerformanceCounter(&i);
            return i.QuadPart;
        }

        unsigned long long old;
    };

#endif

}  // namespace mongo
