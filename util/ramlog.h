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
        RamLog( string name );

        virtual void write(LogLevel ll, const string& str);

        void get( vector<const char*>& v) const;

        void toHTML(stringstream& s);

        static RamLog* get( string name );
        static void getNames( vector<string>& names );

        time_t lastWrite() { return _lastWrite; } // 0 if no writes

    protected:
        static int repeats(const vector<const char *>& v, int i);
        static string clean(const vector<const char *>& v, int i, string line="");
        static string color(string line);

        /* turn http:... into an anchor */
        static string linkify(const char *s);

    private:
        ~RamLog(); // want this private as we want to leak so we can use them till the very end

        enum {
            N = 128, // number of links
            C = 256 // max size of line
        };
        char lines[N][C];
        unsigned h; // current position
        unsigned n; // numer of lines stores 0 o N
        string _name;

        typedef map<string,RamLog*> RM;
        static mongo::mutex* _namedLock;
        static RM*  _named;
        time_t _lastWrite;
    };

}
