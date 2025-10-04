/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/curop.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_filter.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <functional>

#include <absl/container/node_hash_map.h>

namespace mongo {

class MONGO_MOD_NEEDS_REPLACEMENT ProfileFilterImpl final : public ProfileFilter {
public:
    MONGO_MOD_PRIVATE ProfileFilterImpl(BSONObj expr,
                                        boost::intrusive_ptr<ExpressionContext> parserExpCtx);
    MONGO_MOD_PRIVATE bool matches(OperationContext* opCtx,
                                   const OpDebug& op,
                                   const CurOp& curop) const override;
    MONGO_MOD_PRIVATE BSONObj serialize() const override {
        return _matcher.getMatchExpression()->serialize();
    }

    MONGO_MOD_PRIVATE bool dependsOn(StringData topLevelField) const override {
        return _needWholeDocument || _dependencies.count(topLevelField) > 0;
    }

    MONGO_MOD_NEEDS_REPLACEMENT static void initializeDefaults(ServiceContext* svcCtx);

private:
    StringSet _dependencies;
    bool _needWholeDocument = false;

    Matcher _matcher;
    std::function<BSONObj(ProfileFilter::Args)> _makeBSON;
};

}  // namespace mongo
