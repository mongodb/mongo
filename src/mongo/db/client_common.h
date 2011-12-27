// client_common.h

/**
*    Copyright (C) 2008 10gen Inc.
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

#pragma once

//#include "../pch.h"
//#include "security.h"
#include "../util/net/hostandport.h"

namespace mongo {

    class AuthenticationInfo;
    
    /**
     * this is the base class for Client and ClientInfo
     * Client is for mongod
     * Client is for mongos
     * They should converge slowly
     * The idea is this has the basic api so that not all code has to be duplicated
     */
    class ClientBasic : boost::noncopyable {
    public:
        virtual ~ClientBasic(){}
        virtual const AuthenticationInfo * getAuthenticationInfo() const = 0;
        virtual AuthenticationInfo * getAuthenticationInfo() = 0;

        virtual bool hasRemote() const = 0;
        virtual HostAndPort getRemote() const = 0;

        static ClientBasic* getCurrent();
    };
}
