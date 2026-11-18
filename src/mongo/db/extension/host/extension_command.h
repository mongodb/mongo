// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/commands.h"
#include "mongo/db/extension/public/extension_command_static_properties_gen.h"
#include "mongo/db/extension/shared/handle/command/command_descriptor.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo::extension::host {

/**
 * ExtensionCommand is a host-side BasicCommand that wraps an extension-provided command descriptor.
 * Its static behavior comes from the descriptor's static properties, and its run() delegates
 * execution to the extension via the C API boundary.
 */
class ExtensionCommand final : public BasicCommand {
public:
    ExtensionCommand(CommandDescriptorHandle descriptor,
                     MongoExtensionCommandStaticProperties properties);

    /**
     * Registers the command in each cluster role that its static properties declare and this
     * process serves. Throws if the properties are invalid or if a command with the same name is
     * already registered for one of the roles.
     */
    static void registerCommand(CommandDescriptorHandle descriptor);

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override;

    bool adminOnly() const override {
        return _properties.getAdminOnly();
    }

    bool supportsWriteConcern(const BSONObj& cmdObj) const override {
        return _properties.getSupportsWriteConcern();
    }

    ReadWriteType getReadWriteType() const override;

    bool isSubjectToIngressAdmissionControl() const override {
        return _properties.getIsSubjectToIngressAdmissionControl();
    }

    bool requiresAuthzChecks() const override {
        return !_properties.getRequiredPrivileges().empty();
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override;

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override;

    std::string help() const override {
        return "extension-defined command";
    }

private:
    CommandDescriptorHandle _descriptor;
    MongoExtensionCommandStaticProperties _properties;
};

}  // namespace mongo::extension::host
