/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_contract.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_repair_gen.h"
#include "mongo/util/assert_util.h"

#include <sstream>
#include <string>
#include <string_view>

namespace mongo {
namespace {

/**
 * Assembles a WiredTiger configuration string, mirroring the WTConfig helper used by the native
 * repair tooling: keys are comma-separated and nested groups are wrapped in parentheses. This keeps
 * the command from splicing user input into a raw config string by hand.
 */
class WtConfigBuilder {
public:
    WtConfigBuilder& append(std::string_view key) {
        if (!_first) {
            _oss << ",";
        }
        _first = false;
        _oss << key;
        return *this;
    }

    WtConfigBuilder& append(std::string_view key, std::string_view value) {
        append(key);
        _oss << "=" << value;
        return *this;
    }

    WtConfigBuilder& append(std::string_view key, const WtConfigBuilder& nested) {
        append(key);
        _oss << "=(" << nested.str() << ")";
        return *this;
    }

    std::string str() const {
        return _oss.str();
    }

private:
    std::ostringstream _oss;
    bool _first = true;
};

/**
 * Quotes a string value for inclusion in a WiredTiger config. The value becomes part of a config
 * string parsed by WiredTiger, so validate it against an allow-list of the characters that appear
 * in WiredTiger URIs and config keys (alphanumerics plus '.', '_', '-', ':' and '/') and reject
 * everything else. An allow-list -- rather than blocking known-bad characters -- keeps the typed
 * command surface from becoming a config-injection vector.
 */
std::string quoteConfigValue(std::string_view value) {
    for (char c : value) {
        const bool allowed = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '.' || c == '_' || c == '-' || c == ':' || c == '/';
        uassert(ErrorCodes::BadValue,
                "wiredTigerRepair config value contains a disallowed character",
                allowed);
    }
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted += '"';
    quoted += value;
    quoted += '"';
    return quoted;
}

// Builds the wiredtiger_repair() config for whichever read-only sub-command is present. The caller
// (typedRun) has already enforced that exactly one of the fetch sub-commands is set.
std::string buildRepairConfig(const WiredTigerRepairCommandRequest& request) {
    const auto& fetchDatabaseSize = request.getFetchDatabaseSize();
    const auto& fetchMetadata = request.getFetchMetadata();

    WtConfigBuilder config;
    if (fetchDatabaseSize) {
        WtConfigBuilder sub;
        sub.append("local", fetchDatabaseSize->getLocal() ? "true" : "false");
        config.append("fetch_database_size", sub);
    } else {
        WtConfigBuilder sub;
        sub.append("local", fetchMetadata->getLocal() ? "true" : "false");
        if (auto uri = fetchMetadata->getUri()) {
            sub.append("uri", quoteConfigValue(*uri));
        }
        if (auto key = fetchMetadata->getKey()) {
            sub.append("key", quoteConfigValue(*key));
        }
        config.append("fetch_metadata", sub);
    }
    return config.str();
}

/**
 * Command surface for WiredTiger's runtime maintenance operations. The typed fields are translated
 * into the WiredTiger config string here rather than accepting a raw config, and the work is
 * plumbed to the storage engine (never to WiredTigerKVEngine directly). Access is gated on
 * ActionType::wiredtiger, which is not held by any built-in role by default.
 */
class WiredTigerRepairCommand final : public TypedCommand<WiredTigerRepairCommand> {
public:
    using Request = WiredTigerRepairCommandRequest;
    using Response = WiredTigerRepairReply;

    std::string help() const override {
        return "WiredTiger maintenance command. Requires the 'wiredtiger' action privilege "
               "(ActionType::wiredtiger), which is not held by any built-in role by default. "
               "Provide exactly one of fetchDatabaseSize / fetchMetadata (read-only, via "
               "wiredtiger_repair) or fixDatabaseSize (recomputes the disaggregated database size "
               "via a checkpoint).";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    // Declare the authorization requirement (cluster resource + ActionType::wiredtiger) in the
    // command's contract so it is discoverable from the command definition, not only enforced in
    // doCheckAuthorization below. The IDL access_check generates this contract.
    const AuthorizationContract* getAuthorizationContract() const final {
        return &Request::kAuthorizationContract;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            const auto& req = request();
            const bool fixDatabaseSize = req.getFixDatabaseSize().value_or(false);
            const int subCommands = req.getFetchDatabaseSize().has_value() +
                req.getFetchMetadata().has_value() + (fixDatabaseSize ? 1 : 0);
            uassert(ErrorCodes::InvalidOptions,
                    "wiredTigerRepair requires exactly one sub-command (fetchDatabaseSize, "
                    "fetchMetadata, or fixDatabaseSize)",
                    subCommands == 1);

            auto* storageEngine = opCtx->getServiceContext()->getStorageEngine();

            // Global lock keeps the storage engine from shutting down mid-operation.
            // fixDatabaseSize (a checkpoint) needs MODE_IX with an explicit LocalWrite intent --
            // otherwise the IntentRegistry treats it as a replicated write and rejects it on
            // secondaries.
            Lock::GlobalLock globalLock{
                opCtx,
                fixDatabaseSize ? MODE_IX : MODE_IS,
                Date_t::max(),
                Lock::InterruptBehavior::kThrow,
                Lock::GlobalLockOptions{.skipFlowControlTicket = true,
                                        .skipRSTLLock = true,
                                        .explicitIntent = fixDatabaseSize
                                            ? rss::consensus::IntentRegistry::Intent::LocalWrite
                                            : rss::consensus::IntentRegistry::Intent::Read}};

            Response response;
            if (fixDatabaseSize) {
                uassertStatusOK(storageEngine->fixDatabaseSize());
                response.setResult(
                    "fixDatabaseSize: checkpoint(debug=(database_size_fix=true)) completed");
            } else {
                response.setResult(
                    uassertStatusOK(storageEngine->wiredTigerRepair(buildRepairConfig(req))));
            }
            return response;
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString::kEmpty;
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::wiredtiger));
        }
    };
};
MONGO_REGISTER_COMMAND(WiredTigerRepairCommand).forShard();

}  // namespace
}  // namespace mongo
