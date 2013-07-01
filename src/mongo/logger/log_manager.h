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

#pragma once

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/logger/message_log_domain.h"
#include "mongo/logger/rotatable_file_writer.h"
#include "mongo/platform/unordered_map.h"

namespace mongo {
namespace logger {

    /**
     * Container for managing log domains.
     *
     * Use this while setting up the logging system, before launching any threads.
     */
    class LogManager {
        MONGO_DISALLOW_COPYING(LogManager);
    public:
        LogManager();
        ~LogManager();

        /**
         * Gets the global domain for this manager.  It has no name.
         */
        MessageLogDomain* getGlobalDomain() { return &_globalDomain; }

        /**
         * Get the log domain with the given name, creating if needed.
         */
        MessageLogDomain* getNamedDomain(const std::string& name);

    private:
        typedef unordered_map<std::string, MessageLogDomain*> DomainsByNameMap;

        DomainsByNameMap _domains;
        MessageLogDomain _globalDomain;
    };

}  // namespace logger
}  // namespace mongo
