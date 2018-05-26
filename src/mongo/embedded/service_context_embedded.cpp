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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context_registrar.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_lock_file.h"
#include "mongo/db/storage/storage_engine_metadata.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/embedded/service_context_embedded.h"
#include "mongo/embedded/service_entry_point_embedded.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/map_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/system_tick_source.h"

namespace mongo {
namespace {
ServiceContextRegistrar serviceContextCreator([]() {
    auto service = std::make_unique<ServiceContextMongoEmbedded>();
    service->setServiceEntryPoint(std::make_unique<ServiceEntryPointEmbedded>(service.get()));
    service->setTickSource(std::make_unique<SystemTickSource>());
    service->setFastClockSource(std::make_unique<SystemClockSource>());
    service->setPreciseClockSource(std::make_unique<SystemClockSource>());
    return service;
});
}  // namespace

extern bool _supportsDocLocking;

ServiceContextMongoEmbedded::ServiceContextMongoEmbedded() = default;

ServiceContextMongoEmbedded::~ServiceContextMongoEmbedded() = default;

std::unique_ptr<OperationContext> ServiceContextMongoEmbedded::_newOpCtx(Client* client,
                                                                         unsigned opId) {
    invariant(&cc() == client);
    auto opCtx = stdx::make_unique<OperationContext>(client, opId);

    if (isMMAPV1()) {
        opCtx->setLockState(stdx::make_unique<MMAPV1LockerImpl>());
    } else {
        opCtx->setLockState(stdx::make_unique<DefaultLockerImpl>());
    }

    opCtx->setRecoveryUnit(getStorageEngine()->newRecoveryUnit(),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    return opCtx;
}

}  // namespace mongo
