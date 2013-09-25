/**
 *    Copyright (C) 2013 10gen Inc.
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
 */

#include "mongo/platform/basic.h"

#include "mongo/unittest/temp_dir.h"

#include <fstream>
#include <boost/filesystem.hpp>

#include "mongo/unittest/unittest.h"

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
