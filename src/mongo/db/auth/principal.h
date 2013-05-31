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
#include "mongo/base/string_data.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/util/string_map.h"

namespace mongo {

    /**
     * Represents an authenticated user.  Every principal has a name, a time that the user's
     * authentication expires, and a flag that describes whether or not privileges should be
     * acquired implicitly.
     *
     * The implicit privilege acquisition flag defaults to disabled, and the expiration time
     * defaults to never.
     *
     * This class does not do any locking/synchronization, the consumer will be responsible for
     * synchronizing access.
     */
    class Principal {
        MONGO_DISALLOW_COPYING(Principal);

    public:
        Principal(const UserName& name,
                  const boost::posix_time::ptime& expirationTime);
        explicit Principal(const UserName& name);
        ~Principal();

        const UserName& getName() const { return _name; }

        // Returns the expiration time of this principal information.
        // No expiration is represented as boost::posix_time::pos_infin
        const boost::posix_time::ptime& getExpirationTime() const { return _expirationTime; }

        // Returns true if this principal is configured for implicit acquisition of privileges.
        bool isImplicitPrivilegeAcquisitionEnabled() const { return _enableImplicitPrivileges; }

        void setExpirationTime(boost::posix_time::ptime& expiration);
        void setImplicitPrivilegeAcquisition(bool enabled);

        bool isDatabaseProbed(const StringData& dbname) const;
        void markDatabaseAsProbed(const StringData& dbname);

    private:
        UserName _name;
        boost::posix_time::ptime _expirationTime;
        bool _enableImplicitPrivileges;
        StringMap<bool> _probedDatabases;
    };

} // namespace mongo
