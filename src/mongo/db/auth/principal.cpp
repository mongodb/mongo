/**
*    Copyright (C) 2012 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "mongo/db/auth/principal.h"

#include <boost/date_time/posix_time/posix_time.hpp>
#include <string>

namespace mongo {

    Principal::Principal(const std::string& name, const boost::posix_time::ptime& expirationTime) :
            _name(name), _expirationTime(expirationTime) {}
    Principal::Principal(const std::string& name) :
            _name(name), _expirationTime(boost::posix_time::pos_infin) {}
    Principal::Principal() : _name(""), _expirationTime(boost::posix_time::pos_infin) {}

    const std::string& Principal::getName() const {
        return _name;
    }

    const boost::posix_time::ptime& Principal::getExpirationTime() const {
        return _expirationTime;
    }

    void Principal::setExpirationTime(boost::posix_time::ptime& expiration) {
        _expirationTime = expiration;
    }

} // namespace mongo
