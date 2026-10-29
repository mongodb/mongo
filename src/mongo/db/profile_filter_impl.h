// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/curop.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_filter.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <functional>
#include <string_view>

#include <absl/container/node_hash_map.h>

namespace mongo {

class [[MONGO_MOD_NEEDS_REPLACEMENT]] ProfileFilterImpl final : public ProfileFilter {
public:
    [[MONGO_MOD_PRIVATE]] ProfileFilterImpl(BSONObj expr,
                                            boost::intrusive_ptr<ExpressionContext> parserExpCtx);
    [[MONGO_MOD_PRIVATE]] bool matches(OperationContext* opCtx,
                                       const OpDebug& op,
                                       const CurOp& curop) const override;
    [[MONGO_MOD_PRIVATE]] BSONObj serialize() const override {
        return _matcher.getMatchExpression()->serialize();
    }

    [[MONGO_MOD_PRIVATE]] bool dependsOn(std::string_view topLevelField) const override {
        return _needWholeDocument || _dependencies.count(topLevelField) > 0;
    }

    [[MONGO_MOD_NEEDS_REPLACEMENT]] static void initializeDefaults(ServiceContext* svcCtx);

private:
    StringSet _dependencies;
    bool _needWholeDocument = false;

    Matcher _matcher;
    std::function<BSONObj(OpDebug::AppendArgs)> _makeBSON;
};

}  // namespace mongo
