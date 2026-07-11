// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/options_parser/constraints.h"

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"

namespace mongo {
namespace optionenvironment {

Status MutuallyExclusiveKeyConstraint::check(const Environment& env) {
    Value env_value;
    Status ret = env.get(_key, &env_value);
    if (ret.isOK()) {
        ret = env.get(_otherKey, &env_value);
        if (ret.isOK()) {
            StringBuilder sb;
            sb << _otherKey << " is not allowed when " << _key << " is specified";
            return Status(ErrorCodes::BadValue, sb.str());
        }
    }

    return Status::OK();
}

Status RequiresOtherKeyConstraint::check(const Environment& env) {
    Value env_value;
    Status ret = env.get(_key, &env_value);
    if (ret.isOK()) {
        ret = env.get(_otherKey, &env_value);
        if (!ret.isOK()) {
            StringBuilder sb;
            sb << _otherKey << " is required when " << _key << " is specified";
            return Status(ErrorCodes::BadValue, sb.str());
        }
    }

    return Status::OK();
}

}  // namespace optionenvironment
}  // namespace mongo
