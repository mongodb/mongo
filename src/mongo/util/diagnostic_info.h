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
#include "mongo/db/service_context.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/time_support.h"

namespace mongo {
MONGO_FAIL_POINT_DECLARE(keepDiagnosticCaptureOnFailedLock);
/**
 * DiagnosticInfo keeps track of diagnostic information such as a developer provided
 * name, the time when a lock was first acquired, and a partial caller call stack.
 */
class DiagnosticInfo {
public:
    struct Diagnostic {
        static std::shared_ptr<DiagnosticInfo> get(Client*);
        static void set(Client*, std::shared_ptr<DiagnosticInfo>);
        static void clearDiagnostic();
        stdx::mutex m;
        std::shared_ptr<DiagnosticInfo> diagnostic;
    };

    virtual ~DiagnosticInfo() = default;
    DiagnosticInfo(const DiagnosticInfo&) = delete;
    DiagnosticInfo& operator=(const DiagnosticInfo&) = delete;
    DiagnosticInfo(DiagnosticInfo&&) = default;
    DiagnosticInfo& operator=(DiagnosticInfo&&) = default;

    struct StackFrame {
        std::string toString() const;
        friend bool operator==(const StackFrame& frame1, const StackFrame& frame2);
        friend bool operator!=(const StackFrame& frame1, const StackFrame& frame2) {
            return !(frame1 == frame2);
        }

        StringData objectPath;
        uintptr_t instructionOffset = 0;
    };
    struct StackTrace {
        std::string toString() const;
        friend bool operator==(const StackTrace& trace1, const StackTrace& trace2);
        friend bool operator!=(const StackTrace& trace1, const StackTrace& trace2) {
            return !(trace1 == trace2);
        }

        std::vector<StackFrame> frames;
    };

    Date_t getTimestamp() const {
        return _timestamp;
    }

    StringData getCaptureName() const {
        return _captureName;
    }

    StackTrace makeStackTrace() const;

    static std::vector<void*> getBacktraceAddresses();

    std::string toString() const;
    friend DiagnosticInfo takeDiagnosticInfo(const StringData& captureName);

private:
    friend bool operator==(const DiagnosticInfo& info1, const DiagnosticInfo& info2);
    friend bool operator!=(const DiagnosticInfo& info1, const DiagnosticInfo& info2) {
        return !(info1 == info2);
    }
    friend std::ostream& operator<<(std::ostream& s, const DiagnosticInfo& info);

    Date_t _timestamp;
    StringData _captureName;
    std::vector<void*> _backtraceAddresses;

    DiagnosticInfo(const Date_t& timestamp,
                   const StringData& captureName,
                   std::vector<void*> backtraceAddresses)
        : _timestamp(timestamp),
          _captureName(captureName),
          _backtraceAddresses(backtraceAddresses) {}
};


inline std::ostream& operator<<(std::ostream& s, const DiagnosticInfo::StackFrame& frame) {
    return s << frame.toString();
}

inline std::ostream& operator<<(std::ostream& s, const DiagnosticInfo& info) {
    return s << info.toString();
}

/**
 * Captures the diagnostic information based on the caller's context.
 */
DiagnosticInfo takeDiagnosticInfo(const StringData& captureName);
}  // namespace mongo
