/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/embedded/service_entry_point_embedded.h"

#include "mongo/db/service_entry_point_common.h"
#include "mongo/embedded/not_implemented.h"

namespace mongo {

class ServiceEntryPointEmbedded::Hooks : public ServiceEntryPointCommon::Hooks {
public:
    bool lockedForWriting() const override {
        return false;
    }

    void waitForReadConcern(OperationContext*,
                            const CommandInvocation*,
                            const OpMsgRequest&) const override {}

    void waitForWriteConcern(OperationContext*,
                             const CommandInvocation*,
                             const repl::OpTime&,
                             BSONObjBuilder&) const override {}

    void waitForLinearizableReadConcern(OperationContext*) const override {}

    void uassertCommandDoesNotSpecifyWriteConcern(const BSONObj&) const override {}

    void attachCurOpErrInfo(OperationContext*, const BSONObj&) const override {}
};

DbResponse ServiceEntryPointEmbedded::handleRequest(OperationContext* opCtx, const Message& m) {
    return ServiceEntryPointCommon::handleRequest(opCtx, m, Hooks{});
}

void ServiceEntryPointEmbedded::startSession(transport::SessionHandle session) {
    UASSERT_NOT_IMPLEMENTED;
}

void ServiceEntryPointEmbedded::endAllSessions(transport::Session::TagMask tags) {}

bool ServiceEntryPointEmbedded::shutdown(Milliseconds timeout) {
    UASSERT_NOT_IMPLEMENTED;
}

ServiceEntryPoint::Stats ServiceEntryPointEmbedded::sessionStats() const {
    UASSERT_NOT_IMPLEMENTED;
}

size_t ServiceEntryPointEmbedded::numOpenSessions() const {
    UASSERT_NOT_IMPLEMENTED;
}

}  // namespace mongo
