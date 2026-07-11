// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands.h"
#include "mongo/db/commands_test_example_gen.h"

#include <string_view>

namespace mongo {
namespace commands_test_example {

// Define example commands that can be used in unit tests.

/**
 * ExampleIncrement command defined using TypedCommand.
 */
class ExampleIncrementCommand final : public TypedCommand<ExampleIncrementCommand> {
private:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return "Return an incremented request.i. Example of a simple TypedCommand.";
    }

public:
    using Request = ExampleIncrement;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        /**
         * Reply with an incremented 'request.i'.
         */
        auto typedRun(OperationContext* opCtx) {
            ExampleIncrementReply r;
            r.setIPlusOne(request().getI() + 1);
            return r;
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext*) const override {}

        /**
         * The ns() for when Request's IDL specifies "namespace: concatenate_with_db".
         */
        NamespaceString ns() const override {
            return request().getNamespace();
        }
    };
};

/**
 * ExampleMinimal command defined using TypedCommand with MinimalInvocationBase.
 */
class ExampleMinimalCommand final : public TypedCommand<ExampleMinimalCommand> {
private:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Return an incremented request.i. Example of a simple TypedCommand.";
    }

public:
    using Request = ExampleMinimal;

    class Invocation final : public MinimalInvocationBase {
    public:
        using MinimalInvocationBase::MinimalInvocationBase;

        /**
         * Reply with an incremented 'request.i'.
         */
        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) override {
            ExampleIncrementReply r;
            r.setIPlusOne(request().getI() + 1);
            reply->fillFrom(r);
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {}

        void doCheckAuthorization(OperationContext*) const override {}

        /**
         * The ns() for when Request's IDL specifies "namespace: concatenate_with_db".
         */
        NamespaceString ns() const override {
            return request().getNamespace();
        }
    };
};

/**
 * ExampleVoid command defined using TypedCommand with a void typedRun.
 */
class ExampleVoidCommand final : public TypedCommand<ExampleVoidCommand> {
private:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Accepts Request and returns void.";
    }

public:
    using Request = ExampleVoid;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        /**
         * Have some testable side-effect.
         */
        void typedRun(OperationContext*) {
            static_cast<const ExampleVoidCommand*>(definition())->iCapture = request().getI() + 1;
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {}

        void doCheckAuthorization(OperationContext*) const override {}

        /**
         * The ns() for when Request's IDL specifies "namespace: concatenate_with_db".
         */
        NamespaceString ns() const override {
            return request().getNamespace();
        }
    };

    mutable std::int32_t iCapture = 0;
};

class ExampleVoidCommandNeverAllowedOnSecondary
    : public TypedCommand<ExampleVoidCommandNeverAllowedOnSecondary> {
private:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

public:
    using Request = ExampleVoidNeverAllowedOnSecondary;
    using Invocation = ExampleVoidCommand::Invocation;
};

class ExampleVoidCommandAlwaysAllowedOnSecondary
    : public TypedCommand<ExampleVoidCommandAlwaysAllowedOnSecondary> {
private:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kAlways;
    }

public:
    using Request = ExampleVoidAlwaysAllowedOnSecondary;
    using Invocation = ExampleVoidCommand::Invocation;
};

class ExampleVoidCommandAllowedOnSecondaryIfOptedIn
    : public TypedCommand<ExampleVoidCommandAllowedOnSecondaryIfOptedIn> {
private:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kOptIn;
    }

public:
    using Request = ExampleVoidAllowedOnSecondaryIfOptedIn;
    using Invocation = ExampleVoidCommand::Invocation;
};

/**
 * ExampleVoid command defined using TypedCommand with Derived.
 */
template <typename Derived>
class MyCommand : public TypedCommand<MyCommand<Derived>> {
public:
    class Invocation final : public TypedCommand<MyCommand>::InvocationBase {
    public:
        using Base = typename TypedCommand<MyCommand>::InvocationBase;
        using Base::Base;

        auto typedRun(OperationContext*) const {
            return _command()->doRun();
        }

    private:
        NamespaceString ns() const override {
            return Base::request().getNamespace();
        }
        bool supportsWriteConcern() const override {
            return false;
        }
        void doCheckAuthorization(OperationContext* opCtx) const override {
            return _command()->doAuth();
        }

        const Derived* _command() const {
            return static_cast<const Derived*>(Base::definition());
        }
    };

    using Request = ExampleVoid;

    explicit MyCommand(std::string_view name) : TypedCommand<MyCommand>(name) {}

    void doRun() const {}
    void doAuth() const {}

private:
    Command::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return "Accepts Request and returns void.";
    }
};

/**
 * MyCommand command that throws an UnknownError error.
 */
class ThrowsStatusCommand : public MyCommand<ThrowsStatusCommand> {
public:
    ThrowsStatusCommand() : MyCommand<ThrowsStatusCommand>{"throwsStatus"} {}
    void doRun() const {
        uasserted(ErrorCodes::UnknownError, "some error");
    }
};

/**
 * MyCommand command that throws an Unauthorized error.
 */
class UnauthorizedCommand : public MyCommand<UnauthorizedCommand> {
public:
    UnauthorizedCommand() : MyCommand<UnauthorizedCommand>{"unauthorizedCmd"} {}
    void doAuth() const {
        uasserted(ErrorCodes::Unauthorized, "Not authorized");
    }
};

}  // namespace commands_test_example
}  // namespace mongo
