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

#include "mongo/base/string_data.h"

namespace mongo {
namespace auth {

    /**
     * Hashes the password so that it can be stored in a user object or used for MONGODB-CR
     * authentication.
     */
    std::string createPasswordDigest(const StringData& username,
                                     const StringData& clearTextPassword);

}  // namespace auth
}  // namespace mongo
