// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"

#include <string>
#include <vector>

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
