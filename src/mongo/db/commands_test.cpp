/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands_test_example_gen.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(Commands, appendCommandStatusOK) {
    BSONObjBuilder actualResult;
    CommandHelpers::appendCommandStatusNoThrow(actualResult, Status::OK());

    BSONObjBuilder expectedResult;
    expectedResult.append("ok", 1.0);

    ASSERT_BSONOBJ_EQ(actualResult.obj(), expectedResult.obj());
}

TEST(Commands, appendCommandStatusError) {
    BSONObjBuilder actualResult;
    const Status status(ErrorCodes::InvalidLength, "Response payload too long");
    CommandHelpers::appendCommandStatusNoThrow(actualResult, status);

    BSONObjBuilder expectedResult;
    expectedResult.append("ok", 0.0);
    expectedResult.append("errmsg", status.reason());
    expectedResult.append("code", status.code());
    expectedResult.append("codeName", ErrorCodes::errorString(status.code()));

    ASSERT_BSONOBJ_EQ(actualResult.obj(), expectedResult.obj());
}

TEST(Commands, appendCommandStatusNoOverwrite) {
    BSONObjBuilder actualResult;
    actualResult.append("a", "b");
    actualResult.append("c", "d");
    actualResult.append("ok", "not ok");
    const Status status(ErrorCodes::InvalidLength, "Response payload too long");
    CommandHelpers::appendCommandStatusNoThrow(actualResult, status);

    BSONObjBuilder expectedResult;
    expectedResult.append("a", "b");
    expectedResult.append("c", "d");
    expectedResult.append("ok", "not ok");
    expectedResult.append("errmsg", status.reason());
    expectedResult.append("code", status.code());
    expectedResult.append("codeName", ErrorCodes::errorString(status.code()));

    ASSERT_BSONOBJ_EQ(actualResult.obj(), expectedResult.obj());
}

TEST(Commands, appendCommandStatusErrorExtraInfo) {
    BSONObjBuilder actualResult;
    const Status status(ErrorExtraInfoExample(123), "not again!");
    CommandHelpers::appendCommandStatusNoThrow(actualResult, status);

    BSONObjBuilder expectedResult;
    expectedResult.append("ok", 0.0);
    expectedResult.append("errmsg", status.reason());
    expectedResult.append("code", status.code());
    expectedResult.append("codeName", ErrorCodes::errorString(status.code()));
    expectedResult.append("data", 123);

    ASSERT_BSONOBJ_EQ(actualResult.obj(), expectedResult.obj());
}

class ParseNsOrUUID : public unittest::Test {
public:
    ParseNsOrUUID()
        : client(service.makeClient("test")),
          opCtxPtr(client->makeOperationContext()),
          opCtx(opCtxPtr.get()) {}
    ServiceContextNoop service;
    ServiceContext::UniqueClient client;
    ServiceContext::UniqueOperationContext opCtxPtr;
    OperationContext* opCtx;
};

TEST_F(ParseNsOrUUID, FailWrongType) {
    auto cmd = BSON("query" << BSON("a" << BSON("$gte" << 11)));
    ASSERT_THROWS_CODE(
        CommandHelpers::parseNsOrUUID("db", cmd), DBException, ErrorCodes::InvalidNamespace);
}

TEST_F(ParseNsOrUUID, FailEmptyDbName) {
    auto cmd = BSON("query"
                    << "coll");
    ASSERT_THROWS_CODE(
        CommandHelpers::parseNsOrUUID("", cmd), DBException, ErrorCodes::InvalidNamespace);
}

TEST_F(ParseNsOrUUID, FailInvalidDbName) {
    auto cmd = BSON("query"
                    << "coll");
    ASSERT_THROWS_CODE(
        CommandHelpers::parseNsOrUUID("test.coll", cmd), DBException, ErrorCodes::InvalidNamespace);
}

TEST_F(ParseNsOrUUID, ParseValidColl) {
    auto cmd = BSON("query"
                    << "coll");
    auto parsedNss = CommandHelpers::parseNsOrUUID("test", cmd);
    ASSERT_EQ(*parsedNss.nss(), NamespaceString("test.coll"));
}

TEST_F(ParseNsOrUUID, ParseValidUUID) {
    const CollectionUUID uuid = UUID::gen();
    auto cmd = BSON("query" << uuid);
    auto parsedNsOrUUID = CommandHelpers::parseNsOrUUID("test", cmd);
    ASSERT_EQUALS(uuid, *parsedNsOrUUID.uuid());
}

/**
 * TypedCommand test
 */
class ExampleIncrementCommand final : public TypedCommand<ExampleIncrementCommand> {
private:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Return an incremented request.i. Example of a simple TypedCommand.";
    }

