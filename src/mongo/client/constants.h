// constants.h

/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

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

        /* for backward compatibility: this let's us know the server supports
           the QueryOption_AwaitData option. if it doesn't, a repl slave client should sleep
        a little between getMore's.
        */
        ResultFlag_AwaitCapable = 8
    };

}
