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

#include "mongo/logger/message_log_domain.h"
#include "mongo/logger/log_manager.h"
#include "mongo/logger/rotatable_file_manager.h"

namespace mongo {
namespace logger {

    /**
     * Gets a global singleton instance of RotatableFileManager.
     */
    RotatableFileManager* globalRotatableFileManager();

    /**
     * Gets a global singleton instance of LogManager.
     */
    LogManager* globalLogManager();

    /**
     * Gets the global MessageLogDomain associated for the global log manager.
     */
    inline MessageLogDomain* globalLogDomain() { return globalLogManager()->getGlobalDomain(); }

}  // namespace logger
}  // namespace mongo

