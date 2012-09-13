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

#include <string>

namespace mongo {

    /**
     * This is a generated class containg a table of error codes and their corresponding
     * error strings. The class is derived from the definitions in the error_codes.err
     * file.
     *
     * TODO: Do not update this file directly. Update error_codes.err instead.
     *
     *  # Error table
     *  [OK, "ok"]
     *  [InternalError, 1]
     *  [BadValue, 2]
     *  ...
     *  [HostUnreachable, <nnnn>]
     *  [HostNotFound, <nnnn>]
     *
     *  # Error classes
     *  [NetworkError, [HostUnreachable, HostNotFound]]
     *
     */
    class ErrorCodes {
    public:
        enum Error {
            OK = 0,
            InternalError = 1,
            BadValue = 2,
            DuplicateKey = 3,
            NoSuchKey = 4,
            GraphContainsCycle = 5,
            HostUnreachable = 6,
            HostNotFound = 7,
            UnknownError = 8,
            FailedToParse = 9,
            MaxError
        };

        static const char* errorString(Error err) {
            switch (err) {
            case OK:
                return "OK";
            case InternalError:
                return "InternalError";
            case BadValue:
                return "BadValue";
            case NoSuchKey:
                return "NoSuchKey";
            case HostUnreachable:
                return "HostUnreachable";
            case HostNotFound:
                return "HostNotFound";
            case DuplicateKey:
                return "DuplicateKey";
            case GraphContainsCycle:
                return "GraphContainsCycle";
            case UnknownError:
                return "UnknownError";
            case FailedToParse:
                return "FailedToParse";
            default:
                return "Unknown error code";
            }
        }

        static bool isNetworkError(Error err) {
            switch (err) {
            case HostUnreachable:
            case HostNotFound:
                return true;
            default:
                return false;
            }
        }

    };

} // namespace mongo

