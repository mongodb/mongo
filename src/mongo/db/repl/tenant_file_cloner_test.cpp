/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/repl/tenant_cloner_test_fixture.h"
#include "mongo/db/repl/tenant_file_cloner.h"
#include "mongo/db/repl/tenant_migration_shard_merge_util.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo::repl {

class MockCallbackState final : public mongo::executor::TaskExecutor::CallbackState {
public:
    MockCallbackState() = default;
    void cancel() override {}
    void waitForCompletion() override {}
    bool isCanceled() const override {
        return false;
    }
};

class TenantFileClonerTest : public TenantClonerTestFixture {
public:
    TenantFileClonerTest() : _backupId(UUID::gen()), _migrationId(UUID::gen()) {}

protected:
    void setUp() override {
        TenantClonerTestFixture::setUp();
        _destinationClonePath = shard_merge_utils::fileClonerTempDir(_migrationId);
    }
    std::unique_ptr<TenantFileCloner> maketenantFileCloner(const std::string& remoteFileName,
                                                           const std::string& relativePath,
                                                           size_t remoteFileSize) {
        return std::make_unique<TenantFileCloner>(_backupId,
                                                  _migrationId,
                                                  remoteFileName,
                                                  remoteFileSize,
                                                  relativePath,
                                                  getSharedData(),
                                                  _source,
                                                  _mockClient.get(),
                                                  &_storageInterface,
                                                  _dbWorkThreadPool.get());
    }

    const UUID _backupId;
    const UUID _migrationId;
    boost::filesystem::path _destinationClonePath;

private:
    unittest::MinimumLoggedSeverityGuard replLogSeverityGuard{logv2::LogComponent::kTenantMigration,
                                                              logv2::LogSeverity::Debug(3)};
};

TEST_F(TenantFileClonerTest, RelativePathIsnt) {
    // current_path() is specified to be absolute; using it avoids the need to hardcode a path
    // which would need to be different between Windows and Unix.
    auto absolutePath = boost::filesystem::current_path();
    auto tenantFileCloner =
        maketenantFileCloner("/path/to/backupfile", absolutePath.generic_string(), 0);
    auto status = tenantFileCloner->run();
    ASSERT_EQ(status.code(), 6113300) << status;
}

TEST_F(TenantFileClonerTest, RelativePathEscapes) {
    auto tenantFileCloner = maketenantFileCloner("/path/to/backupfile", "../escapee", 0);
    auto status = tenantFileCloner->run();
    ASSERT_EQ(status.code(), 6113301) << status;
}

TEST_F(TenantFileClonerTest, CantOpenFile) {
    // File can't be opened because it's actually a directory.
    auto tenantFileCloner = maketenantFileCloner("/path/to/backupfile", "dir/badfile", 0);
    boost::filesystem::create_directory(_destinationClonePath);
    auto badfilePath = _destinationClonePath;
    badfilePath.append("dir/badfile");
    boost::filesystem::create_directories(badfilePath);
    auto status = tenantFileCloner->run();
    ASSERT_EQ(status.code(), ErrorCodes::FileOpenFailed) << status;
}

TEST_F(TenantFileClonerTest, CantCreateDirectory) {
    // Directory can't be created because it's a file already.
    auto tenantFileCloner = maketenantFileCloner("/path/to/backupfile", "baddir/file", 0);
    boost::filesystem::create_directory(_destinationClonePath);
    auto baddirPath = _destinationClonePath;
    baddirPath.append("baddir");
    std::ofstream baddirFile(baddirPath.native(), std::ios_base::out | std::ios_base::trunc);
    auto status = tenantFileCloner->run();
    ASSERT_EQ(status.code(), 6113303) << status;
}

