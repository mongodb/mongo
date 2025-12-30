/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#pragma once

#include "mongo/unittest/unittest_options.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

#include <boost/optional.hpp>

namespace mongo::unittest {

struct SelectedTest {
    const char* suite;
    const char* test;
    bool keep;
};
std::string gtestFilterForSelection(const std::vector<SelectedTest>& selection);

/** Config for `testMain`. */
struct MONGO_MOD_PUBLIC MainOptions {
    bool startSignalProcessingThread = true;
    bool suppressGlobalInitializers = false;

    /** Overrides if engaged */
    boost::optional<std::vector<std::string>> testSuites;
    boost::optional<std::string> testFilter;
    boost::optional<std::string> fileNameFilter;
    boost::optional<int> runsPerTest;
};

/**
 * Tracks the state that must be carried among unit test program execution steps.
 */
class MONGO_MOD_PUBLIC MainProgress {
public:
    MainProgress(MainOptions options, std::vector<std::string> argVec)
        : _options{std::move(options)}, _argVec{std::move(argVec)} {}

    /**
     * Parses and removes flags relevant to test framework.
     * Call this at the top of main and then call `parseAndAcceptOptions` later.
     */
    void initialize();

    /**
     * The `uto` options are validated and take effect within this function.
     * The `argVec` should have already been parsed to produce `uto`. It isn't used
     * except for possible user-friendly messaging.
     * Returns an exit code if the program should immediately end.
     */
    boost::optional<ExitCode> parseAndAcceptOptions();

    /** Call `initialize` and `parseAndAcceptOptions` before calling this. */
    int test();

    std::vector<std::string>& args() {
        return _argVec;
    }

private:
    MainOptions _options;
    std::vector<std::string> _argVec;
};

}  // namespace mongo::unittest
