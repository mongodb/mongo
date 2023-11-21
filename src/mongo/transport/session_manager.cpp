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

#include "mongo/transport/session_manager.h"
#include "mongo/db/service_context.h"
#include "mongo/transport/transport_layer.h"

namespace mongo::transport {

class SessionManager::OperationObserver : public ServiceContext::ClientObserver {
public:
    void onCreateClient(Client* client) final {}
    void onDestroyClient(Client* client) final {}

    void onCreateOperationContext(OperationContext* opCtx) final {
        if (auto sm = getSessionManager(opCtx->getClient())) {
            sm->_totalOperations.fetchAndAddRelaxed(1);
        }
    }

    void onDestroyOperationContext(OperationContext* opCtx) final {
        if (auto sm = getSessionManager(opCtx->getClient())) {
            sm->_completedOperations.fetchAndAddRelaxed(1);
        }
    }

private:
    static SessionManager* getSessionManager(Client* client) {
        if (!client->session())
            return nullptr;

        auto* tl = client->session()->getTransportLayer();
        if (!tl)
            return nullptr;

        return tl->getSessionManager();
    }
};

namespace {
ServiceContext::ConstructorActionRegisterer opCountObserverRegisterer{
    "SessionManager::OperationObserver", [](ServiceContext* sc) {
        sc->registerClientObserver(std::make_unique<SessionManager::OperationObserver>());
    }};
}  // namespace

}  // namespace mongo::transport
