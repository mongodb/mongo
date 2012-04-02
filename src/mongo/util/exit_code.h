/** @file mongo/util/exit_code.h
 *
 * Mongo exit codes.
 */

/*    Copyright 2009 10gen Inc.
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

    enum ExitCode {
        EXIT_CLEAN = 0 ,
        EXIT_BADOPTIONS = 2 ,
        EXIT_REPLICATION_ERROR = 3 ,
        EXIT_NEED_UPGRADE = 4 ,
        EXIT_SHARDING_ERROR = 5 ,
        EXIT_KILL = 12 ,
        EXIT_ABRUPT = 14 ,
        EXIT_NTSERVICE_ERROR = 20 ,
        EXIT_JAVA = 21 ,
        EXIT_OOM_MALLOC = 42 ,
        EXIT_OOM_REALLOC = 43 ,
        EXIT_FS = 45 ,
        EXIT_CLOCK_SKEW = 47 ,
        EXIT_NET_ERROR = 48 ,
        EXIT_WINDOWS_SERVICE_STOP = 49 ,
        EXIT_POSSIBLE_CORRUPTION = 60 , // this means we detected a possible corruption situation, like a buf overflow
        EXIT_UNCAUGHT = 100 , // top level exception that wasn't caught
        EXIT_TEST = 101
    };

}  // namespace mongo
