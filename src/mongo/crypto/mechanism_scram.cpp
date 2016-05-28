/*
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

#include "mongo/platform/basic.h"

#include "mongo/crypto/mechanism_scram.h"

#include <vector>

#include "mongo/crypto/crypto.h"
#include "mongo/platform/random.h"
#include "mongo/util/base64.h"

namespace mongo {
namespace scram {

using std::unique_ptr;

// Compute the SCRAM step Hi() as defined in RFC5802
static void HMACIteration(const unsigned char input[],
                          size_t inputLen,
                          const unsigned char salt[],
                          size_t saltLen,
                          unsigned int iterationCount,
                          unsigned char output[]) {
    unsigned char intermediateDigest[hashSize];
    unsigned char startKey[hashSize];
    // Placeholder for HMAC return size, will always be scram::hashSize for HMAC SHA-1
    unsigned int hashLen = 0;

    uassert(17450, "invalid salt length provided", saltLen + 4 == hashSize);
    memcpy(startKey, salt, saltLen);

    startKey[saltLen] = 0;
    startKey[saltLen + 1] = 0;
    startKey[saltLen + 2] = 0;
    startKey[saltLen + 3] = 1;

    // U1 = HMAC(input, salt + 0001)
    fassert(17494, crypto::hmacSha1(input, inputLen, startKey, saltLen + 4, output, &hashLen));

    memcpy(intermediateDigest, output, hashSize);

    // intermediateDigest contains Ui and output contains the accumulated XOR:ed result
    for (size_t i = 2; i <= iterationCount; i++) {
        unsigned char intermediateOutput[hashSize];
        fassert(17495,
                crypto::hmacSha1(
                    input, inputLen, intermediateDigest, hashSize, intermediateOutput, &hashLen));
        memcpy(intermediateDigest, intermediateOutput, hashSize);
        for (size_t k = 0; k < hashSize; k++) {
            output[k] ^= intermediateDigest[k];
        }
    }
}

// Iterate the hash function to generate SaltedPassword
void generateSaltedPassword(StringData hashedPassword,
                            const unsigned char* salt,
                            const int saltLen,
                            const int iterationCount,
                            unsigned char saltedPassword[hashSize]) {
    // saltedPassword = Hi(hashedPassword, salt)
    HMACIteration(reinterpret_cast<const unsigned char*>(hashedPassword.rawData()),
                  hashedPassword.size(),
                  salt,
                  saltLen,
                  iterationCount,
                  saltedPassword);
}

void generateSecrets(const std::string& hashedPassword,
                     const unsigned char salt[],
                     size_t saltLen,
                     size_t iterationCount,
                     unsigned char storedKey[hashSize],
                     unsigned char serverKey[hashSize]) {
    unsigned char saltedPassword[hashSize];
    unsigned char clientKey[hashSize];
    unsigned int hashLen = 0;

    generateSaltedPassword(hashedPassword, salt, saltLen, iterationCount, saltedPassword);

    // clientKey = HMAC(saltedPassword, "Client Key")
    fassert(17498,
            crypto::hmacSha1(saltedPassword,
                             hashSize,
                             reinterpret_cast<const unsigned char*>(clientKeyConst.data()),
                             clientKeyConst.size(),
                             clientKey,
                             &hashLen));

    // storedKey = H(clientKey)
    fassert(17499, crypto::sha1(clientKey, hashSize, storedKey));

    // serverKey = HMAC(saltedPassword, "Server Key")
    fassert(17500,
            crypto::hmacSha1(saltedPassword,
                             hashSize,
                             reinterpret_cast<const unsigned char*>(serverKeyConst.data()),
                             serverKeyConst.size(),
                             serverKey,
                             &hashLen));
}

BSONObj generateCredentials(const std::string& hashedPassword, int iterationCount) {
    const int saltLenQWords = 2;

    // Generate salt
    uint64_t userSalt[saltLenQWords];

    unique_ptr<SecureRandom> sr(SecureRandom::create());

    userSalt[0] = sr->nextInt64();
    userSalt[1] = sr->nextInt64();
    std::string encodedUserSalt =
        base64::encode(reinterpret_cast<char*>(userSalt), sizeof(userSalt));

    // Compute SCRAM secrets serverKey and storedKey
    unsigned char storedKey[hashSize];
    unsigned char serverKey[hashSize];

    generateSecrets(hashedPassword,
                    reinterpret_cast<unsigned char*>(userSalt),
                    saltLenQWords * sizeof(uint64_t),
                    iterationCount,
                    storedKey,
                    serverKey);

    std::string encodedStoredKey = base64::encode(reinterpret_cast<char*>(storedKey), hashSize);
    std::string encodedServerKey = base64::encode(reinterpret_cast<char*>(serverKey), hashSize);

    return BSON(iterationCountFieldName << iterationCount << saltFieldName << encodedUserSalt
                                        << storedKeyFieldName
                                        << encodedStoredKey
                                        << serverKeyFieldName
                                        << encodedServerKey);
}

std::string generateClientProof(const unsigned char saltedPassword[hashSize],
                                const std::string& authMessage) {
    // ClientKey := HMAC(saltedPassword, "Client Key")
    unsigned char clientKey[hashSize];
    unsigned int hashLen = 0;
    fassert(18689,
            crypto::hmacSha1(saltedPassword,
                             hashSize,
                             reinterpret_cast<const unsigned char*>(clientKeyConst.data()),
                             clientKeyConst.size(),
                             clientKey,
                             &hashLen));

    // StoredKey := H(clientKey)
    unsigned char storedKey[hashSize];
    fassert(18701, crypto::sha1(clientKey, hashSize, storedKey));

    // ClientSignature := HMAC(StoredKey, AuthMessage)
    unsigned char clientSignature[hashSize];
    fassert(18702,
            crypto::hmacSha1(storedKey,
                             hashSize,
                             reinterpret_cast<const unsigned char*>(authMessage.c_str()),
                             authMessage.size(),
                             clientSignature,
                             &hashLen));

    // ClientProof   := ClientKey XOR ClientSignature
    unsigned char clientProof[hashSize];
    for (size_t i = 0; i < hashSize; i++) {
        clientProof[i] = clientKey[i] ^ clientSignature[i];
    }

    return base64::encode(reinterpret_cast<char*>(clientProof), hashSize);
}

/**
 * Compare two arrays of bytes for equality in constant time.
 *
 * This means that the function runs for the same amount of time even if they differ. Unlike memcmp,
 * this function does not exit on the first difference.
 *
 * Returns true if the two arrays are equal.
 *
 * TODO: evaluate if LTO inlines or changes the code flow of this function.
 */
