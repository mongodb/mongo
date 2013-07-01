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

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/base/status.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/logger/appender.h"
#include "mongo/logger/message_event.h"
#include "mongo/logger/tee.h"

namespace mongo {

    class RamLog : public logger::Tee {
        MONGO_DISALLOW_COPYING(RamLog);
    public:
        RamLog( const std::string& name );

        void write(const std::string& str);

        void get( std::vector<const char*>& v);

        void toHTML(std::stringstream& s);

        static RamLog* get( const std::string& name );
        static void getNames( std::vector<std::string>& names );

        time_t lastWrite();
        long long getTotalLinesWritten();

    private:
        static int repeats(const std::vector<const char *>& v, int i);
        static string clean(const std::vector<const char *>& v, int i, string line="");
        static string color(const std::string& line);

        /* turn http:... into an anchor */
        static string linkify(const char *s);

        ~RamLog(); // want this private as we want to leak so we can use them till the very end

        enum {
            N = 1024, // number of lines
            C = 512 // max size of line
        };

        boost::mutex _mutex;  // Guards all non-static data.
        char lines[N][C];
        unsigned h; // current position
        unsigned n; // number of lines stores 0 o N
        string _name;
        long long _totalLinesWritten;

        typedef std::map<string,RamLog*> RM;
        static mongo::mutex* _namedLock;
        static RM*  _named;
        time_t _lastWrite;
    };

    class RamLogAppender : public logger::Appender<logger::MessageEventEphemeral> {
    public:
        explicit RamLogAppender(RamLog* ramlog);
        virtual ~RamLogAppender();

        virtual Status append(const logger::MessageEventEphemeral& event);

    private:
        RamLog* _ramlog;
    };
}
