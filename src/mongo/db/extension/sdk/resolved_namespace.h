// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <string_view>
#include <vector>

namespace mongo::extension::sdk {

/**
 * ResolvedNamespace represents the view information passed to an aggregation stage AST node when
 * binding view information. It contains the view namespace and the view pipeline.
 */
class ResolvedNamespace final {
public:
    ResolvedNamespace(std::string_view viewNssDb,
                      std::string_view viewNssName,
                      std::vector<mongo::BSONObj> viewPipeline)
        : _viewNssDb(viewNssDb),
          _viewNssName(viewNssName),
          _viewPipeline(std::move(viewPipeline)) {}

    std::string_view dbName() const {
        return _viewNssDb;
    }

    std::string_view viewName() const {
        return _viewNssName;
    }

    const std::vector<mongo::BSONObj>& viewPipeline() const {
        return _viewPipeline;
    }

private:
    std::string _viewNssDb;
    std::string _viewNssName;
    std::vector<mongo::BSONObj> _viewPipeline;
};
}  // namespace mongo::extension::sdk
