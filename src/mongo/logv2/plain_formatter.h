// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/logv2/constants.h"
#include "mongo/logv2/log_format.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <cstdint>

#include <boost/log/core/record_view.hpp>
#include <boost/log/utility/formatting_ostream_fwd.hpp>
#include <fmt/format.h>

namespace mongo::logv2 {

// Text formatter without metadata. Just contains the formatted message.
class [[MONGO_MOD_OPEN]] PlainFormatter {
public:
    PlainFormatter(const Atomic<int32_t>* maxAttributeSizeKB = nullptr)
        : _maxAttributeSizeKB(maxAttributeSizeKB) {}

    void operator()(boost::log::record_view const& rec, fmt::memory_buffer& buffer) const;
    void operator()(boost::log::record_view const& rec, boost::log::formatting_ostream& strm) const;

private:
    const Atomic<int32_t>* _maxAttributeSizeKB;
};

}  // namespace mongo::logv2
