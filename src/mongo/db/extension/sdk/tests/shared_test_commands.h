// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/command.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

namespace mongo::extension::sdk::shared_test_commands {

/**
 * Test scaffolding for extension commands, shared by the host and SDK unit tests and the
 * mock_commands test extension.
 */

/**
 * Replies with {request, db, coll}: the full request, followed by the namespace from the command
 * context. If the request has an 'extra' object, its fields are also appended to the top level of
 * the reply, so that tests can make the command return a specific reply field. The name and static
 * properties default to 'testEcho' with no required privileges.
 */
class EchoCommandDescriptor : public CommandDescriptor {
public:
    static constexpr std::string_view kCommandName = "testEcho";

    static BSONObj defaultProperties() {
        return BSON("clusterRole" << "both"
                                  << "requiredPrivileges" << BSONArray());
    }

    EchoCommandDescriptor(std::string name = std::string(kCommandName),
                          BSONObj properties = defaultProperties())
        : CommandDescriptor(std::move(name)), _properties(properties.getOwned()) {}

    BSONObj getProperties() const override {
        return _properties;
    }

    BSONObj run(const BSONObj& request, const ::MongoExtensionCommandContext& ctx) const override {
        BSONObjBuilder bob;
        bob.append("request", request);
        bob.append("db", byteViewAsStringView(ctx.namespaceString.databaseName));
        bob.append("coll", byteViewAsStringView(ctx.namespaceString.collectionName));
        if (auto extra = request["extra"]; extra.isABSONObj()) {
            bob.appendElements(extra.Obj());
        }
        return bob.obj();
    }

private:
    const BSONObj _properties;
};

}  // namespace mongo::extension::sdk::shared_test_commands
