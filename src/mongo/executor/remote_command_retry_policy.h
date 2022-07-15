/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"
#include <string>

namespace mongo {
namespace executor {
namespace remote_command_runner {

class RemoteCommandRetryPolicy {
public:
    /**
     * Given an error, returns true if the scheduler should retry the remote command.
     */
    virtual bool shouldRetry(Status s) const = 0;

    /**
     * Retry scheduler should wait this long in between retrying remote commands.
     */
    virtual Milliseconds getNextRetryDelay() = 0;

    virtual BSONObj toBSON() const = 0;

    virtual ~RemoteCommandRetryPolicy() = default;
};

class RemoteCommandNoRetryPolicy : public RemoteCommandRetryPolicy {
public:
    bool shouldRetry(Status s) const override final {
        return false;
    }

    Milliseconds getNextRetryDelay() override final {
        return Milliseconds::zero();
    }

    BSONObj toBSON() const override final {
        return BSON("retryPolicyType"
                    << "NoRetryPolicy");
    }
};
}  // namespace remote_command_runner
}  // namespace executor
}  // namespace mongo
