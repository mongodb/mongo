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

#include <functional>

#include "mongo/transport/service_executor.h"
#include "mongo/util/out_of_line_executor.h"

namespace mongo::transport {

class MockServiceExecutor : public ServiceExecutor {
public:
    Status start() override {
        return startCb();
    }

    Status scheduleTask(Task task, ScheduleFlags flags) override {
        return scheduleTaskCb(std::move(task), std::move(flags));
    }

    void runOnDataAvailable(const SessionHandle& session,
                            OutOfLineExecutor::Task onCompletionCallback) override {
        runOnDataAvailableCb(session, std::move(onCompletionCallback));
    }

    Status shutdown(Milliseconds timeout) override {
        return shutdownCb(std::move(timeout));
    }

    size_t getRunningThreads() const override {
        return getRunningThreadsCb();
    }

    void appendStats(BSONObjBuilder* bob) const override {
        appendStatsCb(bob);
    }

    std::function<Status()> startCb;
    std::function<Status(Task, ScheduleFlags)> scheduleTaskCb;
    std::function<void(const SessionHandle&, OutOfLineExecutor::Task)> runOnDataAvailableCb;
    std::function<Status(Milliseconds)> shutdownCb;
    std::function<size_t()> getRunningThreadsCb;
    std::function<void(BSONObjBuilder*)> appendStatsCb;
};

}  // namespace mongo::transport
