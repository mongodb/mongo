// write_conflict_exception.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kWrite

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/log.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

std::atomic<bool> WriteConflictException::trace(false);  // NOLINT

WriteConflictException::WriteConflictException()
    : DBException("WriteConflict", ErrorCodes::WriteConflict) {
    if (trace) {
        printStackTrace();
    }
}

void WriteConflictException::logAndBackoff(int attempt, StringData operation, StringData ns) {
    LOG(1) << "Caught WriteConflictException doing " << operation << " on " << ns
           << ", attempt: " << attempt << " retrying";

    // All numbers below chosen by guess and check against a few random benchmarks.
    if (attempt < 4) {
        // no-op
    } else if (attempt < 10) {
        sleepmillis(1);
    } else if (attempt < 100) {
        sleepmillis(5);
    } else {
        sleepmillis(10);
    }
}

namespace {
// for WriteConflictException
ExportedServerParameter<bool, ServerParameterType::kStartupAndRuntime> TraceWCExceptionsSetting(
    ServerParameterSet::getGlobal(),
    "traceWriteConflictExceptions",
    &WriteConflictException::trace);
}
}
