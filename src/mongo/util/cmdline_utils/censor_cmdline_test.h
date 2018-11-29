/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"

namespace mongo {
namespace test {

inline void censoringArgv(const std::vector<std::string>& expected,
                          const std::vector<std::string>& toCensor) {
    ASSERT_EQ(expected.size(), toCensor.size());

    // Make a local copy we can mutate.
    std::vector<std::string> localCensor = toCensor;
    std::vector<char*> arrayStandin;
    for (auto& censor : localCensor) {
        arrayStandin.push_back(&*censor.begin());
    }

    char** argv = &*arrayStandin.begin();
    cmdline_utils::censorArgvArray(arrayStandin.size(), argv);

    for (size_t i = 0; i < expected.size(); ++i) {
        ASSERT_EQ(expected[i], std::string(argv[i], localCensor[i].size()));
    }
}

inline void censoringVector(const std::vector<std::string>& expected,
                            const std::vector<std::string>& toCensor) {
    ASSERT_EQ(expected.size(), toCensor.size());

    // Make a local copy we can mutate.
    std::vector<std::string> localCensor = toCensor;

    cmdline_utils::censorArgsVector(&localCensor);

    for (size_t i = 0; i < expected.size(); ++i) {
        ASSERT_EQUALS(expected[i], localCensor[i]);
    }
}

}  // namespace test
}  // namespace mongo
