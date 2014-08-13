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

#include "mongo/db/auth/mechanism_scram.h"

#include <vector>

#ifdef MONGO_SSL
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#endif

#include "mongo/platform/random.h"
#include "mongo/util/base64.h"

namespace mongo {

const int scramHashSize = 20;

// Need to #ifdef this until our SCRAM implementation
// is independent of libcrypto
#ifdef MONGO_SSL
    // Compute the SCRAM step Hi() as defined in RFC5802
    static void HMACIteration(const unsigned char input[],
                              size_t inputLen,
                              const unsigned char salt[],
                              size_t saltLen,
                              unsigned int iterationCount,
                              unsigned char output[]){
        unsigned char tmpRes[scramHashSize];
        unsigned char startKey[scramHashSize];
        unsigned int hashLen = 0;

        uassert(17450, "invalid salt length provided", saltLen <= 16);
        memcpy (startKey, salt, saltLen);
        
        startKey[saltLen] = 0;
        startKey[saltLen+1] = 0;
        startKey[saltLen+2] = 0;
        startKey[saltLen+3] = 1;

        // U1 = HMAC(input, salt || 1)
        fassert(17494, HMAC(EVP_sha1(),
                            input,
                            inputLen,
                            startKey,
                            saltLen + 4,
                            output,
                            &hashLen));

        memcpy(tmpRes, output, scramHashSize);

        // tmpRes contain Uj and result contains the accumulated XOR:ed result
        for (size_t i = 2; i <= iterationCount; i++) {
            fassert(17495, HMAC(EVP_sha1(),
                                input,
                                inputLen,
                                tmpRes,
                                scramHashSize,
                                tmpRes,
                                &hashLen));

            for (int k = 0; k < scramHashSize; k++) {
                output[k] ^= tmpRes[k];
            }
        }
    }

    /* Compute the SCRAM secrets storedKey and serverKey
     * as defined in RFC5802 */
    static void computeSCRAMProperties(const std::string& password,
                                       const unsigned char salt[],
                                       size_t saltLen,
                                       size_t iterationCount,
                                       unsigned char storedKey[scramHashSize],
                                       unsigned char serverKey[scramHashSize]) {

        unsigned char saltedPassword[scramHashSize];
        unsigned char clientKey[scramHashSize];
        unsigned int hashLen = 0;

        // saltedPassword = Hi(password, salt)
        HMACIteration(reinterpret_cast<const unsigned char*>(password.data()),
                      password.size(),
                      salt,
                      saltLen,
                      iterationCount,
                      saltedPassword);
       
        // clientKey = HMAC(saltedPassword, "Client Key")
        const std::string clientKeyConst = "Client Key";
        fassert(17498, HMAC(EVP_sha1(),
                            saltedPassword,
                            scramHashSize,
                            reinterpret_cast<const unsigned char*>(clientKeyConst.data()),
                            clientKeyConst.size(),
                            clientKey,
                            &hashLen));
        
        // storedKey = H(clientKey)
        fassert(17499, SHA1(clientKey, scramHashSize, storedKey));
        
        // serverKey = HMAC(saltedPassword, "Server Key")
        const std::string serverKeyConst = "Server Key";
        fassert(17500, HMAC(EVP_sha1(),
                            saltedPassword,
                            scramHashSize,
                            reinterpret_cast<const unsigned char*>(serverKeyConst.data()),
                            serverKeyConst.size(),
                            serverKey,
                            &hashLen));
    }
    
#endif //MONGO_SSL

    BSONObj generateSCRAMCredentials(const std::string& hashedPassword) {
#ifndef MONGO_SSL
        return BSONObj();
#else

        // TODO: configure the default iteration count via setParameter
        const int iterationCount = 10000;
        const int saltLenQWords = 2;

        // Generate salt
        uint64_t userSalt[saltLenQWords];
        
        scoped_ptr<SecureRandom> sr(SecureRandom::create());

        userSalt[0] = sr->nextInt64();
        userSalt[1] = sr->nextInt64();
        std::string encodedUserSalt = 
            base64::encode(reinterpret_cast<char*>(userSalt), sizeof(userSalt));

        // Compute SCRAM secrets serverKey and storedKey
        unsigned char storedKey[scramHashSize];
        unsigned char serverKey[scramHashSize];

        computeSCRAMProperties(hashedPassword,
                               reinterpret_cast<unsigned char*>(userSalt),
                               saltLenQWords*sizeof(uint64_t),
                               iterationCount,
                               storedKey,
                               serverKey);

        std::string encodedStoredKey = 
            base64::encode(reinterpret_cast<char*>(storedKey), scramHashSize);
        std::string encodedServerKey = 
            base64::encode(reinterpret_cast<char*>(serverKey), scramHashSize);
     
        return BSON("iterationCount" << iterationCount <<
                    "salt" << encodedUserSalt << 
                    "storedKey" << encodedStoredKey <<
                    "serverKey" << encodedServerKey);
#endif
    }
} // namespace mongo
