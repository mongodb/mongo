/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/platform/basic.h"

#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/operations.hpp>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/storage/mmap_v1/aligned_builder.h"
#include "mongo/db/storage/mmap_v1/logfile.h"
#include "mongo/db/storage/paths.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/background.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::max;
using std::min;
using std::string;
using std::stringstream;

namespace dur {
boost::filesystem::path getJournalDir();
}

// Testing-only, enabled via command line
class JournalLatencyTestCmd : public Command {
public:
    JournalLatencyTestCmd() : Command("journalLatencyTest") {}

    virtual bool slaveOk() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool adminOnly() const {
        return true;
    }
    virtual void help(stringstream& h) const {
        h << "test how long to write and fsync to a test file in the journal/ directory";
    }
    // No auth needed because it only works when enabled via command line.
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}
    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        boost::filesystem::path p = dur::getJournalDir();
        p /= "journalLatencyTest";

        // remove file if already present
        try {
            boost::filesystem::remove(p);
        } catch (...) {
        }

        BSONObjBuilder bb[2];
        for (int pass = 0; pass < 2; pass++) {
            LogFile f(p.string());
            AlignedBuilder b(1024 * 1024);
            {
                Timer t;
                for (int i = 0; i < 100; i++) {
                    f.synchronousAppend(b.buf(), 8192);
                }
                bb[pass].append("8KB", t.millis() / 100.0);
            }
            {
                const int N = 50;
                Timer t2;
                long long x = 0;
                for (int i = 0; i < N; i++) {
                    Timer t;
                    f.synchronousAppend(b.buf(), 8192);
                    x += t.micros();
                    sleepmillis(4);
                }
                long long y = t2.micros() - 4 * N * 1000;
                // not really trusting the timer granularity on all platforms so whichever is higher
                // of x and y
                bb[pass].append("8KBWithPauses", max(x, y) / (N * 1000.0));
            }
            {
                Timer t;
                for (int i = 0; i < 20; i++) {
                    f.synchronousAppend(b.buf(), 1024 * 1024);
                }
                bb[pass].append("1MB", t.millis() / 20.0);
            }
            // second time around, we are prealloced.
        }
        result.append("timeMillis", bb[0].obj());
        result.append("timeMillisWithPrealloc", bb[1].obj());

        try {
            remove(p);
        } catch (...) {
        }

        try {
            result.append(
                "onSamePartition",
                onSamePartition(dur::getJournalDir().string(), storageGlobalParams.dbpath));
        } catch (...) {
        }

        return 1;
    }
};
MONGO_INITIALIZER(RegisterJournalLatencyTestCmd)(InitializerContext* context) {
    if (Command::testCommandsEnabled) {
        // Leaked intentionally: a Command registers itself when constructed.
        new JournalLatencyTestCmd();
    }
    return Status::OK();
}
}