NOINLINE_DECL
bool memequal(volatile const unsigned char* s1, volatile const unsigned char* s2, size_t length) {
    unsigned char ret = 0;

    for (size_t i = 0; i < length; ++i) {
        ret |= s1[i] ^ s2[i];
    }

    return ret == 0;
}

bool verifyServerSignature(const unsigned char saltedPassword[hashSize],
                           const std::string& authMessage,
                           const std::string& receivedServerSignature) {
    // ServerKey       := HMAC(SaltedPassword, "Server Key")
    unsigned int hashLen;
    unsigned char serverKey[hashSize];
    fassert(18703,
            crypto::hmacSha1(saltedPassword,
                             hashSize,
                             reinterpret_cast<const unsigned char*>(serverKeyConst.data()),
                             serverKeyConst.size(),
                             serverKey,
                             &hashLen));

    // ServerSignature := HMAC(ServerKey, AuthMessage)
    unsigned char serverSignature[hashSize];
    fassert(18704,
            crypto::hmacSha1(serverKey,
                             hashSize,
                             reinterpret_cast<const unsigned char*>(authMessage.c_str()),
                             authMessage.size(),
                             serverSignature,
                             &hashLen));

    std::string encodedServerSignature =
        base64::encode(reinterpret_cast<char*>(serverSignature), sizeof(serverSignature));

    if (encodedServerSignature.size() != receivedServerSignature.size()) {
        return false;
    }

    return memequal(reinterpret_cast<const unsigned char*>(encodedServerSignature.c_str()),
                    reinterpret_cast<const unsigned char*>(receivedServerSignature.c_str()),
                    encodedServerSignature.size());
}

}  // namespace scram
}  // namespace mongo
