// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"
#include "mongo/util/timer.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class BSONObj;
class OperationContext;

class MoveTimingHelper {
public:
    MoveTimingHelper(OperationContext* opCtx,
                     const std::string& where,
                     const NamespaceString& ns,
                     const boost::optional<BSONObj>& min,
                     const boost::optional<BSONObj>& max,
                     int totalNumSteps,
                     const ShardId& toShard,
                     const ShardId& fromShard);
    ~MoveTimingHelper();

    void setMin(const BSONObj& min) {
        _min.emplace(min);
    }

    void setMax(const BSONObj& max) {
        _max.emplace(max);
    }

    void setCmdErrMsg(std::string cmdErrMsg) {
        _cmdErrmsg = std::move(cmdErrMsg);
    }

    void done(int step);

private:
    // Measures how long the receiving of a chunk takes
    Timer _t;

    OperationContext* const _opCtx;
    const std::string _where;
    const NamespaceString _ns;
    const ShardId _to;
    const ShardId _from;

    boost::optional<BSONObj> _min, _max;
    const int _totalNumSteps;
    std::string _cmdErrmsg;

    int _nextStep;
    BSONObjBuilder _b;
};

}  // namespace mongo