public:
    using Request = commands_test_example::ExampleIncrement;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        /**
         * Reply with an incremented 'request.i'.
         */
        auto typedRun(OperationContext* opCtx) {
            commands_test_example::ExampleIncrementReply r;
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

// Just like ExampleIncrementCommand, but using the MinimalInvocationBase.
class ExampleMinimalCommand final : public TypedCommand<ExampleMinimalCommand> {
private:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Return an incremented request.i. Example of a simple TypedCommand.";
    }

public:
    using Request = commands_test_example::ExampleMinimal;

    class Invocation final : public MinimalInvocationBase {
    public:
        using MinimalInvocationBase::MinimalInvocationBase;

        /**
         * Reply with an incremented 'request.i'.
         */
        void run(OperationContext* opCtx, CommandReplyBuilder* reply) override {
            commands_test_example::ExampleIncrementReply r;
            r.setIPlusOne(request().getI() + 1);
            reply->fillFrom(r);
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     BSONObjBuilder* result) override {}

        void doCheckAuthorization(OperationContext*) const override {}

        /**
         * The ns() for when Request's IDL specifies "namespace: concatenate_with_db".
         */
        NamespaceString ns() const override {
            return request().getNamespace();
        }
    };
};

// Just like ExampleIncrementCommand, but with a void typedRun.
class ExampleVoidCommand final : public TypedCommand<ExampleVoidCommand> {
private:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Accepts Request and returns void.";
    }

public:
    using Request = commands_test_example::ExampleVoid;

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
                     BSONObjBuilder* result) override {}

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

template <typename Fn>
class MyCommand final : public TypedCommand<MyCommand<Fn>> {
public:
    class Invocation final : public TypedCommand<MyCommand>::InvocationBase {
    public:
        using Base = typename TypedCommand<MyCommand>::InvocationBase;
        using Base::Base;

        auto typedRun(OperationContext*) const {
            return _command()->_fn();
        }

    private:
        NamespaceString ns() const override {
            return Base::request().getNamespace();
        }
        bool supportsWriteConcern() const override {
            return false;
        }
        void doCheckAuthorization(OperationContext* opCtx) const override {}

        const MyCommand* _command() const {
            return static_cast<const MyCommand*>(Base::definition());
        }
    };

    using Request = commands_test_example::ExampleVoid;

    MyCommand(StringData name, Fn fn) : TypedCommand<MyCommand<Fn>>(name), _fn{std::move(fn)} {}

private:
    Command::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return "Accepts Request and returns void.";
    }

    Fn _fn;
};

template <typename Fn>
using CmdT = MyCommand<typename std::decay<Fn>::type>;

auto throwFn = [] { uasserted(ErrorCodes::UnknownError, "some error"); };

ExampleIncrementCommand exampleIncrementCommand;
ExampleMinimalCommand exampleMinimalCommand;
ExampleVoidCommand exampleVoidCommand;
CmdT<decltype(throwFn)> throwStatusCommand("throwsStatus", throwFn);

struct IncrementTestCommon {
    template <typename T>
    void run(T& command, std::function<void(int, const BSONObj&)> postAssert) {
        const NamespaceString ns("testdb.coll");
        auto client = getGlobalServiceContext()->makeClient("commands_test");
        for (std::int32_t i : {123, 12345, 0, -456}) {
            const OpMsgRequest request = [&] {
                typename T::Request incr(ns);
                incr.setI(i);
                return incr.serialize(BSON("$db" << ns.db()));
            }();
            auto opCtx = client->makeOperationContext();
            auto invocation = command.parse(opCtx.get(), request);

            ASSERT_EQ(invocation->ns(), ns);

            const BSONObj reply = [&] {
                BufBuilder bb;
                CommandReplyBuilder crb{BSONObjBuilder{bb}};
                try {
                    invocation->run(opCtx.get(), &crb);
                    auto bob = crb.getBodyBuilder();
                    CommandHelpers::extractOrAppendOk(bob);
                } catch (const DBException& e) {
                    auto bob = crb.getBodyBuilder();
                    CommandHelpers::appendCommandStatusNoThrow(bob, e.toStatus());
                }
                return BSONObj(bb.release());
            }();

            postAssert(i, reply);
        }
    }
};

TEST(TypedCommand, runTyped) {
    IncrementTestCommon{}.run(exampleIncrementCommand, [](int i, const BSONObj& reply) {
        ASSERT_EQ(reply["ok"].Double(), 1.0);
        ASSERT_EQ(reply["iPlusOne"].Int(), i + 1);
    });
}

TEST(TypedCommand, runMinimal) {
    IncrementTestCommon{}.run(exampleMinimalCommand, [](int i, const BSONObj& reply) {
        ASSERT_EQ(reply["ok"].Double(), 1.0);
        ASSERT_EQ(reply["iPlusOne"].Int(), i + 1);
    });
}

TEST(TypedCommand, runVoid) {
    IncrementTestCommon{}.run(exampleVoidCommand, [](int i, const BSONObj& reply) {
        ASSERT_EQ(reply["ok"].Double(), 1.0);
        ASSERT_EQ(exampleVoidCommand.iCapture, i + 1);
    });
}

TEST(TypedCommand, runThrowStatus) {
    IncrementTestCommon{}.run(throwStatusCommand, [](int i, const BSONObj& reply) {
        Status status = Status::OK();
        try {
            (void)throwFn();
        } catch (const DBException& e) {
            status = e.toStatus();
        }
        ASSERT_EQ(reply["ok"].Double(), 0.0);
        ASSERT_EQ(reply["errmsg"].String(), status.reason());
        ASSERT_EQ(reply["code"].Int(), status.code());
        ASSERT_EQ(reply["codeName"].String(), ErrorCodes::errorString(status.code()));
    });
}

}  // namespace
}  // namespace mongo
