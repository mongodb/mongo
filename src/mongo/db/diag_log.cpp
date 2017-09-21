/**
 *    Copyright (C) 2008-2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault;

#include "mongo/platform/basic.h"

#include <fstream>
#include <iomanip>
#include <sstream>

#include "mongo/db/diag_log.h"

#include "mongo/db/storage/storage_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

using std::hex;
using std::ios;
using std::ofstream;
using std::string;
using std::stringstream;

DiagLog::DiagLog() : f(0), level(0) {}

void DiagLog::openFile() {
    verify(f == 0);
    stringstream ss;
    ss << storageGlobalParams.dbpath << "/diaglog." << hex << time(0);
    string name = ss.str();
    f = new ofstream(name.c_str(), ios::out | ios::binary);
    if (!f->good()) {
        str::stream msg;
        msg << "diagLogging couldn't open " << name;
        log() << msg.ss.str();
        uasserted(ErrorCodes::FileStreamFailed, msg.ss.str());
    } else {
        log() << "diagLogging using file " << name;
    }
}

int DiagLog::setLevel(int newLevel) {
    stdx::lock_guard<stdx::mutex> lk(mutex);
    int old = level;
    log() << "diagLogging level=" << newLevel;
    if (f == 0) {
        openFile();
    }
    level = newLevel;  // must be done AFTER f is set
    return old;
}

void DiagLog::flush() {
    if (level) {
        log() << "flushing diag log";
        stdx::lock_guard<stdx::mutex> lk(mutex);
        f->flush();
    }
}

void DiagLog::writeop(char* data, int len) {
    if (level & 1) {
        stdx::lock_guard<stdx::mutex> lk(mutex);
        f->write(data, len);
    }
}

void DiagLog::readop(char* data, int len) {
    if (level & 2) {
        bool log = (level & 4) == 0;
        OCCASIONALLY log = true;
        if (log) {
            stdx::lock_guard<stdx::mutex> lk(mutex);
            verify(f);
            f->write(data, len);
        }
    }
}

DiagLog _diaglog;

}  // namespace mongo