TEST_F(TenantFileClonerTest, PreStageSuccess) {
    // First test with a file and directory which don't exist.
    auto tenantFileCloner = maketenantFileCloner("/path/to/backupfile", "dir/file", 0);
    tenantFileCloner->setStopAfterStage_forTest("preStage");
    boost::filesystem::create_directory(_destinationClonePath);
    auto filePath = _destinationClonePath;
    filePath.append("dir/file");
    ASSERT_OK(tenantFileCloner->run());
    ASSERT(boost::filesystem::exists(filePath));
    ASSERT_EQ(0, boost::filesystem::file_size(filePath));

    // Now that it exists, test that it is truncated if we try again.
    std::ofstream file(filePath.native());
    file.write("stuff", 5);
    file.close();
    ASSERT_EQ(5, boost::filesystem::file_size(filePath));

    tenantFileCloner = maketenantFileCloner("/path/to/backupfile", "dir/file", 5);
    tenantFileCloner->setStopAfterStage_forTest("preStage");

    ASSERT_OK(tenantFileCloner->run());
    ASSERT(boost::filesystem::exists(filePath));
    // File should be truncated.
    ASSERT_EQ(0, boost::filesystem::file_size(filePath));
}

TEST_F(TenantFileClonerTest, EmptyFile) {
    auto absolutePath = boost::filesystem::current_path();
    auto tenantFileCloner = maketenantFileCloner("/path/dir/backupfile", "dir/backupfile", 0);
    CursorResponse response(
        NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
        0 /* cursorId */,
        {BSON("byteOffset" << 0 << "endOfFile" << true << "data" << BSONBinData())});
    _mockServer->setCommandReply("aggregate", response.toBSONAsInitialResponse());
    auto filePath = _destinationClonePath;
    filePath.append("dir/backupfile");
    ASSERT_OK(tenantFileCloner->run());
    ASSERT(boost::filesystem::exists(filePath));
    ASSERT_EQ(0, boost::filesystem::file_size(filePath));

    BSONObj stats = tenantFileCloner->getStats().toBSON();

    ASSERT_EQ("dir/backupfile", stats["filePath"].str());
    ASSERT(stats["fileSize"].isNumber());
    ASSERT(stats["bytesCopied"].isNumber());
    ASSERT_EQ(0, stats["fileSize"].numberLong());
    ASSERT_EQ(0, stats["bytesCopied"].numberLong());
    // The empty batch counts as a batch.
    ASSERT_EQ(1, stats["receivedBatches"].numberLong());
    ASSERT_EQ(1, stats["writtenBatches"].numberLong());
}

TEST_F(TenantFileClonerTest, NoEOF) {
    auto absolutePath = boost::filesystem::current_path();
    auto tenantFileCloner = maketenantFileCloner("/path/dir/backupfile", "dir/backupfile", 0);
    CursorResponse response(NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
                            0 /* cursorId */,
                            {BSON("byteOffset" << 0 << "data" << BSONBinData())});
    _mockServer->setCommandReply("aggregate", response.toBSONAsInitialResponse());
    auto filePath = _destinationClonePath;
    filePath.append("dir/backupfile");
    auto status = tenantFileCloner->run();
    ASSERT_EQ(status.code(), 6113304) << status;
}

