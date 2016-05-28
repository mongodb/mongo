// ramlog.h

/*    Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <boost/version.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/logger/appender.h"
#include "mongo/logger/message_event.h"
#include "mongo/logger/tee.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/mutex.h"

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
    static void getNames(std::vector<std::string>& names);

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
    static int repeats(const std::vector<const char*>& v, int i);
    static std::string clean(const std::vector<const char*>& v, int i, std::string line = "");
    static std::string color(const std::string& line);

    /* turn http:... into an anchor */
    static std::string linkify(const char* s);

    explicit RamLog(const std::string& name);
    ~RamLog();  // want this private as we want to leak so we can use them till the very end

    enum {
        N = 1024,  // number of lines
        C = 512    // max size of line
    };

    const char* getLine_inlock(unsigned lineNumber) const;

    stdx::mutex _mutex;  // Guards all non-static data.
    char lines[N][C];
    unsigned h;  // current position
    unsigned n;  // number of lines stores 0 o N
    std::string _name;
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
    bool more() const {
        return _nextLineIndex < _ramlog->n;
    }

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
    stdx::lock_guard<stdx::mutex> _lock;
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
