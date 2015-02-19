/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#include <sstream>

#include "mongo/platform/basic.h"

#include "mongo/db/storage/rocks/rocks_server_status.h"
#include "mongo/db/storage/rocks/rocks_recovery_unit.h"

#include "boost/scoped_ptr.hpp"

#include <rocksdb/db.h>

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/storage/rocks/rocks_engine.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
    using std::string;

    namespace {
        std::string PrettyPrintBytes(size_t bytes) {
            if (bytes < (16 << 10)) {
                return std::to_string(bytes) + "B";
            } else if (bytes < (16 << 20)) {
                return std::to_string(bytes >> 10) + "KB";
            } else if (bytes < (16LL << 30)) {
                return std::to_string(bytes >> 20) + "MB";
            } else {
                return std::to_string(bytes >> 30) + "GB";
            }
        }
    }  // namespace

    RocksServerStatusSection::RocksServerStatusSection(RocksEngine* engine)
        : ServerStatusSection("RocksDB"), _engine(engine) {}

    bool RocksServerStatusSection::includeByDefault() const { return true; }

    BSONObj RocksServerStatusSection::generateSection(OperationContext* txn,
                                                      const BSONElement& configElement) const {

        BSONObjBuilder bob;

        // if the second is true, that means that we pass the value through PrettyPrintBytes
        std::vector<std::pair<std::string, bool>> properties = {
            {"stats", false},
            {"num-immutable-mem-table", false},
            {"mem-table-flush-pending", false},
            {"compaction-pending", false},
            {"background-errors", false},
            {"cur-size-active-mem-table", true},
            {"cur-size-all-mem-tables", true},
            {"num-entries-active-mem-table", false},
            {"num-entries-imm-mem-tables", false},
            {"estimate-table-readers-mem", true},
            {"num-snapshots", false},
            {"oldest-snapshot-time", false},
            {"num-live-versions", false}};
        for (auto const& property : properties) {
            std::string statsString;
            if (!_engine->getDB()->GetProperty("rocksdb." + property.first, &statsString)) {
                statsString = "<error> unable to retrieve statistics";
            }
            if (property.first == "stats") {
                // special casing because we want to turn it into array
                BSONArrayBuilder a;
                std::stringstream ss(statsString);
                std::string line;
                while (std::getline(ss, line)) {
                    a.append(line);
                }
                bob.appendArray(property.first, a.arr());
            } else if (property.second) {
                bob.append(property.first, PrettyPrintBytes(std::stoll(statsString)));
            } else {
                bob.append(property.first, statsString);
            }
        }
        bob.append("total-live-recovery-units", RocksRecoveryUnit::getTotalLiveRecoveryUnits());
        bob.append("block-cache-usage", PrettyPrintBytes(_engine->getBlockCacheUsage()));

        return bob.obj();
    }

}  // namespace mongo

