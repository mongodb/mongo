/**
 *    Copyright (C) 2012 10gen Inc.
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

#include <vector>

#include "boost/scoped_array.hpp"

#include "mongo/unittest/unittest.h"
#include "mongo/util/processinfo.h"

using mongo::ProcessInfo;

namespace mongo_test {
    TEST(ProcessInfo, SysInfoIsInitialized) {
        ProcessInfo processInfo;
        if (processInfo.supported()) {
            ASSERT_FALSE(processInfo.getOsType().empty());
        }
    }

    TEST(ProcessInfo, NonZeroPageSize) {
        if (ProcessInfo::blockCheckSupported()) {
            ASSERT_GREATER_THAN(ProcessInfo::getPageSize(), 0u);
        }
    }

    const size_t PAGES = 10;

    TEST(ProcessInfo, BlockInMemoryDoesNotThrowIfSupported) {
        if (ProcessInfo::blockCheckSupported()) {
            boost::scoped_array<char> ptr(new char[ProcessInfo::getPageSize() * PAGES]);
            ProcessInfo::blockInMemory(ptr.get() + ProcessInfo::getPageSize() * 2);
        }
    }

    TEST(ProcessInfo, PagesInMemoryIsSensible) {
        if (ProcessInfo::blockCheckSupported()) {
            static volatile char ptr[4096 * PAGES];
            ptr[1] = 'a';
            std::vector<char> result;
            ASSERT_TRUE(ProcessInfo::pagesInMemory(const_cast<char*>(ptr), PAGES, &result));
            ASSERT_TRUE(result[0]);
            ASSERT_FALSE(result[1]);
            ASSERT_FALSE(result[2]);
            ASSERT_FALSE(result[3]);
            ASSERT_FALSE(result[4]);
            ASSERT_FALSE(result[5]);
            ASSERT_FALSE(result[6]);
            ASSERT_FALSE(result[7]);
            ASSERT_FALSE(result[8]);
            ASSERT_FALSE(result[9]);
        }
    }
}
