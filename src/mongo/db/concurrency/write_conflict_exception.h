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
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * This is thrown if during a write, two or more operations conflict with each other.
 * For example if two operations get the same version of a document, and then both try to
 * modify that document, this exception will get thrown by one of them.
 */
class WriteConflictException final : public DBException {
public:
    WriteConflictException();

    WriteConflictException(StringData context) : WriteConflictException() {
        // Avoid unnecessary update to embedded Status within DBException.
        if (context.empty()) {
            return;
        }
        addContext(context);
    }

    WriteConflictException(const Status& status) : DBException(status) {}

    /**
     * If true, will call printStackTrace on every WriteConflictException created.
     * Can be set via setParameter named traceWriteConflictExceptions.
     */
    static inline AtomicWord<bool> trace{false};

private:
    void defineOnlyInFinalSubclassToPreventSlicing() final {}
};

}  // namespace mongo
