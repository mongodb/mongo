/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <wiredtiger.h>

#include "mongo/db/operation_context.h"

namespace mongo {

void failPointPauseBeforeStorageCompactCommand();

/**
 * Sets up a WT_SESSION to have callback data with which to check for MDB layer interrupts.
 * See the 'general_handle' callback defined for wiredtiger_open() for more details.
 */
class SessionDataRAII {
public:
    /**
     * Allows WT operations running on this 'session' access to the MDB layer 'opCtx'.
     */
    SessionDataRAII(WT_SESSION* session, OperationContext* opCtx) : _session(session) {
        invariant(!_session->app_private);
        _session->app_private = opCtx;
    }

    /**
     * Clears on exit the WT_SESSION::app_private void*. This allows the WT_SESSION to be safely
     * returned to the WiredTigerSessionCache.
     */
    ~SessionDataRAII() {
        _session->app_private = nullptr;
    }

private:
    WT_SESSION* _session;
};

}  // namespace mongo
