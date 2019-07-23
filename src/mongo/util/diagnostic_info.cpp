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

#include "mongo/base/init.h"
#include "mongo/db/client.h"
#include "mongo/platform/basic.h"
#include "mongo/platform/mutex.h"

#include "mongo/util/diagnostic_info.h"

#include "mongo/util/clock_source.h"

namespace mongo {

namespace {
const auto gDiagnosticHandle = Client::declareDecoration<DiagnosticInfo::Diagnostic>();

MONGO_INITIALIZER(LockActions)(InitializerContext* context) {
    class LockActionsSubclass : public LockActions {
        void onContendedLock(const StringData& name) override {
            if (haveClient()) {
                DiagnosticInfo::Diagnostic::set(
                    Client::getCurrent(),
                    std::make_shared<DiagnosticInfo>(takeDiagnosticInfo(name)));
            }
        }
        void onUnlock() override {
            if (haveClient()) {
                DiagnosticInfo::Diagnostic::set(Client::getCurrent(), nullptr);
            }
        }
    };
    std::unique_ptr<LockActions> myPointer = std::make_unique<LockActionsSubclass>();
    Mutex::setLockActions(std::move(myPointer));

    return Status::OK();
}
}  // namespace

auto DiagnosticInfo::Diagnostic::get(Client* const client) -> DiagnosticInfo& {
    auto& handle = gDiagnosticHandle(client);
    stdx::lock_guard lk(handle.m);
    return *handle.diagnostic;
}

void DiagnosticInfo::Diagnostic::set(Client* const client,
                                     std::shared_ptr<DiagnosticInfo> newDiagnostic) {
    auto& handle = gDiagnosticHandle(client);
    stdx::lock_guard lk(handle.m);
    handle.diagnostic = newDiagnostic;
}

DiagnosticInfo takeDiagnosticInfo(const StringData& captureName) {
    return DiagnosticInfo(getGlobalServiceContext()->getFastClockSource()->now(), captureName);
}
}  // namespace mongo
