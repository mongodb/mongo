// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_diagnostic_printer.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log_util.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;


static constexpr std::string_view kCmdName = "mockCmd"sv;
static constexpr std::string_view kCmdValue = "abcdefgh"sv;
static constexpr std::string_view kSensitiveFieldName = "sensitive"sv;
static constexpr std::string_view kSensitiveValue = "12345678"sv;

class MockCmd : public BasicCommand {
public:
    MockCmd() : BasicCommand{kCmdName} {}

    std::set<std::string_view> sensitiveFieldNames() const final {
        return {kSensitiveFieldName};
    }

    bool run(OperationContext*, const DatabaseName&, const BSONObj&, BSONObjBuilder&) override {
        return true;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    bool supportsWriteConcern(const BSONObj&) const override {
        return true;
    }
    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();
    }

    bool enableDiagnosticPrintingOnFailure() const override {
        return true;
    }
};

class DiagnosticPrinterTest : public ServiceContextTest {
public:
    DiagnosticPrinterTest() {
        _nss = NamespaceString::createNamespaceString_forTest("myDB.myColl");
        _opCtxHolder = makeOperationContext();
        _cmdBson = BSON(kCmdName << kCmdValue << kSensitiveFieldName << kSensitiveValue);
    }

    OperationContext* opCtx() {
        return _opCtxHolder.get();
    }

    CurOp* curOp() {
        return CurOp::get(opCtx());
    }

    void setMockCmdOnCurOp() {
        std::lock_guard<Client> clientLock(*opCtx()->getClient());
        curOp()->setGenericOpRequestDetails(clientLock, _nss, &_cmd, _cmdBson, NetworkOp::dbQuery);
    }

    void setCmdOnCurOp(Command* cmdObj, const BSONObj& cmdBson) {
        std::lock_guard<Client> clientLock(*opCtx()->getClient());
        curOp()->setGenericOpRequestDetails(clientLock, _nss, cmdObj, cmdBson, NetworkOp::dbQuery);
    }

    std::string printCurOpDiagnostics() {
        diagnostic_printers::CurOpPrinter printer{opCtx()};
        return fmt::format("{}", printer);
    }

    MockCmd _cmd;
    NamespaceString _nss;
    ServiceContext::UniqueOperationContext _opCtxHolder;
    BSONObj _cmdBson;
};

TEST_F(DiagnosticPrinterTest, IsIneligibleForPrintingWhenOpCtxIsNull) {
    ASSERT_EQ(diagnostic_printers::kOpCtxIsNullMsg,
              diagnostic_printers::isIneligibleForDiagnosticPrinting(nullptr));
}

TEST_F(DiagnosticPrinterTest, IsIneligibleForPrintingWhenCurOpIsIneligible) {
    setMockCmdOnCurOp();
    {
        std::lock_guard<Client> clientLock(*opCtx()->getClient());
        curOp()->setShouldOmitDiagnosticInformation(clientLock, true);
    }
    ASSERT_EQ(diagnostic_printers::kOmitUnsupportedCurOpMsg,
              diagnostic_printers::isIneligibleForDiagnosticPrinting(opCtx()));
}

TEST_F(DiagnosticPrinterTest, IsIneligibleForPrintingWhenThereIsNoCommandSet) {
    // When no command is set, it's not clear if any of the command fields are sensitive.
    ASSERT_EQ(diagnostic_printers::kOmitUnrecognizedCommandMsg,
              diagnostic_printers::isIneligibleForDiagnosticPrinting(opCtx()));
}

TEST_F(DiagnosticPrinterTest, IsEligibleWhenCommandHasSensitiveFields) {
    setMockCmdOnCurOp();
    ASSERT_EQ(boost::none, diagnostic_printers::isIneligibleForDiagnosticPrinting(opCtx()));
}

TEST_F(DiagnosticPrinterTest, IsEligibleWhenRedactionEnabled) {
    setMockCmdOnCurOp();
    logv2::setShouldRedactLogs(true);
    ASSERT_EQ(boost::none, diagnostic_printers::isIneligibleForDiagnosticPrinting(opCtx()));

    // Reset at the end of the test to not affect other test cases.
    logv2::setShouldRedactLogs(false);
}

