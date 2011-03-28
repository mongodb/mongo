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

    /**
     *  simple scoped timer
     */
    class Timer {
    public:
        Timer() { reset(); }
        Timer( unsigned long long start ) { old = start; }
        int seconds() const { return (int)(micros() / 1000000); }
        int millis() const { return (long)(micros() / 1000); }
        unsigned long long micros() const {
            unsigned long long n = curTimeMicros64();
            return n - old;
        }
        unsigned long long micros(unsigned long long & n) const { // returns cur time in addition to timer result
            n = curTimeMicros64();
            return n - old;
        }
        unsigned long long startTime() { return old; }
        void reset() { old = curTimeMicros64(); }
    private:
        unsigned long long old;
    };

#if 1
    class DevTimer {
    public:
        class scoped { 
        public:
            scoped(DevTimer& dt) { }
            ~scoped() { }
        };
        DevTimer(string) { }
        ~DevTimer() { }
    };
#elif defined(_WIN32)
    class DevTimer {
        const string _name;
    public:
        unsigned long long _ticks;
        class scoped { 
            DevTimer& _dt;
            unsigned long long _start;
        public:
            scoped(DevTimer& dt) : _dt(dt) { 
                LARGE_INTEGER i;
                QueryPerformanceCounter(&i);
                _start = i.QuadPart;
            }
            ~scoped() { 
                LARGE_INTEGER i;
                QueryPerformanceCounter(&i);
                _dt._ticks += (i.QuadPart - _start);
            }
        };
        DevTimer(string name) : _name(name), _ticks(0) { 
        }
        ~DevTimer() {
            LARGE_INTEGER freq;
            assert( QueryPerformanceFrequency(&freq) );
            cout << "devtimer\t" << _name << '\t' << _ticks*1000.0/freq.QuadPart << "ms" << endl;
        }
    };
#endif

}  // namespace mongo
