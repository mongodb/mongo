// security_common.cpp
/*
 *    Copyright (C) 2010 10gen Inc.
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

/**
 * This file contains inter-mongo instance security helpers.  Due to the
 * requirement that it be possible to compile this into mongos and mongod, it
 * should not depend on much external stuff.
 */

#include "pch.h"

#include <sys/stat.h>

#include "security.h"
#include "security_common.h"
#include "commands.h"
#include "nonce.h"
#include "../util/md5.hpp"
#include "client_common.h"
#include "mongo/client/dbclientinterface.h"


namespace mongo {

    // this is a config setting, set at startup and not changing after initialization.
    bool noauth = true;

    AuthInfo internalSecurity;

    bool setUpSecurityKey(const string& filename) {
        struct stat stats;

        // check obvious file errors
        if (stat(filename.c_str(), &stats) == -1) {
            log() << "error getting file " << filename << ": " << strerror(errno) << endl;
            return false;
        }

#if !defined(_WIN32)
        // check permissions: must be X00, where X is >= 4
        if ((stats.st_mode & (S_IRWXG|S_IRWXO)) != 0) {
            log() << "permissions on " << filename << " are too open" << endl;
            return false;
        }
#endif

        const unsigned long long fileLength = stats.st_size;
        if (fileLength < 6 || fileLength > 1024) {
            log() << " key file " << filename << " has length " << stats.st_size
                  << ", must be between 6 and 1024 chars" << endl;
            return false;
        }

        FILE* file = fopen( filename.c_str(), "rb" );
        if (!file) {
            log() << "error opening file: " << filename << ": " << strerror(errno) << endl;
            return false;
        }

        string str = "";

        // strip key file
        unsigned long long read = 0;
        while (read < fileLength) {
            char buf;
            int readLength = fread(&buf, 1, 1, file);
            if (readLength < 1) {
                log() << "error reading file " << filename << endl;
                return false;
            }
            read++;

            // check for whitespace
            if ((buf >= '\x09' && buf <= '\x0D') || buf == ' ') {
                continue;
            }

            // check valid base64
            if ((buf < 'A' || buf > 'Z') && (buf < 'a' || buf > 'z') && (buf < '0' || buf > '9') && buf != '+' && buf != '/') {
                log() << "invalid char in key file " << filename << ": " << buf << endl;
                return false;
            }

            str += buf;
        }

        if (str.size() < 6) {
            log() << "security key must be at least 6 characters" << endl;
            return false;
        }

        log(1) << "security key: " << str << endl;

        // createPWDigest should really not be a member func
        DBClientConnection conn;
        internalSecurity.pwd = conn.createPasswordDigest(internalSecurity.user, str);

        return true;
    }

    void CmdAuthenticate::authenticate(const string& dbname, const string& user, const bool readOnly) {
        ClientBasic* c = ClientBasic::getCurrent();
        verify(c);
        AuthenticationInfo *ai = c->getAuthenticationInfo();

        if ( readOnly ) {
            ai->authorizeReadOnly( dbname , user );
        }
        else {
            ai->authorize( dbname , user );
        }
    }


    bool AuthenticationInfo::_isAuthorized(const string& dbname, Auth::Level level) const {
        if ( noauth ) {
            return true;
        }
        {
            scoped_spinlock lk(_lock);

            if ( _isAuthorizedSingle_inlock( dbname , level ) )
                return true;

            if ( _isAuthorizedSingle_inlock( "admin" , level ) )
                return true;

            if ( _isAuthorizedSingle_inlock( "local" , level ) )
                return true;
        }
        return _isAuthorizedSpecialChecks( dbname );
    }

    bool AuthenticationInfo::_isAuthorizedSingle_inlock(const string& dbname, Auth::Level level) const {
        MA::const_iterator i = _dbs.find(dbname);
        return i != _dbs.end() && i->second.level >= level;
    }

} // namespace mongo
