/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <string>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/analyze_command_gen.h"
#include "mongo/db/query/query_feature_flags_gen.h"

namespace mongo {
namespace {

class CmdAnalyze final : public TypedCommand<CmdAnalyze> {
public:
    using Request = AnalyzeCommandRequest;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Command to generate statistics for a collection for use in the optimizer.";
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kWrite;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool supportsWriteConcern() const final {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        void typedRun(OperationContext* opCtx) {
            const auto& cmd = request();
            const NamespaceString& nss = ns();

            // Validate collection
            // Namespace exists
            AutoGetCollectionForReadMaybeLockFree autoColl(opCtx, nss);
            const auto& collection = autoColl.getCollection();
            uassert(6799700, str::stream() << "Couldn't find collection " << nss.ns(), collection);

            // Namespace cannot be capped collection
            const bool isCapped = collection->isCapped();
            uassert(6799701, "Analyze command is not supported on capped collections", !isCapped);

            // Namespace is normal or clustered collection
            const bool isNormalColl = nss.isNormalCollection();
            const bool isClusteredColl = collection->isClustered();
            uassert(6799702,
                    str::stream() << nss.toString() << " is not a normal or clustered collection",
                    isNormalColl || isClusteredColl);

            // Validate key
            auto key = cmd.getKey();
            if (key) {
                const FieldRef keyFieldRef(*key);

                // Empty path
                uassert(6799703, "Key path is empty", !keyFieldRef.empty());

                for (size_t i = 0; i < keyFieldRef.numParts(); ++i) {
                    FieldPath::uassertValidFieldName(keyFieldRef.getPart(i));
                }

                // Numerics
                const auto numericPathComponents = keyFieldRef.getNumericPathComponents(0);
                uassert(6799704,
                        str::stream() << "Key path contains numeric component "
                                      << keyFieldRef.getPart(*(numericPathComponents.begin())),
                        numericPathComponents.empty());
            }

            // Sample rate and sample size can't both be present
            auto sampleRate = cmd.getSampleRate();
            auto sampleSize = cmd.getSampleSize();
            uassert(6799705,
                    "Only one of sample rate and sample size may be present",
                    !sampleRate || !sampleSize);

            if (sampleSize || sampleRate) {
                uassert(
                    6799706, "It is illegal to pass sampleRate or sampleSize without a key", key);
            }

            uassert(6660400,
                    "Analyze command requires common query framework feature flag to be enabled",
                    serverGlobalParams.featureCompatibility.isVersionInitialized() &&
                        feature_flags::gFeatureFlagCommonQueryFramework.isEnabled(
                            serverGlobalParams.featureCompatibility));

            uasserted(ErrorCodes::NotImplemented, "Analyze command not yet implemented");
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto* authzSession = AuthorizationSession::get(opCtx->getClient());
            const NamespaceString& ns = request().getNamespace();

            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to call analyze on collection " << ns,
                    authzSession->isAuthorizedForActionsOnNamespace(ns, ActionType::analyze));
        }
    };

} cmdAnalyze;

}  // namespace
}  // namespace mongo
