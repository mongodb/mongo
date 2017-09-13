/**
 *    Copyright 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <boost/filesystem.hpp>
#include <fstream>
#include <ostream>

#include "mongo/db/storage/storage_engine_lock_file.h"
#include "mongo/platform/process_id.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

using std::string;
using mongo::unittest::TempDir;

using namespace mongo;

TEST(StorageEngineLockFileTest, UncleanShutdownNoExistingFile) {
    TempDir tempDir("StorageEngineLockFileTest_UncleanShutdownNoExistingFile");
    StorageEngineLockFile lockFile(tempDir.path());
    ASSERT_FALSE(lockFile.createdByUncleanShutdown());
}

TEST(StorageEngineLockFileTest, UncleanShutdownEmptyExistingFile) {
    TempDir tempDir("StorageEngineLockFileTest_UncleanShutdownEmptyExistingFile");
    {
        std::string filename(tempDir.path() + "/mongod.lock");
        std::ofstream(filename.c_str());
    }
    StorageEngineLockFile lockFile(tempDir.path());
    ASSERT_FALSE(lockFile.createdByUncleanShutdown());
}

TEST(StorageEngineLockFileTest, UncleanShutdownNonEmptyExistingFile) {
    TempDir tempDir("StorageEngineLockFileTest_UncleanShutdownNonEmptyExistingFile");
    {
        std::string filename(tempDir.path() + "/mongod.lock");
        std::ofstream ofs(filename.c_str());
        ofs << 12345 << std::endl;
    }
    StorageEngineLockFile lockFile(tempDir.path());
    ASSERT_TRUE(lockFile.createdByUncleanShutdown());
}

TEST(StorageEngineLockFileTest, OpenInvalidDirectory) {
    StorageEngineLockFile lockFile("no_such_directory");
    ASSERT_EQUALS((boost::filesystem::path("no_such_directory") / "mongod.lock").string(),
                  lockFile.getFilespec());
    Status status = lockFile.open();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::NonExistentPath, status.code());
}

// Cause ::open() to fail by providing a regular file instead of a directory for 'dbpath'.
TEST(StorageEngineLockFileTest, OpenInvalidFilename) {
    TempDir tempDir("StorageEngineLockFileTest_OpenInvalidFilename");
    std::string filename(tempDir.path() + "/some_file");
    std::ofstream(filename.c_str());
    StorageEngineLockFile lockFile(filename);
    Status status = lockFile.open();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::DBPathInUse, status.code());
}

TEST(StorageEngineLockFileTest, OpenNoExistingLockFile) {
    TempDir tempDir("StorageEngineLockFileTest_OpenNoExistingLockFile");
    StorageEngineLockFile lockFile(tempDir.path());
    ASSERT_OK(lockFile.open());
    lockFile.close();
}

TEST(StorageEngineLockFileTest, OpenEmptyLockFile) {
    TempDir tempDir("StorageEngineLockFileTest_OpenEmptyLockFile");
    StorageEngineLockFile lockFile(tempDir.path());
    std::string filename(lockFile.getFilespec());
    std::ofstream(filename.c_str());
    ASSERT_OK(lockFile.open());
    lockFile.close();
}

TEST(StorageEngineLockFileTest, WritePidFileNotOpened) {
    TempDir tempDir("StorageEngineLockFileTest_WritePidFileNotOpened");
    StorageEngineLockFile lockFile(tempDir.path());
    Status status = lockFile.writePid();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::FileNotOpen, status.code());
}

TEST(StorageEngineLockFileTest, WritePidFileOpened) {
    TempDir tempDir("StorageEngineLockFileTest_WritePidFileOpened");
    StorageEngineLockFile lockFile(tempDir.path());
    ASSERT_OK(lockFile.open());
    ASSERT_OK(lockFile.writePid());
    lockFile.close();

    // Read PID from lock file.
    std::string filename(lockFile.getFilespec());
    std::ifstream ifs(filename.c_str());
    int64_t pidFromLockFile = 0;
    ASSERT_TRUE(ifs >> pidFromLockFile);
    ASSERT_EQUALS(ProcessId::getCurrent().asInt64(), pidFromLockFile);
}

// Existing data in lock file must be removed before writing process ID.
TEST(StorageEngineLockFileTest, WritePidTruncateExistingFile) {
    TempDir tempDir("StorageEngineLockFileTest_WritePidTruncateExistingFile");
    StorageEngineLockFile lockFile(tempDir.path());
    {
        std::string filename(tempDir.path() + "/mongod.lock");
        std::ofstream ofs(filename.c_str());
        std::string currentPidStr = ProcessId::getCurrent().toString();
        ASSERT_FALSE(currentPidStr.empty());
        ofs << std::string(currentPidStr.size() * 100, 'X') << std::endl;
    }
    ASSERT_OK(lockFile.open());
    ASSERT_OK(lockFile.writePid());
    lockFile.close();

    // Read PID from lock file.
    std::string filename(lockFile.getFilespec());
    std::ifstream ifs(filename.c_str());
    int64_t pidFromLockFile = 0;
    ASSERT_TRUE(ifs >> pidFromLockFile);
    ASSERT_EQUALS(ProcessId::getCurrent().asInt64(), pidFromLockFile);

    // There should not be any data in the file after the process ID.
    std::string extraData;
    ASSERT_FALSE(ifs >> extraData);
}

TEST(StorageEngineLockFileTest, ClearPidAndUnlock) {
    TempDir tempDir("StorageEngineLockFileTest_ClearPidAndUnlock");
    StorageEngineLockFile lockFile(tempDir.path());
    ASSERT_OK(lockFile.open());
    ASSERT_OK(lockFile.writePid());

    // Clear lock file contents.
    lockFile.clearPidAndUnlock();
    ASSERT_TRUE(boost::filesystem::exists(lockFile.getFilespec()));
    ASSERT_EQUALS(0U, boost::filesystem::file_size(lockFile.getFilespec()));
}

class ScopedReadOnlyDirectory {
public:
    ScopedReadOnlyDirectory(const std::string& path) : _path(std::move(path)) {
        _applyToPathRecursive(_path, makePathReadOnly);
    }

    ~ScopedReadOnlyDirectory() {
        _applyToPathRecursive(_path, makePathWritable);
    }

private:
    const std::string& _path;

    static void makePathReadOnly(const boost::filesystem::path& path) {
#ifdef _WIN32
        ::SetFileAttributes(path.c_str(), FILE_ATTRIBUTE_READONLY);
#else
        ::chmod(path.c_str(), 0544);
#endif
    }

    static void makePathWritable(const boost::filesystem::path& path) {
#ifdef _WIN32
        ::SetFileAttributes(path.c_str(), FILE_ATTRIBUTE_NORMAL);
#else
        ::chmod(path.c_str(), 0777);
#endif
    }

    template <typename Func>
    static void _applyToPathRecursive(const boost::filesystem::path& path, Func func) {
        func(path);

        using rdi = boost::filesystem::recursive_directory_iterator;
        for (auto iter = rdi{path}; iter != rdi(); ++iter) {
            func(*iter);
        }
    }
};

#ifndef _WIN32

// Windows has no concept of read only directories - only read only files.
TEST(StorageEngineLockFileTest, ReadOnlyDirectory) {
    // If we are running as root, do not run this test as read only permissions will not be
    // respected.
    if (::getuid() == 0) {
        return;
    }

    TempDir tempDir("StorageEngineLockFileTest_ReadOnlyDirectory");

    // Make tempDir read-only.
    ScopedReadOnlyDirectory srod(tempDir.path());

    StorageEngineLockFile lockFile(tempDir.path());

    auto openStatus = lockFile.open();

    ASSERT_NOT_OK(openStatus);
    ASSERT_EQ(openStatus, ErrorCodes::IllegalOperation);
}

#endif

TEST(StorageEngineLockFileTest, ReadOnlyDirectoryWithLockFile) {
#ifndef _WIN32
    // If we are running as root, do not run this test as read only permissions will not be
    // respected.
    if (::getuid() == 0) {
        return;
    }
#endif

    TempDir tempDir("StorageEngineLockFileTest_ReadOnlyDirectoryWithLockFile");


    StorageEngineLockFile lockFile(tempDir.path());
    ASSERT_OK(lockFile.open());
    ASSERT_OK(lockFile.writePid());

    // Make tempDir read-only.
    ScopedReadOnlyDirectory srod(tempDir.path());

    // Try to create a new lock file.
    StorageEngineLockFile lockFile2(tempDir.path());

    auto openStatus = lockFile2.open();

    ASSERT_NOT_OK(openStatus);
    ASSERT_EQ(openStatus, ErrorCodes::IllegalOperation);
}

}  // namespace
