// ramlog.h

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
    public:
        RamLog();

        virtual void write(LogLevel ll, const string& str);

        void get( vector<const char*>& v) const;

        void toHTML(stringstream& s);

        static int repeats(const vector<const char *>& v, int i);
        static string clean(const vector<const char *>& v, int i, string line="");
        static string color(string line);

        /* turn http:... into an anchor */
        static string linkify(const char *s);


    private:
        enum {
            N = 128, // number of links
            C = 256 // max size of line
        };
        char lines[N][C];
        unsigned h; // current position
        unsigned n; // numer of lines stores 0 o N
    };

}
