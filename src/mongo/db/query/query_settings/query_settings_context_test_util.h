// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands.h"
#include "mongo/db/query/query_settings/query_settings_context.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"

#include <memory>
#include <utility>

namespace mongo {
class OperationContext;

namespace query_settings {

/**
 * Test-only helper that resolves 'settings' onto 'opCtx' as a PQS-supported command would, so that
 * 'forOp(opCtx)' returns them and 'QueryKnobConfiguration::get(opCtx)' installs their knob
 * overrides. Construction swaps the new settings into the operation's query settings state and
 * saves the previous state; destruction restores it, so guards may nest or be used sequentially on
 * an operation that outlives them.
 *
 * Note: any QueryKnobConfiguration already materialized for the operation is not reset. If knob
 * values were read (via 'QueryKnobConfiguration::get') under one guard, a subsequent guard's
 * settings will not be reflected in that cached configuration.
 */
class QuerySettingsGuardForTest {
public:
    QuerySettingsGuardForTest(OperationContext* opCtx, BSONObj settingsObj);
    QuerySettingsGuardForTest(OperationContext* opCtx, const QuerySettings& settings);
    ~QuerySettingsGuardForTest();

    QuerySettingsGuardForTest(const QuerySettingsGuardForTest&) = delete;
    QuerySettingsGuardForTest& operator=(const QuerySettingsGuardForTest&) = delete;

    QuerySettingsGuardForTest(QuerySettingsGuardForTest&& other) noexcept
        : _state(std::exchange(other._state, nullptr)), _previous(std::move(other._previous)) {}

    QuerySettingsGuardForTest& operator=(QuerySettingsGuardForTest&& other) noexcept {
        QuerySettingsGuardForTest moved(std::move(other));
        using std::swap;
        swap(_state, moved._state);
        swap(_previous, moved._previous);
        return *this;
    }

private:
    query_settings_details::QuerySettingsOperationState* _state;
    query_settings_details::QuerySettingsOperationState _previous;
};

/**
 * Rejection is decided by the active command invocation; this scope installs one with the given
 * bypass behavior (true simulates an exempt command such as explain).
 */
class CommandInvocationScope {
public:
    explicit CommandInvocationScope(OperationContext* opCtx,
                                    bool shouldBypassQuerySettingsRejection = false)
        : _opCtx(opCtx), _previousInvocation(CommandInvocation::get(opCtx)) {
        static const MockCommand command;
        CommandInvocation::set(
            opCtx,
            std::make_shared<MockCommandInvocation>(&command, shouldBypassQuerySettingsRejection));
    }

    ~CommandInvocationScope() {
        CommandInvocation::set(_opCtx, std::move(_previousInvocation));
    }

private:
    class MockCommand final : public Command {
    public:
        MockCommand() : Command("mockCommand") {}

        std::unique_ptr<CommandInvocation> parse(OperationContext*, const OpMsgRequest&) override {
            MONGO_UNREACHABLE;
        }

        AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
            return AllowedOnSecondary::kAlways;
        }
    };

    class MockCommandInvocation final : public CommandInvocation {
    public:
        MockCommandInvocation(const Command* command, bool shouldBypassQuerySettingsRejection)
            : CommandInvocation(command),
              _shouldBypassQuerySettingsRejection(shouldBypassQuerySettingsRejection) {}

        void run(OperationContext*, rpc::ReplyBuilderInterface*) override {
            MONGO_UNREACHABLE;
        }

        NamespaceString ns() const override {
            return NamespaceString();
        }

        const DatabaseName& db() const override {
            return DatabaseName::kAdmin;
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        bool shouldBypassQuerySettingsRejection() const override {
            return _shouldBypassQuerySettingsRejection;
        }

        const GenericArguments& getGenericArguments() const override {
            return _genericArguments;
        }

    private:
        void doCheckAuthorization(OperationContext*) const override {}

        const bool _shouldBypassQuerySettingsRejection;
        GenericArguments _genericArguments;
    };

    OperationContext* _opCtx;
    std::shared_ptr<CommandInvocation> _previousInvocation;
};

}  // namespace query_settings
}  // namespace mongo
