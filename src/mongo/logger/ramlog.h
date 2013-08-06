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

#include <sstream>
#include <string>
#include <vector>
#include <boost/version.hpp>

#if BOOST_VERSION >= 105300
#include <boost/thread/lock_guard.hpp>
#endif

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/base/status.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/logger/appender.h"
#include "mongo/logger/message_event.h"
#include "mongo/logger/tee.h"

namespace mongo {

    /**
     * Fixed-capacity log of line-oriented messages.
     *
     * Holds up to RamLog::N lines of up to RamLog::C bytes, each.
     *
     * RamLogs are stored in a global registry, accessed via RamLog::get() and
     * RamLog::getIfExists().
     *
     * RamLogs and their registry are self-synchronizing.  See documentary comments.
     * To read a RamLog, instantiate a RamLog::LineIterator, documented below.
     */
    class RamLog : public logger::Tee {
        MONGO_DISALLOW_COPYING(RamLog);
    public:
        class LineIterator;
        friend class RamLog::LineIterator;

        /**
         * Returns a pointer to the ramlog named "name", creating one if it did not already exist.
         *
         * Synchronizes on the RamLog catalog lock, _namedLock.
         */
        static RamLog* get(const std::string& name);

        /**
         * Returns a pointer to the ramlog named "name", or NULL if no such ramlog exists.
         *
         * Synchronizes on the RamLog catalog lock, _namedLock.
         */
        static RamLog* getIfExists(const std::string& name);

        /**
         * Writes the names of all existing ramlogs into "names".
         *
         * Synchronizes on the RamLog catalog lock, _namedLock.
         */
        static void getNames( std::vector<std::string>& names );

        /**
         * Writes "str" as a line into the RamLog.  If "str" is longer than the maximum
         * line size, RamLog::C, truncates the line to the first C bytes.  If "str"
         * is shorter than RamLog::C and has a terminal '\n', it omits that character.
         *
         * Synchronized on the instance's own mutex, _mutex.
         */
        void write(const std::string& str);


        /**
         * Writes an HTML representation of the log to "s".
         *
         * Synchronized on the instance's own mutex, _mutex.
         */
        void toHTML(std::stringstream& s);

    private:
        static int repeats(const std::vector<const char *>& v, int i);
        static string clean(const std::vector<const char *>& v, int i, string line="");
        static string color(const std::string& line);

        /* turn http:... into an anchor */
        static string linkify(const char *s);

        explicit RamLog( const std::string& name );
        ~RamLog(); // want this private as we want to leak so we can use them till the very end

        enum {
            N = 1024, // number of lines
            C = 512 // max size of line
        };

        const char* getLine_inlock(unsigned lineNumber) const;

        boost::mutex _mutex;  // Guards all non-static data.
        char lines[N][C];
        unsigned h; // current position
        unsigned n; // number of lines stores 0 o N
        string _name;
        long long _totalLinesWritten;

        time_t _lastWrite;
    };

    /**
     * Iterator over the lines of a RamLog.
     *
     * Also acts as a means of inspecting other properites of a ramlog consistently.
     *
     * Instances of LineIterator hold the lock for the underlying RamLog for their whole lifetime,
     * and so should not be kept around.
     */
    class RamLog::LineIterator {
        MONGO_DISALLOW_COPYING(LineIterator);
    public:
        explicit LineIterator(RamLog* ramlog);

        /**
         * Returns true if there are more lines available to return by calls to next().
         */
        bool more() const { return _nextLineIndex < _ramlog->n; }

        /**
         * Returns the next line and advances the iterator.
         */
        const char* next() {
            return _ramlog->getLine_inlock(_nextLineIndex++);  // Postfix increment.
        }

        /**
         * Returns the time of the last write to the ramlog.
         */
        time_t lastWrite();

        /**
         * Returns the total number of lines ever written to the ramlog.
         */
        long long getTotalLinesWritten();

    private:
        const RamLog* _ramlog;
        boost::lock_guard<boost::mutex> _lock;
        unsigned _nextLineIndex;
    };

    /**
     * Appender for appending MessageEvents to a RamLog.
     */
    class RamLogAppender : public logger::Appender<logger::MessageEventEphemeral> {
    public:
        explicit RamLogAppender(RamLog* ramlog);
        virtual ~RamLogAppender();

        virtual Status append(const logger::MessageEventEphemeral& event);

    private:
        RamLog* _ramlog;
    };
}
