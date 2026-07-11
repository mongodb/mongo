// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/op_msg_fuzzer_shard_fixture.h"

#include <boost/log/core/core.hpp>

extern "C" int LLVMFuzzerTestOneInput(const char* Data, size_t Size) {
    static auto fixture = []() {
        auto core = boost::log::core::get();
        core->set_logging_enabled(false);
        return mongo::OpMsgFuzzerShardFixture();
    }();
    return fixture.testOneInput(Data, Size);
}