TEST_F(TenantFileClonerTest, SingleBatch) {
    auto absolutePath = boost::filesystem::current_path();
    std::string fileData = "The slow green fox\n takes\r a nap\0 next to the lazy dog.";
    auto tenantFileCloner =
        maketenantFileCloner("/path/dir/backupfile", "dir/backupfile", fileData.size());
    auto bindata = BSONBinData(fileData.data(), fileData.size(), BinDataGeneral);
    CursorResponse response(NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
                            0 /* cursorId */,
                            {BSON("byteOffset" << 0 << "endOfFile" << true << "data" << bindata)});
    _mockServer->setCommandReply("aggregate", response.toBSONAsInitialResponse());
    auto filePath = _destinationClonePath;
    filePath.append("dir/backupfile");
    ASSERT_OK(tenantFileCloner->run());
    ASSERT(boost::filesystem::exists(filePath));
    ASSERT_EQ(fileData.size(), boost::filesystem::file_size(filePath));
    std::string actualFileData(fileData.size(), 0);
    std::ifstream checkStream(filePath.string(), std::ios_base::in | std::ios_base::binary);
    checkStream.read(actualFileData.data(), fileData.size());
    ASSERT_EQ(fileData, actualFileData);

    BSONObj stats = tenantFileCloner->getStats().toBSON();

    ASSERT_EQ("dir/backupfile", stats["filePath"].str());
    ASSERT_EQ(fileData.size(), stats["fileSize"].numberLong());
    ASSERT_EQ(fileData.size(), stats["bytesCopied"].numberLong());
    ASSERT_EQ(1, stats["receivedBatches"].numberLong());
    ASSERT_EQ(1, stats["writtenBatches"].numberLong());
}

TEST_F(TenantFileClonerTest, Multibatch) {
    auto absolutePath = boost::filesystem::current_path();
    std::string fileData = "ABCDEFGHJIKLMNOPQRST0123456789";
    auto tenantFileCloner =
        maketenantFileCloner("/path/dir/backupfile", "dir/backupfile", fileData.size());
    auto batch1bindata = BSONBinData(fileData.data(), 20, BinDataGeneral);
    auto batch2bindata = BSONBinData(fileData.data() + 20, fileData.size() - 20, BinDataGeneral);
    CursorResponse batch1response(
        NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
        1 /* cursorId */,
        {BSON("byteOffset" << 0 << "data" << batch1bindata)});
    CursorResponse batch2response(
        NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
        0 /* cursorId */,
        {BSON("byteOffset" << 20 << "endOfFile" << true << "data" << batch2bindata)});
    _mockServer->setCommandReply(
        "aggregate",
        {batch1response.toBSONAsInitialResponse(),
         batch2response.toBSON(CursorResponse::ResponseType::SubsequentResponse),
         Status(ErrorCodes::UnknownError, "This should never be seen")});
    auto filePath = _destinationClonePath;
    filePath.append("dir/backupfile");
    ASSERT_OK(tenantFileCloner->run());
    ASSERT(boost::filesystem::exists(filePath));
    ASSERT_EQ(fileData.size(), boost::filesystem::file_size(filePath));
    std::string actualFileData(fileData.size(), 0);
    std::ifstream checkStream(filePath.string(), std::ios_base::in | std::ios_base::binary);
    checkStream.read(actualFileData.data(), fileData.size());
    ASSERT_EQ(fileData, actualFileData);

    BSONObj stats = tenantFileCloner->getStats().toBSON();

    ASSERT_EQ("dir/backupfile", stats["filePath"].str());
    ASSERT_EQ(fileData.size(), stats["fileSize"].numberLong());
    ASSERT_EQ(fileData.size(), stats["bytesCopied"].numberLong());
    ASSERT_EQ(2, stats["receivedBatches"].numberLong());
    ASSERT_EQ(2, stats["writtenBatches"].numberLong());
}

