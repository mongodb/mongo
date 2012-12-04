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

namespace mongo {

    /**
     * Represents an authenticated user.  Every principal has a name and a time that the user's
     * authentication expires.
     * This class does not do any locking/synchronization, the consumer will be responsible for
     * synchronizing access.
     */
    class Principal {
    public:
        // No expiration is represented as boost::posix_time::pos_infin
        Principal(const std::string& name,
                  const std::string& dbname,
                  const boost::posix_time::ptime& expirationTime);
        Principal(const std::string& name, const std::string& dbname);
        Principal();
        ~Principal(){}

        const std::string& getName() const;
        const std::string& getDBName() const;
        const boost::posix_time::ptime& getExpirationTime() const;

        void setExpirationTime(boost::posix_time::ptime& expiration);

    private:
        std::string _name;
        // Database where authentication credential information is stored for this principal.
        // For externally authenticated principals this will be "$sasl".
        std::string _dbname;
        boost::posix_time::ptime _expirationTime;
    };

} // namespace mongo
