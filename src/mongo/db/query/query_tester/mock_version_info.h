// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/version.h"

#include <string_view>

namespace mongo::query_tester {
class MockVersionInfo : public VersionInfoInterface {
public:
    int majorVersion() const noexcept final {
        return -1;
    }

    int minorVersion() const noexcept final {
        return 0;
    }

    int patchVersion() const noexcept final {
        return 0;
    }

    int extraVersion() const noexcept final {
        return 0;
    }

    std::string_view version() const noexcept final {
        return "UNKNOWN";
    }

    std::string_view gitVersion() const noexcept final {
        return "UNKNOWN";
    }

    std::vector<std::string_view> modules() const final {
        return {"mongoTester"};
    }

    std::string_view allocator() const noexcept final {
        return "UNKNOWN";
    }

    std::string_view jsEngine() const noexcept final {
        return "UNKNOWN";
    }

    std::string_view targetMinOS() const noexcept final {
        return "UNKNOWN";
    }

    std::vector<BuildInfoField> buildInfo() const final {
        return {};
    }
};
}  // namespace mongo::query_tester
