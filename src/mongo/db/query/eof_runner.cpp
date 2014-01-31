/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/query/eof_runner.h"

#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/type_explain.h"

namespace mongo {

    EOFRunner::EOFRunner(CanonicalQuery* cq, const string& ns) : _cq(cq), _ns(ns) {
    }

    EOFRunner::~EOFRunner() {
    }

    Runner::RunnerState EOFRunner::getNext(BSONObj* objOut, DiskLoc* dlOut) {
        return Runner::RUNNER_EOF;
    }

    bool EOFRunner::isEOF() {
        return true;
    }

    void EOFRunner::saveState() {
    }

    bool EOFRunner::restoreState() {
        // TODO: Does this value matter?
        return false;
    }

    void EOFRunner::setYieldPolicy(Runner::YieldPolicy policy) {
    }

    void EOFRunner::invalidate(const DiskLoc& dl, InvalidationType type) {
    }

    const std::string& EOFRunner::ns() {
        return _ns;
    }

    void EOFRunner::kill() {
    }

    Status EOFRunner::getInfo(TypeExplain** explain,
                              PlanInfo** planInfo) const {
        if (NULL != explain) {
            *explain = new TypeExplain;

            // Fill in mandatory fields.
            (*explain)->setN(0);
            (*explain)->setNScannedObjects(0);
            (*explain)->setNScanned(0);

            // Fill in all the main fields that don't have a default in the explain data structure.
            (*explain)->setCursor("BasicCursor");
            (*explain)->setScanAndOrder(false);
            (*explain)->setIsMultiKey(false);
            (*explain)->setIndexOnly(false);
            (*explain)->setNYields(0);
            (*explain)->setNChunkSkips(0);

            TypeExplain* allPlans = new TypeExplain;
            allPlans->setCursor("BasicCursor");
            (*explain)->addToAllPlans(allPlans); // ownership xfer

            (*explain)->setNScannedObjectsAllPlans(0);
            (*explain)->setNScannedAllPlans(0);
        }
        else if (NULL != planInfo) {
            *planInfo = new PlanInfo();
            (*planInfo)->planSummary = "EOF";
        }

        return Status::OK();
    }

} // namespace mongo
