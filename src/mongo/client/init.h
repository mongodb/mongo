/* Copyright 2013 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "mongo/base/status.h"
#include "mongo/client/export_macros.h"

// NOTE: These functions are only intended to be used when linking against the libmongoclient
// library. The below functions are not defined in servers like mongos or mongod, which have
// their own initialization strategy.

namespace mongo {
namespace client {

    const int kDefaultShutdownGracePeriodMillis = 250;

    /**
     *  Initializes the client driver. If the 'callShutdownAtExit' parameter is true, then
     *  'initialize' schedules a call to 'client::shutdown', with a grace period of
     *  'kDefaultShutdownGracePeriodMillis', via std::atexit. Failure to shutdown within the
     *  grace period in the 'atexit' callback leads to a call to _exit. If the
     *  'callShutDownAtExit' parameter is false, then it is the responsibility of the user of
     *  the client driver to appropriately sequence a call to 'mongo::client::shutdown' and
     *  respond to any failure to terminate within the grace period. Note that 'initialize'
     *  invokes 'runGlobalInitializers', so it is not permitted to explicitly call
     *  'runGlobalInitializers' if calling 'initialize'. If a non-OK status is returned by this
     *  function, the error should be reported and the client driver API must not be used.
     */
    MONGO_CLIENT_API Status initialize(bool callShutdownAtExit = true);

    /**
     *  Terminates the client driver. If the driver does not terminate within the provided
     *  grace period (which defaults to kDefaultShutdownGracePeriodMillis), an
     *  'ExceededTimeLimit' Status will be returned, in which case it is legal to retry
     *  'shutdown'. Other non-OK status values do not admit retrying the operation, and the
     *  failure to terminate the driver should be reported, and it may be unsafe to exit the
     *  process by any mechanism which causes normal destruction of static objects.
     */
    MONGO_CLIENT_API Status shutdown(int gracePeriodMillis = kDefaultShutdownGracePeriodMillis);

} // namespace client
} // namespace mongo
