/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/db/repl/oplog_interface_local.h"

#include "mongo/db/db_raii.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

namespace {

    class OplogIteratorLocal : public OplogInterface::Iterator {
    public:

        OplogIteratorLocal(std::unique_ptr<OldClientContext> ctx,
                           std::unique_ptr<PlanExecutor> exec);

        StatusWith<Value> next() override;

    private:

        std::unique_ptr<OldClientContext> _ctx;
        std::unique_ptr<PlanExecutor> _exec;

    };

    OplogIteratorLocal::OplogIteratorLocal(std::unique_ptr<OldClientContext> ctx,
                                           std::unique_ptr<PlanExecutor> exec)
        : _ctx(std::move(ctx)),
          _exec(std::move(exec)) { }

    StatusWith<OplogInterface::Iterator::Value> OplogIteratorLocal::next() {
        BSONObj obj;
        RecordId recordId;

        if (PlanExecutor::ADVANCED != _exec->getNext(&obj, &recordId)) {
            return StatusWith<Value>(ErrorCodes::NoSuchKey, "no more operations in local oplog");
        }
        return StatusWith<Value>(std::make_pair(obj, recordId));
    }

} // namespace

    OplogInterfaceLocal::OplogInterfaceLocal(OperationContext* txn,
                                             const std::string& collectionName)
        : _txn(txn),
          _collectionName(collectionName) {

        invariant(txn);
        invariant(!collectionName.empty());
    }

    std::string OplogInterfaceLocal::toString() const {
        return str::stream() <<
            "LocalOplogInterface: "
            "operation context: " << _txn->getNS() << "/" << _txn->getOpID() <<
            "; collection: " << _collectionName;
    }

    std::unique_ptr<OplogInterface::Iterator> OplogInterfaceLocal::makeIterator() const {
        std::unique_ptr<OldClientContext> ctx(new OldClientContext(_txn, _collectionName));
        std::unique_ptr<PlanExecutor> exec(
                InternalPlanner::collectionScan(_txn,
                                                _collectionName,
                                                ctx->db()->getCollection(_collectionName),
                                                InternalPlanner::BACKWARD));
        return std::unique_ptr<OplogInterface::Iterator>(
            new OplogIteratorLocal(std::move(ctx), std::move(exec)));
    }

} // namespace repl
} // namespace mongo
