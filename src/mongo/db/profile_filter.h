// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

namespace mongo {

class CurOp;
class OpDebug;
class OperationContext;

class ProfileFilter {
public:
    virtual ~ProfileFilter() = default;

    virtual bool matches(OperationContext*, const OpDebug&, const CurOp&) const = 0;
    virtual BSONObj serialize() const = 0;

    /**
     * Returns true if the profile filter depends on the given top-level field name and false
     * otherwise.
     */
    virtual bool dependsOn(std::string_view topLevelField) const = 0;
};

}  // namespace mongo
