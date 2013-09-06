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

#include "mongo/db/index_builder.h"

#include "mongo/db/client.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/repl/rs.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    AtomicUInt IndexBuilder::_indexBuildCount = 0;

    IndexBuilder::IndexBuilder(const std::string ns, const BSONObj index) :
        BackgroundJob(true /* self-delete */), _ns(ns), _index(index.getOwned()),
        _name(str::stream() << "repl index builder " << (_indexBuildCount++).get()) {
    }

    IndexBuilder::~IndexBuilder() {}

    std::string IndexBuilder::name() const {
        return _name;
    }

    void IndexBuilder::run() {
        LOG(2) << "building index " << _index << " on " << _ns << endl;
        Client::initThread(name().c_str());
        replLocalAuth();

        Client::WriteContext ctx(_ns);
        build();

        cc().shutdown();
    }

    void IndexBuilder::build() const {
        theDataFileMgr.insert(_ns.c_str(), _index.objdata(), _index.objsize(),
                              true /* mayInterrupt */);
    }

    std::vector<BSONObj> IndexBuilder::killMatchingIndexBuilds(const BSONObj& criteria) {
        verify(Lock::somethingWriteLocked());
        std::vector<BSONObj> indexes;
        CurOp* op = NULL;
        while ((op = CurOp::getOp(criteria)) != NULL) {
            BSONObj index = op->query();
            killCurrentOp.kill(op->opNum());
            indexes.push_back(index);
        }
        if (indexes.size() > 0) {
            log() << "halted " << indexes.size() << " index build(s)" << endl;
        }
        return indexes;
    }

    void IndexBuilder::restoreIndexes(const std::string& ns, const std::vector<BSONObj>& indexes) {
        log() << "restarting " << indexes.size() << " index build(s)" << endl;
        for (int i = 0; i < static_cast<int>(indexes.size()); i++) {
            IndexBuilder* indexBuilder = new IndexBuilder(ns, indexes[i]);
            // This looks like a memory leak, but indexBuilder deletes itself when it finishes
            indexBuilder->go();
        }
    }
}

