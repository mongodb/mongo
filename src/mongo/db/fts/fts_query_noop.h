// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/fts/fts_query.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {
namespace fts {

/**
 * A no-op implementation of FTSQuery.
 */
class FTSQueryNoop final : public FTSQuery {
public:
    Status parse(TextIndexVersion textIndexVersion) final {
        return Status::OK();
    }

    std::unique_ptr<FTSQuery> clone() const final;
};

}  // namespace fts
}  // namespace mongo
