// constants.h

#pragma once

namespace mongo {

    /* query results include a 32 result flag word consisting of these bits */
    enum ResultFlagType {
        /* returned, with zero results, when getMore is called but the cursor id
           is not valid at the server. */
        ResultFlag_CursorNotFound = 1,

        /* { $err : ... } is being returned */
        ResultFlag_ErrSet = 2,

        /* Have to update config from the server, usually $err is also set */
        ResultFlag_ShardConfigStale = 4,

        /* for backward compatability: this let's us know the server supports
           the QueryOption_AwaitData option. if it doesn't, a repl slave client should sleep
        a little between getMore's.
        */
        ResultFlag_AwaitCapable = 8
    };

}
