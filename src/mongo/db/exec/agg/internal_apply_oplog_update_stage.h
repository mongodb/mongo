// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

/**
 * This is an internal stage that takes an oplog update description and applies the update to the
 * input Document.
 */
class InternalApplyOplogUpdateStage final : public Stage {
public:
    InternalApplyOplogUpdateStage(std::string_view stageName,
                                  const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                  const BSONObj& oplogUpdate);

private:
    GetNextResult doGetNext() override;

    UpdateDriver _updateDriver;
};

}  // namespace mongo::exec::agg
