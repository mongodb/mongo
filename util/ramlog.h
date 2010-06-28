// log.h

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

#include "log.h"

namespace mongo {

    class RamLog : public Tee {
        enum { 
            N = 128,
            C = 256
        };
        char lines[N][C];
        unsigned h, n;

    public:
        RamLog() { 
            h = 0; n = 0;
            for( int i = 0; i < N; i++ )
                lines[i][C-1] = 0;
        }
        virtual void write(LogLevel ll, const string& str) {
            char *p = lines[(h+n)%N];
            if( str.size() < C )
                strcpy(p, str.c_str());
            else
                memcpy(p, str.c_str(), C-1);
            if( n < N ) n++;
            else h = (h+1) % N;
        }
        vector<const char *> get() const {
            vector<const char *> v;
            for( unsigned x=0, i=h; x++ < n; i=(i+1)%N )
                v.push_back(lines[i]);
            return v;
        }
    };

}
