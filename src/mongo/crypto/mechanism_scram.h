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

#include "mongo/base/secure_allocator.h"
#include "mongo/base/status.h"
#include "mongo/crypto/crypto.h"
#include "mongo/db/jsobj.h"

namespace mongo {
namespace scram {
// Convert a SHA1Hash into a base64 encoded string.
std::string hashToBase64(const SecureHandle<SHA1Hash>& hash);

const std::string serverKeyConst = "Server Key";
const std::string clientKeyConst = "Client Key";

const std::string iterationCountFieldName = "iterationCount";
const std::string saltFieldName = "salt";
const std::string storedKeyFieldName = "storedKey";
const std::string serverKeyFieldName = "serverKey";

/*
 * The precursors necessary to perform the computation which produces SCRAMSecrets.
 * These are the original password, its salt, and the number of times it must be
 * hashed to produce the SaltedPassword used to generate the rest of the SCRAMSecrets.
 */
struct SCRAMPresecrets {
    SCRAMPresecrets(std::string hashedPassword,
                    std::vector<std::uint8_t> salt,
                    size_t iterationCount)
        : hashedPassword(std::move(hashedPassword)),
          salt(std::move(salt)),
          iterationCount(iterationCount) {}

    std::string hashedPassword;
    std::vector<std::uint8_t> salt;
    size_t iterationCount;
};

inline bool operator==(const SCRAMPresecrets& lhs, const SCRAMPresecrets& rhs) {
    return lhs.hashedPassword == rhs.hashedPassword && lhs.salt == rhs.salt &&
        lhs.iterationCount == rhs.iterationCount;
}

/*
 * Computes the SaltedPassword from password, salt and iterationCount.
 */
SHA1Hash generateSaltedPassword(const SCRAMPresecrets& presecrets);

/*
 * Stores all of the keys, generated from a password, needed for a client or server to perform a
 * SCRAM handshake. This structure will secureZeroMemory itself on destruction.
 */
struct SCRAMSecrets {
    SecureHandle<SHA1Hash> clientKey;
    SecureHandle<SHA1Hash> storedKey;
    SecureHandle<SHA1Hash> serverKey;
};

/*
 * Computes the SCRAM secrets clientKey, storedKey, and serverKey using the salt 'salt'
 * and iteration count 'iterationCount' as defined in RFC5802 (server side).
 */
SCRAMSecrets generateSecrets(const SCRAMPresecrets& presecrets);

/*
 * Computes the ClientKey and StoredKey from SaltedPassword (client side).
 */
SCRAMSecrets generateSecrets(const SHA1Hash& saltedPassword);

/*
 * Generates the user salt and the SCRAM secrets storedKey and serverKey as
 * defined in RFC5802 (server side).
 */
BSONObj generateCredentials(const std::string& hashedPassword, int iterationCount);

/*
 * Computes the ClientProof from ClientKey, StoredKey, and authMessage (client side).
 */
std::string generateClientProof(const SCRAMSecrets& clientCredentials,
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
bool verifyServerSignature(const SCRAMSecrets& clientCredentials,
                           const std::string& authMessage,
                           const std::string& serverSignature);

/*
 * Verifies ClientProof (server side).
 */
bool verifyClientProof(StringData clientProof, StringData storedKey, StringData authMessage);

}  // namespace scram
}  // namespace mongo
