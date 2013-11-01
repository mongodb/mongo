/*    Copyright 2013 10gen Inc.
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

#include "mongo/db/server_options.h"

namespace mongo {

    /**
     * This struct represents global configuration data for the server.  These options get set from
     * the command line and are used inline in the code.  Note that much shared code uses this
     * struct, which is why it is here in its own file rather than in the same file as the code that
     * sets it via the command line, which would pull in more dependencies.
     */
    ServerGlobalParams serverGlobalParams;

} // namespace mongo
