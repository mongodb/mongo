/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/sharding_environment/shard_id.h"
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