TEST_F(TenantFileClonerTest, RetryOnFirstBatch) {
    auto absolutePath = boost::filesystem::current_path();
    std::string fileData = "The slow green fox\n takes\r a nap\0 next to the lazy dog.";
    auto tenantFileCloner =
        maketenantFileCloner("/path/dir/backupfile", "dir/backupfile", fileData.size());
    auto bindata = BSONBinData(fileData.data(), fileData.size(), BinDataGeneral);
    CursorResponse response(NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
                            0 /* cursorId */,
                            {BSON("byteOffset" << 0 << "endOfFile" << true << "data" << bindata)});
    _mockServer->setCommandReply(
        "aggregate",
        {Status(ErrorCodes::HostUnreachable, "Retryable Error on first batch"),
         response.toBSONAsInitialResponse()});
    auto filePath = _destinationClonePath;
    filePath.append("dir/backupfile");
    ASSERT_OK(tenantFileCloner->run());
    ASSERT(boost::filesystem::exists(filePath));
    ASSERT_EQ(fileData.size(), boost::filesystem::file_size(filePath));
    std::string actualFileData(fileData.size(), 0);
    std::ifstream checkStream(filePath.string(), std::ios_base::in | std::ios_base::binary);
    checkStream.read(actualFileData.data(), fileData.size());
    ASSERT_EQ(fileData, actualFileData);

    BSONObj stats = tenantFileCloner->getStats().toBSON();

    ASSERT_EQ("dir/backupfile", stats["filePath"].str());
    ASSERT_EQ(fileData.size(), stats["fileSize"].numberLong());
    ASSERT_EQ(fileData.size(), stats["bytesCopied"].numberLong());
    ASSERT_EQ(1, stats["receivedBatches"].numberLong());
    ASSERT_EQ(1, stats["writtenBatches"].numberLong());
}

TEST_F(TenantFileClonerTest, RetryOnSubsequentBatch) {
    auto absolutePath = boost::filesystem::current_path();
    std::string fileData = "ABCDEFGHJIKLMNOPQRST0123456789";
    auto tenantFileCloner =
        maketenantFileCloner("/path/dir/backupfile", "dir/backupfile", fileData.size());
    auto batch1bindata = BSONBinData(fileData.data(), 20, BinDataGeneral);
    auto batch2bindata = BSONBinData(fileData.data() + 20, fileData.size() - 20, BinDataGeneral);
    CursorResponse batch1response(
        NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
        1 /* cursorId */,
        {BSON("byteOffset" << 0 << "data" << batch1bindata)});
    CursorResponse batch2response(
        NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
        0 /* cursorId */,
        {BSON("byteOffset" << 20 << "endOfFile" << true << "data" << batch2bindata)});
    _mockServer->setCommandReply(
        "aggregate",
        {batch1response.toBSONAsInitialResponse(),
         Status(ErrorCodes::HostUnreachable, "Retryable Error on second batch"),
         batch2response.toBSONAsInitialResponse(),
         Status(ErrorCodes::UnknownError, "This should never be seen")});
    boost::filesystem::create_directory(_destinationClonePath);
    auto filePath = _destinationClonePath;
    filePath.append("dir/backupfile");
    ASSERT_OK(tenantFileCloner->run());
    ASSERT(boost::filesystem::exists(filePath));
    ASSERT_EQ(fileData.size(), boost::filesystem::file_size(filePath));
    std::string actualFileData(fileData.size(), 0);
    std::ifstream checkStream(filePath.string(), std::ios_base::in | std::ios_base::binary);
    checkStream.read(actualFileData.data(), fileData.size());
    ASSERT_EQ(fileData, actualFileData);

    BSONObj stats = tenantFileCloner->getStats().toBSON();

    ASSERT_EQ("dir/backupfile", stats["filePath"].str());
    ASSERT_EQ(fileData.size(), stats["fileSize"].numberLong());
    ASSERT_EQ(fileData.size(), stats["bytesCopied"].numberLong());
    ASSERT_EQ(2, stats["receivedBatches"].numberLong());
    ASSERT_EQ(2, stats["writtenBatches"].numberLong());
}

TEST_F(TenantFileClonerTest, NonRetryableErrorFirstBatch) {
    auto absolutePath = boost::filesystem::current_path();
    auto tenantFileCloner = maketenantFileCloner("/path/dir/backupfile", "dir/backupfile", 0);
    _mockServer->setCommandReply(
        "aggregate", Status(ErrorCodes::IllegalOperation, "Non-retryable Error on first batch"));
    auto status = tenantFileCloner->run();
    ASSERT_EQ(status.code(), ErrorCodes::IllegalOperation) << status;
}

