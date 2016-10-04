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

#include "mongo/platform/basic.h"

#include "mongo/util/concurrency/thread_name.h"

#include <boost/thread/tss.hpp>

#include "mongo/base/init.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;

namespace {

boost::thread_specific_ptr<std::string> threadName;
AtomicInt64 nextUnnamedThreadId{1};

// It is unsafe to access threadName before its dynamic initialization has completed. Use
// the execution of mongo initializers (which only happens once we have entered main, and
// therefore after dynamic initialization is complete) to signal that it is safe to use
// 'threadName'.
bool mongoInitializersHaveRun{};
MONGO_INITIALIZER(ThreadNameInitializer)(InitializerContext*) {
    mongoInitializersHaveRun = true;
    // The global initializers should only ever be run from main, so setting thread name
    // here makes sense.
    setThreadName("main");
    return Status::OK();
}

}  // namespace

void setThreadName(StringData name) {
    invariant(mongoInitializersHaveRun);
    threadName.reset(new string(name.toString()));
}

const string& getThreadName() {
    if (MONGO_unlikely(!mongoInitializersHaveRun)) {
        // 'getThreadName' has been called before dynamic initialization for this
        // translation unit has completed, so return a fallback value rather than accessing
        // the 'threadName' variable, which requires dynamic initialization. We assume that
        // we are in the 'main' thread.
        static const std::string kFallback = "main";
        return kFallback;
    }

    std::string* s;
    while (!(s = threadName.get())) {
        setThreadName(std::string(str::stream() << "thread" << nextUnnamedThreadId.fetchAndAdd(1)));
    }
    return *s;
}

}  // namespace mongo
