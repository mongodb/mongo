/**
 *    Copyright (C) 2014 MongoDB, Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include <boost/thread.hpp>

#include "mongo/db/global_timestamp.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/log.h"

namespace {
    mongo::mutex globalTimestampMutex;
    mongo::Timestamp globalTimestamp(0, 0);

    bool skewed(const mongo::Timestamp& val) {
        if (val.getInc() & 0x80000000) {
            mongo::warning() << "clock skew detected  prev: " << val.getSecs()
                             << " now: " << (unsigned) time(0) << std::endl;
            return true;
        }

        return false;
    }
}

namespace mongo {
    void setGlobalTimestamp(const Timestamp& newTime) {
        boost::lock_guard<boost::mutex> lk(globalTimestampMutex);
        globalTimestamp = newTime;
    }

    Timestamp getLastSetTimestamp() {
        boost::lock_guard<boost::mutex> lk(globalTimestampMutex);
        return globalTimestamp;
    }

    Timestamp getNextGlobalTimestamp() {
        boost::lock_guard<boost::mutex> lk(globalTimestampMutex);

        const unsigned now = (unsigned) time(0);
        const unsigned globalSecs = globalTimestamp.getSecs();
        if ( globalSecs == now ) {
            globalTimestamp = Timestamp(globalSecs, globalTimestamp.getInc() + 1);
        }
        else if ( now < globalSecs ) {
            globalTimestamp = Timestamp(globalSecs, globalTimestamp.getInc() + 1);
            // separate function to keep out of the hot code path
            fassert(17449, !skewed(globalTimestamp));
        }
        else {
            globalTimestamp = Timestamp(now, 1);
        }

        return globalTimestamp;
    }
}
