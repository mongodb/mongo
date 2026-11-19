// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/public/extension_command_static_properties_gen.h"
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/sdk/command.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"

#include <string>
#include <string_view>
#include <vector>

namespace sdk = mongo::extension::sdk;

namespace {

/**
 * Builds the static-properties declaration shared by the three mock search index management
 * commands, mirroring the built-ins they replace.
 */
mongo::BSONObj buildSearchIndexCommandProperties(
    mongo::extension::MongoExtensionPrivilegeActionEnum action,
    bool subjectToIngressAdmissionControl) {
    mongo::extension::MongoExtensionCommandStaticProperties properties;
    properties.setClusterRole(mongo::extension::MongoExtensionCommandClusterRoleEnum::kBoth);
    properties.setReadWriteType(mongo::extension::MongoExtensionCommandReadWriteTypeEnum::kWrite);
    properties.setAllowedOnSecondary(
        mongo::extension::MongoExtensionCommandAllowedOnSecondaryEnum::kNever);
    properties.setAdminOnly(false);
    properties.setSupportsWriteConcern(false);
    properties.setIsSubjectToIngressAdmissionControl(subjectToIngressAdmissionControl);

    mongo::extension::MongoExtensionPrivilegeActionEntry actionEntry;
    actionEntry.setAction(action);
    mongo::extension::MongoExtensionRequiredPrivilege privilege;
    privilege.setResourcePattern(
        mongo::extension::MongoExtensionPrivilegeResourcePatternEnum::kNamespace);
    privilege.setActions({actionEntry});
    properties.setRequiredPrivileges(
        std::vector<mongo::extension::MongoExtensionRequiredPrivilege>{privilege});

    mongo::BSONObjBuilder builder;
    properties.serialize(&builder);
    return builder.obj();
}

mongo::BSONObj mockRun(std::string_view message, const ::MongoExtensionCommandContext& ctx) {
    sdk_uassert(
        mongo::ErrorCodes::NamespaceNotFound, "collection does not exist", ctx.uuidString.len > 0);

    mongo::BSONObjBuilder builder;
    builder.append("msg", message);
    builder.append(
        "db",
        std::string(mongo::extension::byteViewAsStringView(ctx.namespaceString.databaseName)));
    builder.append(
        "collection",
        std::string(mongo::extension::byteViewAsStringView(ctx.namespaceString.collectionName)));
    return builder.obj();
}

}  // namespace

/**
 * Mock replacements for the built-in createSearchIndexes, updateSearchIndex, and dropSearchIndex
 * commands. They declare the same static properties and required privileges as the built-ins, so
 * the host's parse, auth, and properties path is exercised identically, but they perform no index
 * management.
 */
class CreateSearchIndexesCommandDescriptor : public sdk::CommandDescriptor {
public:
    static constexpr const char* kCommandName = "createSearchIndexes";

    CreateSearchIndexesCommandDescriptor() : CommandDescriptor(kCommandName) {}

    mongo::BSONObj getProperties() const override {
        return buildSearchIndexCommandProperties(
            mongo::extension::MongoExtensionPrivilegeActionEnum::kCreateSearchIndexes,
            /*subjectToIngressAdmissionControl=*/false);
    }

    mongo::BSONObj run(const mongo::BSONObj&,
                       const ::MongoExtensionCommandContext& ctx,
                       const sdk::CommandExecutionContextHandle&) const override {
        return mockRun("creating a search index", ctx);
    }
};

class UpdateSearchIndexCommandDescriptor : public sdk::CommandDescriptor {
public:
    static constexpr const char* kCommandName = "updateSearchIndex";

    UpdateSearchIndexCommandDescriptor() : CommandDescriptor(kCommandName) {}

    mongo::BSONObj getProperties() const override {
        return buildSearchIndexCommandProperties(
            mongo::extension::MongoExtensionPrivilegeActionEnum::kUpdateSearchIndex,
            /*subjectToIngressAdmissionControl=*/true);
    }

    mongo::BSONObj run(const mongo::BSONObj&,
                       const ::MongoExtensionCommandContext& ctx,
                       const sdk::CommandExecutionContextHandle&) const override {
        return mockRun("updating a search index", ctx);
    }
};

class DropSearchIndexCommandDescriptor : public sdk::CommandDescriptor {
public:
    static constexpr const char* kCommandName = "dropSearchIndex";

    DropSearchIndexCommandDescriptor() : CommandDescriptor(kCommandName) {}

    mongo::BSONObj getProperties() const override {
        return buildSearchIndexCommandProperties(
            mongo::extension::MongoExtensionPrivilegeActionEnum::kDropSearchIndex,
            /*subjectToIngressAdmissionControl=*/true);
    }

    mongo::BSONObj run(const mongo::BSONObj&,
                       const ::MongoExtensionCommandContext& ctx,
                       const sdk::CommandExecutionContextHandle&) const override {
        return mockRun("dropping a search index", ctx);
    }
};

DEFAULT_EXEC_STAGE(ListSearchIndexes)
DEFAULT_LOGICAL_STAGE(ListSearchIndexes)
DEFAULT_AST_NODE(ListSearchIndexes)
DEFAULT_PARSE_NODE(ListSearchIndexes)

using ListSearchIndexesStageDescriptor =
    sdk::TestStageDescriptor<"$listSearchIndexes", ListSearchIndexesParseNode>;

class SearchIndexManagementExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerCommand<CreateSearchIndexesCommandDescriptor>(portal);
        _registerCommand<UpdateSearchIndexCommandDescriptor>(portal);
        _registerCommand<DropSearchIndexCommandDescriptor>(portal);
        _registerStage<ListSearchIndexesStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(SearchIndexManagementExtension)
DEFINE_GET_EXTENSION()
