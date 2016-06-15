/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/move_timing_helper.h"

#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

MoveTimingHelper::MoveTimingHelper(OperationContext* txn,
                                   const std::string& where,
                                   const std::string& ns,
                                   const BSONObj& min,
                                   const BSONObj& max,
                                   int totalNumSteps,
                                   std::string* cmdErrmsg,
                                   const ShardId& toShard,
                                   const ShardId& fromShard)
    : _txn(txn),
      _where(where),
      _ns(ns),
      _to(toShard),
      _from(fromShard),
      _totalNumSteps(totalNumSteps),
      _cmdErrmsg(cmdErrmsg),
      _nextStep(0) {
    _b.append("min", min);
    _b.append("max", max);
}

MoveTimingHelper::~MoveTimingHelper() {
    // even if logChange doesn't throw, bson does
    // sigh
    try {
        if (_to.isValid()) {
            _b.append("to", _to.toString());
        }

        if (_from.isValid()) {
            _b.append("from", _from.toString());
        }

        if (_nextStep != _totalNumSteps) {
            _b.append("note", "aborted");
        } else {
            _b.append("note", "success");
        }

        if (!_cmdErrmsg->empty()) {
            _b.append("errmsg", *_cmdErrmsg);
        }

        grid.catalogClient(_txn)->logChange(
            _txn, str::stream() << "moveChunk." << _where, _ns, _b.obj());
    } catch (const std::exception& e) {
        warning() << "couldn't record timing for moveChunk '" << _where << "': " << e.what();
    }
}

void MoveTimingHelper::done(int step) {
    invariant(step == ++_nextStep);
    invariant(step <= _totalNumSteps);

    const std::string s = str::stream() << "step " << step << " of " << _totalNumSteps;

    CurOp* op = CurOp::get(_txn);

    {
        stdx::lock_guard<Client> lk(*_txn->getClient());
        op->setMessage_inlock(s.c_str());
    }

    _b.appendNumber(s, _t.millis());
    _t.reset();
}

}  // namespace mongo
