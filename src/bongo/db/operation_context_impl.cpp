/**
 *    Copyright (C) 2014 BongoDB Inc.
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

#include "bongo/platform/basic.h"

#include "bongo/db/operation_context_impl.h"

#include <memory>

#include "bongo/db/client.h"
#include "bongo/db/concurrency/lock_state.h"
#include "bongo/db/curop.h"
#include "bongo/db/service_context.h"
#include "bongo/db/storage/storage_engine.h"
#include "bongo/stdx/memory.h"

namespace bongo {

namespace {
std::unique_ptr<Locker> newLocker() {
    if (isMMAPV1())
        return stdx::make_unique<MMAPV1LockerImpl>();
    return stdx::make_unique<DefaultLockerImpl>();
}
}  // namespace

using std::string;

OperationContextImpl::OperationContextImpl(Client* client, unsigned opId)
    : OperationContext(client, opId) {
    setLockState(newLocker());
    StorageEngine* storageEngine = getServiceContext()->getGlobalStorageEngine();
    setRecoveryUnit(storageEngine->newRecoveryUnit(), kNotInUnitOfWork);
}

ProgressMeter* OperationContextImpl::setMessage_inlock(const char* msg,
                                                       const std::string& name,
                                                       unsigned long long progressMeterTotal,
                                                       int secondsBetween) {
    return &CurOp::get(this)->setMessage_inlock(msg, name, progressMeterTotal, secondsBetween);
}

}  // namespace bongo