TEST_F(TenantFileClonerTest, NonRetryableErrorSubsequentBatch) {
    auto absolutePath = boost::filesystem::current_path();
    auto tenantFileCloner = maketenantFileCloner("/path/dir/backupfile", "dir/backupfile", 0);
    std::string fileData = "ABCDEFGHJIKLMNOPQRST0123456789";
    auto batch1bindata = BSONBinData(fileData.data(), 20, BinDataGeneral);
    auto batch2bindata = BSONBinData(fileData.data() + 20, fileData.size() - 20, BinDataGeneral);
    CursorResponse batch1response(
        NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
        1 /* cursorId */,
        {BSON("byteOffset" << 0 << "data" << batch1bindata)});
    _mockServer->setCommandReply(
        "aggregate",
        {batch1response.toBSONAsInitialResponse(),
         Status(ErrorCodes::IllegalOperation, "Non-retryable Error on second batch"),
         Status(ErrorCodes::UnknownError, "This should never be seen")});
    auto status = tenantFileCloner->run();
    ASSERT_EQ(status.code(), ErrorCodes::IllegalOperation) << status;
}

TEST_F(TenantFileClonerTest, NonRetryableErrorFollowsRetryableError) {
    // This scenario is expected when a sync source restarts and thus loses its backup cursor.
    auto absolutePath = boost::filesystem::current_path();
    auto tenantFileCloner = maketenantFileCloner("/path/dir/backupfile", "dir/backupfile", 0);
    std::string fileData = "ABCDEFGHJIKLMNOPQRST0123456789";
    auto batch1bindata = BSONBinData(fileData.data(), 20, BinDataGeneral);
    auto batch2bindata = BSONBinData(fileData.data() + 20, fileData.size() - 20, BinDataGeneral);
    CursorResponse batch1response(
        NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
        1 /* cursorId */,
        {BSON("byteOffset" << 0 << "data" << batch1bindata)});
    _mockServer->setCommandReply(
        "aggregate",
        {batch1response.toBSONAsInitialResponse(),
         Status(ErrorCodes::HostUnreachable, "Retryable Error on second batch"),
         Status(ErrorCodes::IllegalOperation, "Non-retryable Error on retry"),
         Status(ErrorCodes::UnknownError, "This should never be seen")});
    auto status = tenantFileCloner->run();
    ASSERT_EQ(status.code(), ErrorCodes::IllegalOperation) << status;
}

