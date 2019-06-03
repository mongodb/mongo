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
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * DiagnosticInfo keeps track of diagnostic information such as a developer provided
 * name, the time when a lock was first acquired, and a partial caller call stack.
 */
class DiagnosticInfo {
public:
    virtual ~DiagnosticInfo() = default;
    DiagnosticInfo(const DiagnosticInfo&) = delete;
    DiagnosticInfo& operator=(const DiagnosticInfo&) = delete;
    DiagnosticInfo(DiagnosticInfo&&) = default;
    DiagnosticInfo& operator=(DiagnosticInfo&&) = default;

    Date_t getTimestamp() {
        return _timestamp;
    }

    StringData getCaptureName() {
        return _captureName;
    }

    friend DiagnosticInfo takeDiagnosticInfo(const StringData& captureName);

private:
    Date_t _timestamp;
    StringData _captureName;


    DiagnosticInfo(const Date_t& timestamp, const StringData& captureName)
        : _timestamp(timestamp), _captureName(captureName) {}
};

/**
 * Captures the diagnostic information based on the caller's context.
 */
DiagnosticInfo takeDiagnosticInfo(const StringData& captureName);


}  // namespace monogo