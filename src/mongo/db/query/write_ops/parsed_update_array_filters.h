// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <map>
#include <memory>
#include <string_view>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Parses the array filters portion of the update request.
 */
[[MONGO_MOD_PUBLIC]]
StatusWith<std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>>>
parsedUpdateArrayFilters(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                         const std::vector<BSONObj>& rawArrayFiltersIn,
                         const NamespaceString& nss);

}  // namespace mongo
