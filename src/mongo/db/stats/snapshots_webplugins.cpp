/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/db/dbwebserver.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/stats/snapshots.h"
#include "mongo/util/mongoutils/html.h"

namespace mongo {
namespace {

using namespace html;

using std::fixed;
using std::setprecision;
using std::string;
using std::stringstream;

class DBTopStatus : public WebStatusPlugin {
public:
    DBTopStatus() : WebStatusPlugin("dbtop", 50, "(occurrences|percent of elapsed)") {}

    void display(stringstream& ss, double elapsed, const Top::UsageData& usage) {
        ss << "<td>";
        ss << usage.count;
        ss << "</td><td>";
        double per = 100 * ((double)usage.time) / elapsed;
        if (per == (int)per)
            ss << (int)per;
        else {
            const auto precision = ss.precision();
            const auto flags = ss.flags();
            ss << setprecision(1) << fixed << per;
            ss.flags(flags);
            ss.precision(precision);
        }
        ss << '%';
        ss << "</td>";
    }

    void display(stringstream& ss,
                 double elapsed,
                 const string& ns,
                 const Top::CollectionData& data) {
        if (ns != "TOTAL" && data.total.count == 0)
            return;
        ss << "<tr><th>" << html::escape(ns) << "</th>";

        display(ss, elapsed, data.total);

        display(ss, elapsed, data.readLock);
        display(ss, elapsed, data.writeLock);

        display(ss, elapsed, data.queries);
        display(ss, elapsed, data.getmore);
        display(ss, elapsed, data.insert);
        display(ss, elapsed, data.update);
        display(ss, elapsed, data.remove);

        ss << "</tr>\n";
    }

    void run(OperationContext* txn, stringstream& ss) {
        StatusWith<SnapshotDiff> diff = statsSnapshots.computeDelta();

        if (!diff.isOK())
            return;

        ss << "<table border=1 cellpadding=2 cellspacing=0>";
        ss << "<tr align='left'><th>";
        ss << a("http://dochub.mongodb.org/core/whatisanamespace", "namespace")
           << "NS</a></th>"
              "<th colspan=2>total</th>"
              "<th colspan=2>Reads</th>"
              "<th colspan=2>Writes</th>"
              "<th colspan=2>Queries</th>"
              "<th colspan=2>GetMores</th>"
              "<th colspan=2>Inserts</th>"
              "<th colspan=2>Updates</th>"
              "<th colspan=2>Removes</th>";
        ss << "</tr>\n";

        const Top::UsageMap& usage = diff.getValue().usageDiff;
        unsigned long long elapsed = diff.getValue().timeElapsed;
        for (Top::UsageMap::const_iterator i = usage.begin(); i != usage.end(); ++i) {
            display(ss, (double)elapsed, i->first, i->second);
        }

        ss << "</table>";
    }

    virtual void init() {}
} dbtopStatus;

}  // namespace
}  // namespace mongo
