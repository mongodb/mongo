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

#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/log.h"

namespace mongo {

    class RamLog : public Tee {
    public:
        RamLog( const std::string& name );

        virtual void write(LogLevel ll, const string& str);

        void get( vector<const char*>& v) const;

        void toHTML(stringstream& s);

        static RamLog* get( const std::string& name );
        static void getNames( vector<string>& names );

        time_t lastWrite() { return _lastWrite; } // 0 if no writes
        long long getTotalLinesWritten() const { return _totalLinesWritten; }

    protected:
        static int repeats(const vector<const char *>& v, int i);
        static string clean(const vector<const char *>& v, int i, string line="");
        static string color(const std::string& line);

        /* turn http:... into an anchor */
        static string linkify(const char *s);

    private:
        ~RamLog(); // want this private as we want to leak so we can use them till the very end

        enum {
            N = 1024, // number of lines
            C = 512 // max size of line
        };
        char lines[N][C];
        unsigned h; // current position
        unsigned n; // number of lines stores 0 o N
        string _name;
        long long _totalLinesWritten;

        typedef map<string,RamLog*> RM;
        static mongo::mutex* _namedLock;
        static RM*  _named;
        time_t _lastWrite;
    };

}
