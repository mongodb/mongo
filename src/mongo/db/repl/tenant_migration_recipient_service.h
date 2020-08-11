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

#pragma once

#include <boost/optional.hpp>
#include <memory>

#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"

namespace mongo {

class OperationContext;
class ServiceContext;

namespace repl {

/**
 * TenantMigrationRecipientService is a primary only service to handle
 * data copy portion of a multitenant migration on recipient side.
 */
class TenantMigrationRecipientService final : public PrimaryOnlyService {
    // Disallows copying.
    TenantMigrationRecipientService(const TenantMigrationRecipientService&) = delete;
    TenantMigrationRecipientService& operator=(const TenantMigrationRecipientService&) = delete;

public:
    static constexpr StringData kTenantMigrationRecipientServiceName =
        "TenantMigrationRecipientService"_sd;

    explicit TenantMigrationRecipientService(ServiceContext* serviceContext);
    ~TenantMigrationRecipientService() = default;

    StringData getServiceName() const final;

    NamespaceString getStateDocumentsNS() const final;

    ThreadPool::Limits getThreadPoolLimits() const final;

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialStateDoc) const final;

    class Instance final : public PrimaryOnlyService::TypedInstance<Instance> {
    public:
        explicit Instance(BSONObj stateDoc);

        SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor) noexcept final;

        /*
         *  Returns the instance id.
         */
        const UUID& getMigrationUUID() const;

        /*
         *  Returns true if the instance state doc is marked for garbage collect.
         */
        bool isMarkedForGarbageCollect() const;

    private:
        // Protects below data members.
        mutable Mutex _mutex = MONGO_MAKE_LATCH("TenantMigrationRecipientService::_mutex");

        TenantMigrationRecipientDocument _stateDoc;
    };
};


}  // namespace repl
}  // namespace mongo
