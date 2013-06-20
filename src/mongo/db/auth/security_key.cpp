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

#include "mongo/db/auth/security_key.h"

#include <sys/stat.h>
#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/client/sasl_client_authenticate.h"

static bool authParamsSet = false;

namespace mongo {

    bool isInternalAuthSet() {
       return authParamsSet; 
    }

    bool setInternalUserAuthParams(BSONObj authParams) {
        if (!isInternalAuthSet()) {
            internalSecurity.authParams = authParams.copy();
            authParamsSet = true;
            return true;
        }
        else {
            log() << "Internal auth params have already been set" << endl;
            return false;
        }
    }
 
    bool authenticateInternalUser(DBClientWithCommands* conn){
        if (!isInternalAuthSet()) {
            log() << "ERROR: No authentication params set for internal user" << endl;
            return false;
        }
        try {
            conn->auth(internalSecurity.authParams); 
            return true;
        } catch(const UserException& ex) {
            log() << "can't authenticate to " << conn->toString() << " as internal user, error: "
                  << ex.what() << endl;
            return false;
        }
    }

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
                fclose( file );
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
                fclose( file );
                return false;
            }

            str += buf;
        }

        fclose( file );

        if (str.size() < 6) {
            log() << "security key must be at least 6 characters" << endl;
            return false;
        }

        LOG(1) << "security key: " << str << endl;

        // createPWDigest should really not be a member func
        DBClientConnection conn;
        internalSecurity.pwd = conn.createPasswordDigest(internalSecurity.user, str);

        if (cmdLine.clusterAuthMode == "keyfile" || cmdLine.clusterAuthMode == "sendKeyfile") {
            setInternalUserAuthParams(BSON(saslCommandMechanismFieldName << "MONGODB-CR" <<
                                      saslCommandUserSourceFieldName << "local" <<
                                      saslCommandUserFieldName << internalSecurity.user <<
                                      saslCommandPasswordFieldName << internalSecurity.pwd <<
                                      saslCommandDigestPasswordFieldName << false));
        }
        return true;
    }

} // namespace mongo
