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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/auth/security_key.h"

#include <sys/stat.h>
#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/user.h"
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

        FILE* file = fopen( filename.c_str(), "rb" );
        if (!file) {
            log() << "error opening file: " << filename << ": " << strerror(errno) << endl;
            return false;
        }

        string str = "";

        // strip key file
        const unsigned long long fileLength = stats.st_size;
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

        const unsigned long long keyLength = str.size();
        if (keyLength < 6 || keyLength > 1024) {
            log() << " security key in " << filename << " has length " << keyLength
                  << ", must be between 6 and 1024 chars" << endl;
            return false;
        }

        LOG(1) << "security key: " << str << endl;

        User::CredentialData credentials;
        credentials.password = DBClientWithCommands::createPasswordDigest(
                internalSecurity.user->getName().getUser().toString(), str);
        internalSecurity.user->setCredentials(credentials);

        if (cmdLine.clusterAuthMode == "keyfile" || cmdLine.clusterAuthMode == "sendKeyfile") {
            setInternalUserAuthParams(
                    BSON(saslCommandMechanismFieldName << "MONGODB-CR" <<
                         saslCommandUserSourceFieldName <<
                         internalSecurity.user->getName().getDB() <<
                         saslCommandUserFieldName << internalSecurity.user->getName().getUser() <<
                         saslCommandPasswordFieldName << credentials.password <<
                         saslCommandDigestPasswordFieldName << false));
        }
        return true;
    }

} // namespace mongo
