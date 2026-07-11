// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/unittest/temp_dir.h"

#include "mongo/unittest/unittest.h"

#include <fstream>  // IWYU pragma: keep

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

using mongo::unittest::TempDir;

TEST(TempDirTests, CreatesDir) {
    TempDir tempDir("tempDirTests");
    ASSERT(boost::filesystem::exists(tempDir.path()));
}

TEST(TempDirTests, DeletesDir) {
    boost::filesystem::path path;
    {
        TempDir tempDir("tempDirTests");
        path = tempDir.path();
        ASSERT(boost::filesystem::exists(path));
    }
    ASSERT(!boost::filesystem::exists(path));
}

TEST(TempDirTests, DeletesDirContents) {
    boost::filesystem::path tempDirPath;
    boost::filesystem::path filePath;
    {
        TempDir tempDir("tempDirTests");
        tempDirPath = tempDir.path();
        filePath = tempDirPath / "a_file";

        std::ofstream(filePath.string().c_str()) << "some data";

        ASSERT(boost::filesystem::exists(filePath));
    }
    ASSERT(!boost::filesystem::exists(filePath));
    ASSERT(!boost::filesystem::exists(tempDirPath));
}

TEST(TempDirTests, DeletesNestedDirContents) {
    boost::filesystem::path tempDirPath;
    boost::filesystem::path nestedDirPath;
    boost::filesystem::path filePath;
    {
        TempDir tempDir("tempDirTests");
        tempDirPath = tempDir.path();
        nestedDirPath = tempDirPath / "a_directory";
        filePath = nestedDirPath / "a_file";

        boost::filesystem::create_directory(nestedDirPath);
        std::ofstream(filePath.string().c_str()) << "some data";

        ASSERT(boost::filesystem::exists(filePath));
    }
    ASSERT(!boost::filesystem::exists(filePath));
    ASSERT(!boost::filesystem::exists(nestedDirPath));
    ASSERT(!boost::filesystem::exists(tempDirPath));
}
