// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

namespace mongo {

/* query results include a 32 result flag word consisting of these bits */
enum ResultFlagType {
    /* returned, with zero results, when getMore is called but the cursor id
       is not valid at the server. */
    ResultFlag_CursorNotFound = 1,

    /* { $err : ... } is being returned */
    ResultFlag_ErrSet = 2,

    /* Formerly used to comminicate stale version errors */
    ResultFlag_ShardConfigStaleDeprecated = 4,

    /* for backward compatibility: this let's us know the server supports
       the QueryOption_AwaitData option. if it doesn't, a repl secondary client should sleep
    a little between getMore's.
    */
    ResultFlag_AwaitCapable = 8
};
}  // namespace mongo
