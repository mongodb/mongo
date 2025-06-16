/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/storage/wiredtiger/wiredtiger_cursor_helpers.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/testing_proctor.h"

#include <string>

#include <wiredtiger.h>

namespace mongo {

namespace {
void handleWriteContextForDebugging(WiredTigerRecoveryUnit& ru, WT_CURSOR* cursor) {
    if (ru.shouldGatherWriteContextForDebugging()) {
        BSONObjBuilder builder;

        std::string s;
        StringStackTraceSink sink{s};
        printStackTrace(sink);
        builder.append("stacktrace", s);

        builder.append("uri", cursor->uri);

        ru.storeWriteContextForDebugging(builder.obj());
    }
}
}  // namespace

int wiredTigerCursorInsert(WiredTigerRecoveryUnit& ru, WT_CURSOR* cursor) {
    int ret = cursor->insert(cursor);
    if (MONGO_likely(ret == 0)) {
        ru.setTxnModified();
    }
    if (TestingProctor::instance().isEnabled()) {
        handleWriteContextForDebugging(ru, cursor);
    }
    return ret;
}

int wiredTigerCursorModify(WiredTigerRecoveryUnit& ru,
                           WT_CURSOR* cursor,
                           WT_MODIFY* entries,
                           int nentries) {
    int ret = cursor->modify(cursor, entries, nentries);
    if (MONGO_likely(ret == 0)) {
        ru.setTxnModified();
    }
    if (TestingProctor::instance().isEnabled()) {
        handleWriteContextForDebugging(ru, cursor);
    }
    return ret;
}

int wiredTigerCursorUpdate(WiredTigerRecoveryUnit& ru, WT_CURSOR* cursor) {
    int ret = cursor->update(cursor);
    if (MONGO_likely(ret == 0)) {
        ru.setTxnModified();
    }
    if (TestingProctor::instance().isEnabled()) {
        handleWriteContextForDebugging(ru, cursor);
    }
    return ret;
}

int wiredTigerCursorRemove(WiredTigerRecoveryUnit& ru, WT_CURSOR* cursor) {
    int ret = cursor->remove(cursor);
    if (MONGO_likely(ret == 0)) {
        ru.setTxnModified();
    }
    if (TestingProctor::instance().isEnabled()) {
        handleWriteContextForDebugging(ru, cursor);
    }
    return ret;
}

}  // namespace mongo
