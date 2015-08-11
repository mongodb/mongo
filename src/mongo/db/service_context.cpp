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

#include "mongo/platform/basic.h"

#include "mongo/db/service_context.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {

ServiceContext* globalServiceContext = NULL;

}  // namespace

bool hasGlobalServiceContext() {
    return globalServiceContext;
}

ServiceContext* getGlobalServiceContext() {
    fassert(17508, globalServiceContext);
    return globalServiceContext;
}

void setGlobalServiceContext(std::unique_ptr<ServiceContext>&& serviceContext) {
    fassert(17509, serviceContext.get());

    delete globalServiceContext;

    globalServiceContext = serviceContext.release();
}

bool _supportsDocLocking = false;

bool supportsDocLocking() {
    return _supportsDocLocking;
}

bool isMMAPV1() {
    StorageEngine* globalStorageEngine = getGlobalServiceContext()->getGlobalStorageEngine();

    invariant(globalStorageEngine);
    return globalStorageEngine->isMmapV1();
}

Status validateStorageOptions(
    const BSONObj& storageEngineOptions,
    stdx::function<Status(const StorageEngine::Factory* const, const BSONObj&)> validateFunc) {
    BSONObjIterator storageIt(storageEngineOptions);
    while (storageIt.more()) {
        BSONElement storageElement = storageIt.next();
        StringData storageEngineName = storageElement.fieldNameStringData();
        if (storageElement.type() != mongo::Object) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "'storageEngine." << storageElement.fieldNameStringData()
                                        << "' has to be an embedded document.");
        }

        std::unique_ptr<StorageFactoriesIterator> sfi(
            getGlobalServiceContext()->makeStorageFactoriesIterator());
        invariant(sfi);
        bool found = false;
        while (sfi->more()) {
            const StorageEngine::Factory* const& factory = sfi->next();
            if (storageEngineName != factory->getCanonicalName()) {
                continue;
            }
            Status status = validateFunc(factory, storageElement.Obj());
            if (!status.isOK()) {
                return status;
            }
            found = true;
        }
        if (!found) {
            return Status(ErrorCodes::InvalidOptions,
                          str::stream() << storageEngineName
                                        << " is not a registered storage engine for this server");
        }
    }
    return Status::OK();
}

ServiceContext::~ServiceContext() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_clients.empty());
}

ServiceContext::UniqueClient ServiceContext::makeClient(std::string desc,
                                                        AbstractMessagingPort* p) {
    std::unique_ptr<Client> client(new Client(std::move(desc), this, p));
    auto observer = _clientObservers.cbegin();
    try {
        for (; observer != _clientObservers.cend(); ++observer) {
            observer->get()->onCreateClient(client.get());
        }
    } catch (...) {
        try {
            while (observer != _clientObservers.cbegin()) {
                --observer;
                observer->get()->onDestroyClient(client.get());
            }
        } catch (...) {
            std::terminate();
        }
        throw;
    }
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        invariant(_clients.insert(client.get()).second);
    }
    return UniqueClient(client.release());
}

TickSource* ServiceContext::getTickSource() const {
    return _tickSource.get();
}

ClockSource* ServiceContext::getClockSource() const {
    return _clockSource.get();
}

void ServiceContext::setTickSource(std::unique_ptr<TickSource> newSource) {
    _tickSource = std::move(newSource);
}

void ServiceContext::setClockSource(std::unique_ptr<ClockSource> newSource) {
    _clockSource = std::move(newSource);
}

void ServiceContext::ClientDeleter::operator()(Client* client) const {
    ServiceContext* const service = client->getServiceContext();
    {
        stdx::lock_guard<stdx::mutex> lk(service->_mutex);
        invariant(service->_clients.erase(client));
    }
    try {
        for (const auto& observer : service->_clientObservers) {
            observer->onDestroyClient(client);
        }
    } catch (...) {
        std::terminate();
    }
    delete client;
}

ServiceContext::UniqueOperationContext ServiceContext::makeOperationContext(Client* client) {
    auto opCtx = _newOpCtx(client);
    auto observer = _clientObservers.begin();
    try {
        for (; observer != _clientObservers.cend(); ++observer) {
            observer->get()->onCreateOperationContext(opCtx.get());
        }
    } catch (...) {
        try {
            while (observer != _clientObservers.cbegin()) {
                --observer;
                observer->get()->onDestroyOperationContext(opCtx.get());
            }
        } catch (...) {
            std::terminate();
        }
        throw;
    }
    // // TODO(schwerin): When callers no longer construct their own OperationContexts directly,
    // // but only through the ServiceContext, uncomment the following.  Until then, it must
    // // be done in the operation context destructors, which introduces a potential race.
    // {
    //     stdx::lock_guard<Client> lk(*client);
    //     client->setOperationContext(opCtx.get());
    // }
    return UniqueOperationContext(opCtx.release());
};

void ServiceContext::OperationContextDeleter::operator()(OperationContext* opCtx) const {
    auto client = opCtx->getClient();
    auto service = client->getServiceContext();
    // // TODO(schwerin): When callers no longer construct their own OperationContexts directly,
    // // but only through the ServiceContext, uncomment the following.  Until then, it must
    // // be done in the operation context destructors, which introduces a potential race.
    // {
    //     stdx::lock_guard<Client> lk(*client);
    //     client->resetOperationContext();
    // }
    try {
        for (const auto& observer : service->_clientObservers) {
            observer->onDestroyOperationContext(opCtx);
        }
    } catch (...) {
        std::terminate();
    }
    delete opCtx;
}

void ServiceContext::registerClientObserver(std::unique_ptr<ClientObserver> observer) {
    _clientObservers.push_back(std::move(observer));
}

ServiceContext::LockedClientsCursor::LockedClientsCursor(ServiceContext* service)
    : _lock(service->_mutex), _curr(service->_clients.cbegin()), _end(service->_clients.cend()) {}

Client* ServiceContext::LockedClientsCursor::next() {
    if (_curr == _end)
        return nullptr;
    Client* result = *_curr;
    ++_curr;
    return result;
}

BSONArray storageEngineList() {
    if (!hasGlobalServiceContext())
        return BSONArray();

    std::unique_ptr<StorageFactoriesIterator> sfi(
        getGlobalServiceContext()->makeStorageFactoriesIterator());

    if (!sfi)
        return BSONArray();

    BSONArrayBuilder engineArrayBuilder;

    while (sfi->more()) {
        engineArrayBuilder.append(sfi->next()->getCanonicalName());
    }

    return engineArrayBuilder.arr();
}

void appendStorageEngineList(BSONObjBuilder* result) {
    result->append("storageEngines", storageEngineList());
}
}  // namespace mongo
