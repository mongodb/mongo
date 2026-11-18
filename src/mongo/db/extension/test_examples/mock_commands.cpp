// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/public/extension_command_static_properties_gen.h"
#include "mongo/db/extension/sdk/command.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/shared_test_commands.h"

#include <string>
#include <string_view>
#include <vector>

namespace sdk = mongo::extension::sdk;

namespace {

mongo::BSONObj serializeProperties(
    std::vector<mongo::extension::MongoExtensionRequiredPrivilege> requiredPrivileges) {
    mongo::extension::MongoExtensionCommandStaticProperties properties;
    properties.setClusterRole(mongo::extension::MongoExtensionCommandClusterRoleEnum::kBoth);
    properties.setRequiredPrivileges(std::move(requiredPrivileges));

    mongo::BSONObjBuilder builder;
    properties.serialize(&builder);
    return builder.obj();
}

/**
 * Requires the 'find' action on a resource, so that tests can check that the host enforces a
 * declared privilege. The resource pattern selects whether the action is required on the command's
 * target namespace (kNamespace) or on the whole database (kDatabase).
 */
class RequiresFindCommandDescriptor : public sdk::CommandDescriptor {
public:
    RequiresFindCommandDescriptor(std::string name, bool requiresDatabasePrivilege)
        : CommandDescriptor(std::move(name)),
          _requiresDatabasePrivilege(requiresDatabasePrivilege) {}

    mongo::BSONObj getProperties() const override {
        mongo::extension::MongoExtensionPrivilegeActionEntry findAction;
        findAction.setAction(mongo::extension::MongoExtensionPrivilegeActionEnum::kFind);
        mongo::extension::MongoExtensionRequiredPrivilege privilege;
        privilege.setResourcePattern(
            _requiresDatabasePrivilege
                ? mongo::extension::MongoExtensionPrivilegeResourcePatternEnum::kDatabase
                : mongo::extension::MongoExtensionPrivilegeResourcePatternEnum::kNamespace);
        privilege.setActions({findAction});
        return serializeProperties({privilege});
    }

    mongo::BSONObj run(const mongo::BSONObj&,
                       const ::MongoExtensionCommandContext&) const override {
        return mongo::BSONObj();
    }

private:
    bool _requiresDatabasePrivilege;
};

/** Requires the 'find' action on the command's target namespace. */
class RequiresFindOnNamespaceCommandDescriptor : public RequiresFindCommandDescriptor {
public:
    static constexpr std::string_view kCommandName = "extRequiresFind";

    RequiresFindOnNamespaceCommandDescriptor()
        : RequiresFindCommandDescriptor(std::string(kCommandName),
                                        /*requiresDatabasePrivilege=*/false) {}
};

/** Requires the 'find' action on the whole database. */
class RequiresFindOnDatabaseCommandDescriptor : public RequiresFindCommandDescriptor {
public:
    static constexpr std::string_view kCommandName = "extRequiresFindDb";

    RequiresFindOnDatabaseCommandDescriptor()
        : RequiresFindCommandDescriptor(std::string(kCommandName),
                                        /*requiresDatabasePrivilege=*/true) {}
};

}  // namespace

class MockCommandsExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerCommand<sdk::shared_test_commands::EchoCommandDescriptor>(portal);
        _registerCommand<RequiresFindOnNamespaceCommandDescriptor>(portal);
        _registerCommand<RequiresFindOnDatabaseCommandDescriptor>(portal);
    }
};

REGISTER_EXTENSION(MockCommandsExtension)
DEFINE_GET_EXTENSION()
