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

#include <boost/optional/optional.hpp>
#include <cstddef>
#include <iosfwd>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/client.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

namespace mongo {
/**
 * DiagnosticInfo keeps track of diagnostic information such as a developer provided
 * name, the time when a lock was first acquired, and a partial caller call stack.
 */
class DiagnosticInfo {
public:
    // Maximum number of stack frames to appear in a DiagnosticInfo::Backtrace.
    static constexpr size_t kMaxBackTraceFrames = 100ull;
    struct Backtrace {
        std::vector<void*> data = std::vector<void*>(kMaxBackTraceFrames, nullptr);
    };

    /**
     * A simple RAII guard to attempt to join a blocked op once it is no longer needed
     *
     * This type is used in tests in conjunction with maybeMakeBlockedOpForTest
     */
    class BlockedOpGuard {
    public:
        ~BlockedOpGuard();
    };

    static boost::optional<DiagnosticInfo> get(Client& client);

    virtual ~DiagnosticInfo() = default;

    Date_t getTimestamp() const {
        return _timestamp;
    }

    StringData getCaptureName() const {
        return _captureName;
    }

    std::string toString() const;

    /**
     * Simple options struct to go with takeDiagnosticInfo
     */
    struct Options {
        Options() : shouldTakeBacktrace{false} {}

        bool shouldTakeBacktrace;
    };

    /**
     * Captures the diagnostic information based on the caller's context.
     */
    static const DiagnosticInfo& capture(Client* client,
                                         StringData captureName,
                                         Options options = Options{}) noexcept;

    /**
     * This function checks the FailPoint currentOpSpawnsThreadWaitingForLatch and potentially
     * launches a blocked operation to populate waitingForLatch for $currentOp.
     */
    static std::unique_ptr<BlockedOpGuard> maybeMakeBlockedOpForTest(Client* client);

    friend std::ostream& operator<<(std::ostream& s, const DiagnosticInfo& info) {
        return s << info.toString();
    }

private:
    friend bool operator==(const DiagnosticInfo& info1, const DiagnosticInfo& info2);
    friend bool operator!=(const DiagnosticInfo& info1, const DiagnosticInfo& info2) {
        return !(info1 == info2);
    }
    friend std::ostream& operator<<(std::ostream& s, const DiagnosticInfo& info);

    Date_t _timestamp;
    StringData _captureName;
    Backtrace _backtrace;

    DiagnosticInfo(const Date_t& timestamp, StringData captureName, Backtrace backtrace)
        : _timestamp(timestamp), _captureName(captureName), _backtrace(std::move(backtrace)) {}
};


}  // namespace mongo
