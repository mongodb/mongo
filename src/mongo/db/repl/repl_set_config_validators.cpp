// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/repl_set_config_validators.h"

#include "mongo/db/repl/repl_set_config_gen.h"

namespace mongo {
namespace repl {

Status validateReplicaSetIdNotNull(OID replicaSetId) {
    if (!replicaSetId.isSet()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << ReplSetConfigSettings::kReplicaSetIdFieldName
                                    << " field value cannot be null");
    }
    return Status::OK();
}

}  // namespace repl
}  // namespace mongo
