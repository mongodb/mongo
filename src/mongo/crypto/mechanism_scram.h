/**
*    Copyright (C) 2014 10gen Inc.
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

#pragma once

#include <string>

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"

namespace mongo {
namespace scram {
const unsigned int hashSize = 20;

const std::string serverKeyConst = "Server Key";
const std::string clientKeyConst = "Client Key";

const std::string iterationCountFieldName = "iterationCount";
const std::string saltFieldName = "salt";
const std::string storedKeyFieldName = "storedKey";
const std::string serverKeyFieldName = "serverKey";

/*
 * Computes the SaltedPassword from password, salt and iterationCount.
 */
void generateSaltedPassword(StringData hashedPassword,
                            const unsigned char* salt,
                            const int saltLen,
                            const int iterationCount,
                            unsigned char saltedPassword[hashSize]);

/*
 * Computes the SCRAM secrets storedKey and serverKey using the salt 'salt'
 * and iteration count 'iterationCount' as defined in RFC5802 (server side).
 */
void generateSecrets(const std::string& hashedPassword,
                     const unsigned char salt[],
                     size_t saltLen,
                     size_t iterationCount,
                     unsigned char storedKey[hashSize],
                     unsigned char serverKey[hashSize]);

/*
 * Generates the user salt and the SCRAM secrets storedKey and serverKey as
 * defined in RFC5802 (server side).
 */
BSONObj generateCredentials(const std::string& hashedPassword, int iterationCount);

/*
 * Computes the ClientProof from SaltedPassword and authMessage (client side).
 */
std::string generateClientProof(const unsigned char saltedPassword[hashSize],
                                const std::string& authMessage);

/*
 * Validates that the provided password 'hashedPassword' generates the serverKey
 * 'serverKey' given iteration count 'iterationCount' and salt 'salt'.
 */
bool validatePassword(const std::string& hashedPassword,
                      int iterationCount,
                      const std::string& salt,
                      const std::string& storedKey);

/*
 * Verifies ServerSignature (client side).
 */
bool verifyServerSignature(const unsigned char saltedPassword[hashSize],
                           const std::string& authMessage,
                           const std::string& serverSignature);
}  // namespace scram
}  // namespace mongo
