// write_conflict_exception.h

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

#pragma once

#include <exception>

#include "mongo/util/assert_util.h"

#define MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN \
    do {                                      \
        int wcr__Attempts = 0;                \
        do {                                  \
            try
#define MONGO_WRITE_CONFLICT_RETRY_LOOP_END(PTXN, OPSTR, NSSTR) \
    catch (const ::mongo::WriteConflictException& wce) {        \
        const OperationContext* ptxn = (PTXN);                  \
        ++CurOp::get(ptxn)->debug().writeConflicts;             \
        wce.logAndBackoff(wcr__Attempts, (OPSTR), (NSSTR));     \
        ++wcr__Attempts;                                        \
        ptxn->recoveryUnit()->abandonSnapshot();                \
        continue;                                               \
    }                                                           \
    break;                                                      \
    }                                                           \
    while (true)                                                \
        ;                                                       \
    }                                                           \
    while (false)                                               \
        ;

namespace mongo {

/**
 * This is thrown if during a write, two or more operations conflict with each other.
 * For example if two operations get the same version of a document, and then both try to
 * modify that document, this exception will get thrown by one of them.
 */
class WriteConflictException : public DBException {
public:
    WriteConflictException();

    /**
     * Will log a message if sensible and will do an exponential backoff to make sure
     * we don't hammer the same doc over and over.
     * @param attempt - what attempt is this, 1 based
     * @param operation - e.g. "update"
     */
    static void logAndBackoff(int attempt, StringData operation, StringData ns);

    /**
     * If true, will call printStackTrace on every WriteConflictException created.
     * Can be set via setParameter named traceWriteConflictExceptions.
     */
    static std::atomic<bool> trace;  // NOLINT
};
}
