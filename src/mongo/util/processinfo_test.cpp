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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/util/processinfo.h"
#include "mongo/unittest/unittest.h"

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
            static char ptr[4096 * PAGES] = "This needs data to not be in .bss";
            ProcessInfo::blockInMemory(ptr + ProcessInfo::getPageSize() * 2);
        }
    }

    TEST(ProcessInfo, PagesInMemoryIsSensible) {
        if (ProcessInfo::blockCheckSupported()) {
            static char ptr[4096 * PAGES] = "This needs data to not be in .bss";
            ptr[(ProcessInfo::getPageSize() * 0) + 1] = 'a';
            ptr[(ProcessInfo::getPageSize() * 8) + 1] = 'a';
            std::vector<char> result;
            ASSERT_TRUE(ProcessInfo::pagesInMemory(const_cast<char*>(ptr), PAGES, &result));
            ASSERT_TRUE(result[0]);
            ASSERT_TRUE(result[8]);
        }
    }
}