TEST_F(TenantFileClonerTest, InProgressStats) {
    auto absolutePath = boost::filesystem::current_path();
    std::string fileData = "ABCDEFGHJIKLMNOPQRST0123456789";
    auto tenantFileCloner =
        maketenantFileCloner("/path/dir/backupfile", "dir/backupfile", fileData.size());
    auto batch1bindata = BSONBinData(fileData.data(), 20, BinDataGeneral);
    auto batch2bindata = BSONBinData(fileData.data() + 20, fileData.size() - 20, BinDataGeneral);
    CursorResponse batch1response(
        NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
        1 /* cursorId */,
        {BSON("byteOffset" << 0 << "data" << batch1bindata)});
    CursorResponse batch2response(
        NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
        0 /* cursorId */,
        {BSON("byteOffset" << 20 << "endOfFile" << true << "data" << batch2bindata)});
    _mockServer->setCommandReply("aggregate",
                                 {batch1response.toBSONAsInitialResponse(),
                                  batch2response.toBSONAsInitialResponse(),
                                  Status(ErrorCodes::UnknownError, "This should never be seen")});
    boost::filesystem::create_directory(_destinationClonePath);
    auto filePath = _destinationClonePath;
    filePath.append("dir/backupfile");

    // Stats before running
    BSONObj stats = tenantFileCloner->getStats().toBSON();

    ASSERT_EQ("dir/backupfile", stats["filePath"].str());
    ASSERT_EQ(fileData.size(), stats["fileSize"].numberLong());
    ASSERT(stats["bytesCopied"].isNumber());
    ASSERT(stats["receivedBatches"].isNumber());
    ASSERT(stats["writtenBatches"].isNumber());
    ASSERT_EQ(0, stats["bytesCopied"].numberLong());
    ASSERT_EQ(0, stats["receivedBatches"].numberLong());
    ASSERT_EQ(0, stats["writtenBatches"].numberLong());
    ASSERT(stats["start"].eoo());
    ASSERT(stats["end"].eoo());

    stdx::thread TenantFileClonerThread;
    {
        // Pause both network and file-writing threads
        auto networkFailpoint =
            globalFailPointRegistry().find("TenantFileClonerHangAfterHandlingBatchResponse");
        auto fileWritingFailpoint =
            globalFailPointRegistry().find("TenantFileClonerHangDuringFileCloneBackup");
        auto fileWritingFailpointCount = fileWritingFailpoint->setMode(FailPoint::alwaysOn);
        auto networkFailpointCount = networkFailpoint->setMode(FailPoint::alwaysOn);

        // Stop after the query stage for one more stats check.
        FailPointEnableBlock clonerFailpoint("hangAfterClonerStage",
                                             BSON("cloner"
                                                  << "TenantFileCloner"
                                                  << "stage"
                                                  << "query"));
        // Run the cloner in another thread.
        TenantFileClonerThread = stdx::thread([&] {
            Client::initThread("TenantFileClonerRunner");
            ASSERT_OK(tenantFileCloner->run());
        });
        fileWritingFailpoint->waitForTimesEntered(Interruptible::notInterruptible(),
                                                  fileWritingFailpointCount + 1);
        networkFailpoint->waitForTimesEntered(Interruptible::notInterruptible(),
                                              networkFailpointCount + 1);

        // Stats after first batch.
        stats = tenantFileCloner->getStats().toBSON();

        ASSERT_EQ("dir/backupfile", stats["filePath"].str());
        ASSERT_EQ(fileData.size(), stats["fileSize"].numberLong());
        ASSERT_EQ(20, stats["bytesCopied"].numberLong());
        ASSERT_EQ(1, stats["receivedBatches"].numberLong());
        ASSERT_EQ(1, stats["writtenBatches"].numberLong());
        ASSERT_EQ(Date, stats["start"].type());
        ASSERT(stats["end"].eoo());

        fileWritingFailpoint->setMode(FailPoint::off);
        networkFailpoint->setMode(FailPoint::off);
        clonerFailpoint->waitForTimesEntered(Interruptible::notInterruptible(),
                                             clonerFailpoint.initialTimesEntered() + 1);

        // Stats after second batch.
        stats = tenantFileCloner->getStats().toBSON();

        ASSERT_EQ("dir/backupfile", stats["filePath"].str());
        ASSERT_EQ(fileData.size(), stats["fileSize"].numberLong());
        ASSERT_EQ(fileData.size(), stats["bytesCopied"].numberLong());
        ASSERT_EQ(2, stats["receivedBatches"].numberLong());
        ASSERT_EQ(2, stats["writtenBatches"].numberLong());
        ASSERT_EQ(Date, stats["start"].type());
        ASSERT(stats["end"].eoo());
    }

    TenantFileClonerThread.join();

    // Final stats.
    stats = tenantFileCloner->getStats().toBSON();

    ASSERT_EQ("dir/backupfile", stats["filePath"].str());
    ASSERT_EQ(fileData.size(), stats["fileSize"].numberLong());
    ASSERT_EQ(fileData.size(), stats["bytesCopied"].numberLong());
    ASSERT_EQ(2, stats["receivedBatches"].numberLong());
    ASSERT_EQ(2, stats["writtenBatches"].numberLong());
    ASSERT_EQ(Date, stats["start"].type());
    ASSERT_EQ(Date, stats["end"].type());
}

}  // namespace mongo::repl
