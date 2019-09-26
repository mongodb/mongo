/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/db/commands/rwc_defaults_commands_gen.h"
#include "mongo/db/read_write_concern_defaults.h"

namespace mongo {
namespace {

class SetDefaultRWConcernCommand : public TypedCommand<SetDefaultRWConcernCommand> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
    bool adminOnly() const override {
        return true;
    }
    std::string help() const override {
        return "set the current read/write concern defaults (cluster-wide)";
    }

public:
    using Request = SetDefaultRWConcern;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        auto typedRun(OperationContext* opCtx) {
            auto rc = request().getDefaultReadConcern();
            auto wc = request().getDefaultWriteConcern();
            uassert(ErrorCodes::BadValue,
                    str::stream() << "At least one of the \""
                                  << SetDefaultRWConcern::kDefaultReadConcernFieldName << "\" or \""
                                  << SetDefaultRWConcern::kDefaultWriteConcernFieldName
                                  << "\" fields must be present",
                    rc || wc);

            auto& rwcDefaults = ReadWriteConcernDefaults::get(opCtx->getServiceContext());
            return rwcDefaults.setConcerns(opCtx, rc, wc);
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext*) const override {
            // TODO: add and use privilege action
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }
    };
} setDefaultRWConcernCommand;

class GetDefaultRWConcernCommand : public TypedCommand<GetDefaultRWConcernCommand> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    bool adminOnly() const override {
        return true;
    }
    std::string help() const override {
        return "get the current read/write concern defaults being applied by this node";
    }

public:
    using Request = GetDefaultRWConcern;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        auto typedRun(OperationContext* opCtx) {
            auto& rwcDefaults = ReadWriteConcernDefaults::get(opCtx->getServiceContext());
            return rwcDefaults.getDefault();
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext*) const override {
            // TODO: add and use privilege action
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }
    };
} getDefaultRWConcernCommand;
}  // namespace
}  // namespace mongo