TEST_F(DiagnosticPrinterTest, IsIneligibleWhenCommandDoesNotEnableDiagnosticPrinting) {
    class MockCmdWithoutDiagnosticPrinting : public MockCmd {
        bool enableDiagnosticPrintingOnFailure() const final {
            return false;
        }
    };

    MockCmdWithoutDiagnosticPrinting cmdWithoutPrinting;
    BSONObj mockBson = BSON("mockCmd" << 1);
    setCmdOnCurOp(&cmdWithoutPrinting, mockBson);
    ASSERT_EQ(diagnostic_printers::kOmitUnsupportedCommandMsg,
              diagnostic_printers::isIneligibleForDiagnosticPrinting(opCtx()));
}

TEST_F(DiagnosticPrinterTest, PrinterOmitsCommandFieldsWhenThereIsNoCommandSet) {
    // When CurOp doesn't have a command object on it, the diagnostic printer shouldn't log any
    // command fields, since it's unclear if any of them are sensitive.
    ASSERT_EQ(diagnostic_printers::kOmitUnrecognizedCommandMsg, printCurOpDiagnostics());
}

TEST_F(DiagnosticPrinterTest, PrinterOmitsAllFieldsWhenRequested) {
    // When a command requests to omit diagnostic logging, the diagnostic printer shouldn't log any
    // fields.
    setMockCmdOnCurOp();
    {
        std::lock_guard<Client> clientLock(*opCtx()->getClient());
        curOp()->setShouldOmitDiagnosticInformation(clientLock, true);
    }
    ASSERT_EQ(diagnostic_printers::kOmitUnsupportedCurOpMsg, printCurOpDiagnostics());
}

TEST_F(DiagnosticPrinterTest, PrinterRedactsSensitiveCommandFields) {
    // The diagnostic printer should always redact the values of fields specified as sensitive by
    // the command.
    setMockCmdOnCurOp();
    auto str = printCurOpDiagnostics();
    ASSERT_STRING_CONTAINS(str, kCmdName);
    ASSERT_STRING_CONTAINS(str, kCmdValue);
    ASSERT_STRING_CONTAINS(str, kSensitiveFieldName);
    ASSERT_STRING_OMITS(str, kSensitiveValue);
}

TEST_F(DiagnosticPrinterTest, PrinterRedactsWhenRedactionIsEnabled) {
    // When redaction is enabled, all field values should be redacted.
    setMockCmdOnCurOp();
    logv2::setShouldRedactLogs(true);
    auto str = printCurOpDiagnostics();
    ASSERT_STRING_CONTAINS(str, kCmdName);
    ASSERT_STRING_OMITS(str, kCmdValue);
    ASSERT_STRING_CONTAINS(str, kSensitiveFieldName);
    ASSERT_STRING_OMITS(str, kSensitiveValue);

    // Reset at the end of the test to not affect other test cases.
    logv2::setShouldRedactLogs(false);
}

TEST_F(DiagnosticPrinterTest, OmitsAllFieldsWhenCommandDoesNotEnableDiagnosticPrinting) {
    class MockCmdWithoutDiagnosticPrinting : public MockCmd {
        bool enableDiagnosticPrintingOnFailure() const final {
            return false;
        }
    };

    MockCmdWithoutDiagnosticPrinting cmdWithoutPrinting;
    BSONObj mockBson = BSON("mockCmd" << 1);
    setCmdOnCurOp(&cmdWithoutPrinting, mockBson);
    ASSERT_EQ(diagnostic_printers::kOmitUnsupportedCommandMsg, printCurOpDiagnostics());
}

TEST_F(DiagnosticPrinterTest, FormattingGracefullyExitsWhenOpCtxIsNull) {
    diagnostic_printers::CurOpPrinter printer{nullptr};
    ASSERT_EQ(diagnostic_printers::kOpCtxIsNullMsg, fmt::format("{}", printer));
}

TEST_F(DiagnosticPrinterTest, CreateIndexCommandIsEligibleForDiagnosticLog) {
    auto command = CommandHelpers::findCommand(opCtx(), "createIndexes");
    auto createIndexesReq =
        BSON("createIndexes" << _nss.coll() << "indexes"
                             << BSON_ARRAY(BSON("key" << BSON("a" << 1) << "partialFilterExpression"
                                                      << BSON("b" << 1))));

    // Prove that the command BSON is appropriate for this command (parsing succeeds).
    auto request = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx()), _nss.dbName(), createIndexesReq);
    ASSERT_NOT_EQUALS(command->parse(opCtx(), request), nullptr);

    // Diagnostics log includes the entire command BSON (command name, namespace, and index spec).
    setCmdOnCurOp(command, createIndexesReq);
    auto str = printCurOpDiagnostics();
    ASSERT_STRING_CONTAINS(str, createIndexesReq.toString());
}
}  // namespace

}  // namespace mongo
