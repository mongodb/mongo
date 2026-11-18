// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host/extension_command.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/extension/host/extension_host_utils.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"

#include <array>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExtension

namespace mongo::extension::host {

namespace {

ClusterRole toClusterRole(MongoExtensionCommandClusterRoleEnum role) {
    switch (role) {
        case MongoExtensionCommandClusterRoleEnum::kShard:
            return ClusterRole::ShardServer;
        case MongoExtensionCommandClusterRoleEnum::kRouter:
            return ClusterRole::RouterServer;
        case MongoExtensionCommandClusterRoleEnum::kBoth:
            return {ClusterRole::ShardServer, ClusterRole::RouterServer};
    }
    MONGO_UNREACHABLE_TASSERT(13730003);
}

/**
 * Top-level reply fields that the host owns. Errors must be returned through the status, and the
 * remaining fields are appended by the host after the command runs.
 */
constexpr std::array kReservedReplyFields{
    "ok", "errmsg", "code", "codeName", "$clusterTime", "operationTime", "writeConcernError"};

}  // namespace

ExtensionCommand::ExtensionCommand(CommandDescriptorHandle descriptor,
                                   MongoExtensionCommandStaticProperties properties)
    : BasicCommand(descriptor->getName()),
      _descriptor(std::move(descriptor)),
      _properties(std::move(properties)) {}

void ExtensionCommand::registerCommand(CommandDescriptorHandle descriptor) {
    const std::string cmdName{descriptor->getName()};

    if (!feature_flags::gFeatureFlagExtensionCommands.isEnabled()) {
        LOGV2(13730005,
              "Extension commands are disabled; skipping command registration",
              "commandName"_attr = cmdName);
        return;
    }

    uassert(ErrorCodes::ExtensionError,
            fmt::format("Invalid extension command name '{}'", cmdName),
            !cmdName.empty() && cmdName.front() != '$' &&
                cmdName.find_first_of(std::string_view{".\0", 2}) == std::string::npos);

    auto properties = MongoExtensionCommandStaticProperties::parse(
        descriptor->getProperties(), IDLParserContext(fmt::format("{} properties", cmdName)));
    const auto declaredRoles = toClusterRole(properties.getClusterRole());

    // Registries exist only for the roles this process serves, and are built before extensions
    // load. Check each of them for a name collision before registering into any:
    // CommandRegistry::registerCommand() fatally asserts on a duplicate name, and an extension must
    // not shadow a built-in or another extension's command.
    std::vector<ClusterRole> roles;
    for (auto role : {ClusterRole::ShardServer, ClusterRole::RouterServer}) {
        if (declaredRoles.has(role) && getGlobalServiceContext()->getService(role)) {
            uassert(ErrorCodes::ExtensionError,
                    fmt::format("Extension command '{}' is already registered for the {} role",
                                cmdName,
                                toString(role)),
                    !getCommandRegistry(role)->findCommand(cmdName));
            roles.push_back(role);
        }
    }

    for (auto role : roles) {
        LOGV2(13730006,
              "Registering extension command",
              "commandName"_attr = cmdName,
              "role"_attr = toString(role));
        auto command = std::make_unique<ExtensionCommand>(descriptor, properties);
        command->initializeClusterRole(role);
        getCommandRegistry(role)->registerCommand(command.release());
    }
}

Command::AllowedOnSecondary ExtensionCommand::secondaryAllowed(ServiceContext*) const {
    switch (_properties.getAllowedOnSecondary()) {
        case MongoExtensionCommandAllowedOnSecondaryEnum::kAlways:
            return AllowedOnSecondary::kAlways;
        case MongoExtensionCommandAllowedOnSecondaryEnum::kNever:
            return AllowedOnSecondary::kNever;
        case MongoExtensionCommandAllowedOnSecondaryEnum::kOptIn:
            return AllowedOnSecondary::kOptIn;
    }
    MONGO_UNREACHABLE_TASSERT(13730007);
}

Command::ReadWriteType ExtensionCommand::getReadWriteType() const {
    switch (_properties.getReadWriteType()) {
        case MongoExtensionCommandReadWriteTypeEnum::kRead:
            return ReadWriteType::kRead;
        case MongoExtensionCommandReadWriteTypeEnum::kWrite:
            return ReadWriteType::kWrite;
        case MongoExtensionCommandReadWriteTypeEnum::kCommand:
            return ReadWriteType::kCommand;
    }
    MONGO_UNREACHABLE_TASSERT(13730004);
}

Status ExtensionCommand::checkAuthForOperation(OperationContext* opCtx,
                                               const DatabaseName& dbName,
                                               const BSONObj& cmdObj) const {
    const auto& requiredPrivileges = _properties.getRequiredPrivileges();
    if (requiredPrivileges.empty()) {
        // A command that does not require privileges can be run by any authenticated user.
        return Status::OK();
    }

    auto* authSession = AuthorizationSession::get(opCtx->getClient());
    for (const auto& privilege : requiredPrivileges) {
        // Each declared privilege is checked against the resource named by its own resource
        // pattern.
        const auto resource =
            privilege.getResourcePattern() == MongoExtensionPrivilegeResourcePatternEnum::kDatabase
            ? ResourcePattern::forDatabaseName(dbName)
            : parseResourcePattern(dbName, cmdObj);

        if (!authSession->isAuthorizedForActionsOnResource(
                resource, static_properties_util::toActionSet(privilege))) {
            return {ErrorCodes::Unauthorized,
                    fmt::format("not authorized to run command '{}'", getName())};
        }
    }
    return Status::OK();
}

bool ExtensionCommand::run(OperationContext* opCtx,
                           const DatabaseName& dbName,
                           const BSONObj& cmdObj,
                           BSONObjBuilder& result) {
    const auto nss = parseNs(dbName, cmdObj);
    const auto dbNameStr =
        DatabaseNameUtil::serialize(dbName, SerializationContext::stateCommandRequest());

    // TODO SERVER-137298: Pass the resolved collection UUID and view.
    const ::MongoExtensionCommandContext ctx{
        .namespaceString =
            {
                .databaseName = stringViewAsByteView(dbNameStr),
                .collectionName = stringViewAsByteView(nss.coll()),
            },
    };
    _descriptor->run(cmdObj, ctx, result);

    const auto reply = result.asTempObj();
    for (auto field : kReservedReplyFields) {
        uassert(ErrorCodes::ExtensionError,
                fmt::format("Extension command '{}' returned the reserved reply field '{}'",
                            getName(),
                            field),
                !reply.hasField(field));
    }
    return true;
}

}  // namespace mongo::extension::host
