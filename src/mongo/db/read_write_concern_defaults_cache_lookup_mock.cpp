/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"

#include "mongo/util/assert_util.h"

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

ReadWriteConcernDefaults::FetchDefaultsFn ReadWriteConcernDefaultsLookupMock::getFetchDefaultsFn() {
    return [this](OperationContext* opCtx) {
        return lookup(opCtx);
    };
}

boost::optional<RWConcernDefault> ReadWriteConcernDefaultsLookupMock::lookup(
    OperationContext* opCtx) {
    uassert(_status->code(), _status->reason(), !_status);
    return _value;
}

void ReadWriteConcernDefaultsLookupMock::setLookupCallReturnValue(RWConcernDefault&& rwc) {
    _value.emplace(rwc);
}

void ReadWriteConcernDefaultsLookupMock::setLookupCallFailure(boost::optional<Status> status) {
    _status = status;
}

}  // namespace mongo
