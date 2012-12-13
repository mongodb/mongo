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

#include <boost/date_time/posix_time/posix_time.hpp>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/auth/principal_name.h"

namespace mongo {

    /**
     * Represents an authenticated user.  Every principal has a name and a time that the user's
     * authentication expires.
     * This class does not do any locking/synchronization, the consumer will be responsible for
     * synchronizing access.
     */
    class Principal {
        MONGO_DISALLOW_COPYING(Principal);

    public:
        // No expiration is represented as boost::posix_time::pos_infin
        Principal(const PrincipalName& name,
                  const boost::posix_time::ptime& expirationTime);
        explicit Principal(const PrincipalName& name);
        ~Principal();

        const PrincipalName& getName() const { return _name; }
        const boost::posix_time::ptime& getExpirationTime() const { return _expirationTime; }

        void setExpirationTime(boost::posix_time::ptime& expiration);

    private:
        PrincipalName _name;
        boost::posix_time::ptime _expirationTime;
    };

} // namespace mongo
