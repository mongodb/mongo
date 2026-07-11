// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/s/move_timing_helper.h"

#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <exception>
#include <mutex>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

MoveTimingHelper::MoveTimingHelper(OperationContext* opCtx,
                                   const std::string& where,
                                   const NamespaceString& ns,
                                   const boost::optional<BSONObj>& min,
                                   const boost::optional<BSONObj>& max,
                                   int totalNumSteps,
                                   const ShardId& toShard,
                                   const ShardId& fromShard)
    : _opCtx(opCtx),
      _where(where),
      _ns(ns),
      _to(toShard),
      _from(fromShard),
      _min(min),
      _max(max),
      _totalNumSteps(totalNumSteps),
      _nextStep(0) {}

MoveTimingHelper::~MoveTimingHelper() {
    // even if logChange doesn't throw, bson does
    // sigh
    try {
        _b.append("min", _min.get_value_or(BSONObj()));
        _b.append("max", _max.get_value_or(BSONObj()));

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

        if (!_cmdErrmsg.empty()) {
            _b.append("errmsg", _cmdErrmsg);
        }

        ShardingLogging::get(_opCtx)->logChange(_opCtx,
                                                str::stream() << "moveChunk." << _where,
                                                _ns,
                                                _b.obj(),
                                                defaultMajorityWriteConcernDoNotUse());
    } catch (const std::exception& e) {
        LOGV2_WARNING(23759,
                      "couldn't record timing for moveChunk '{where}': {e_what}",
                      "where"_attr = _where,
                      "e_what"_attr = redact(e.what()));
    }
}

void MoveTimingHelper::done(int step) {
    invariant(step == ++_nextStep);
    invariant(step <= _totalNumSteps);

    const std::string s = str::stream() << "step " << step << " of " << _totalNumSteps;

    CurOp* op = CurOp::get(_opCtx);

    {
        std::lock_guard<Client> lk(*_opCtx->getClient());
        op->setMessage(lk, s.c_str());
    }

    _b.appendNumber(s, _t.millis());
    _t.reset();
}

}  // namespace mongo
