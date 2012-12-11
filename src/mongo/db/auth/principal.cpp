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

#include "mongo/db/auth/principal.h"

#include <boost/date_time/posix_time/posix_time.hpp>
#include <string>

namespace mongo {

    Principal::Principal(const PrincipalName& name,
                         const boost::posix_time::ptime& expirationTime) :
            _name(name), _expirationTime(expirationTime) {}

    Principal::Principal(const PrincipalName& name) :
            _name(name), _expirationTime(boost::posix_time::pos_infin) {}

    Principal::~Principal() {}

    void Principal::setExpirationTime(boost::posix_time::ptime& expiration) {
        _expirationTime = expiration;
    }

} // namespace mongo
