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
        theDataFileMgr.insert(_ns.c_str(), _index.objdata(), _index.objsize(),
                              true /* mayInterrupt */);

        cc().shutdown();
    }

    std::vector<BSONObj> IndexBuilder::killMatchingIndexBuilds(const BSONObj& criteria) {
        std::vector<BSONObj> indexes;
        CurOp* op = NULL;
        while ((op = CurOp::getOp(criteria)) != NULL) {
            BSONObj index = op->query();
            killCurrentOp.blockingKill(op->opNum());
            indexes.push_back(index);
        }
        return indexes;
    }

    void IndexBuilder::restoreIndexes(const std::string& ns, const std::vector<BSONObj>& indexes) {
        for (int i = 0; i < static_cast<int>(indexes.size()); i++) {
            IndexBuilder* indexBuilder = new IndexBuilder(ns, indexes[i]);
            // This looks like a memory leak, but indexBuilder deletes itself when it finishes
            indexBuilder->go();
        }
    }
}

