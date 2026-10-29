// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

class ServiceContext;

/**
 * Returns the boolean decoration on 'serviceCtx', which indicates whether or not this is a replica
 * set member running in standalone mode.
 */
bool getReplSetMemberInStandaloneMode(ServiceContext* serviceCtx);

/**
 * Sets the boolean decoration on 'serviceCtx', which indicates whether or not this is a replica set
 * member running in standalone mode.
 *
 * This method will hit an invariant if the decoration is reset after previously being set. The
 * standalone mode is only set at startup and should not change.
 */
void setReplSetMemberInStandaloneMode(ServiceContext* serviceCtx,
                                      bool isReplSetMemberInStandaloneMode);

}  // namespace mongo
