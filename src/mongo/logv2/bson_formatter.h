// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/constants.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <cstdint>

#include <boost/log/core/record_view.hpp>
#include <boost/log/utility/formatting_ostream_fwd.hpp>

namespace mongo::logv2 {

class [[MONGO_MOD_NEEDS_REPLACEMENT]] BSONFormatter {
public:
    BSONFormatter(const Atomic<int32_t>* maxAttributeSizeKB = nullptr) {}

    void operator()(boost::log::record_view const& rec, BSONObjBuilder& builder) const;
    void operator()(boost::log::record_view const& rec, boost::log::formatting_ostream& strm) const;
    BSONObj operator()(boost::log::record_view const& rec) const;
};

}  // namespace mongo::logv2
