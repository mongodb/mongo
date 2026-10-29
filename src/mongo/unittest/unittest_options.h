// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/util/exit_code.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/optional.hpp>

namespace mongo::unittest {

struct UnitTestOptions {
    boost::optional<bool> help;
    boost::optional<bool> list;
    boost::optional<std::vector<std::string>> suites;
    boost::optional<std::string> filter;
    boost::optional<std::string> fileNameFilter;
    boost::optional<int> repeat;
    boost::optional<std::string> verbose;
    boost::optional<std::string> tempPath;
    boost::optional<bool> autoUpdateAsserts;
    boost::optional<bool> rewriteAllAutoAsserts;
    boost::optional<bool> enhancedReporter;
    boost::optional<bool> showEachTest;
};

UnitTestOptions parseUnitTestOptions(std::vector<std::string>& argVec);
std::string getUnitTestOptionsHelpString(std::vector<std::string>& argVec);

}  // namespace mongo::unittest
