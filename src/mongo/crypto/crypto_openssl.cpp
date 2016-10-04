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

#include "mongo/config.h"
#include "mongo/util/scopeguard.h"

#ifndef MONGO_CONFIG_SSL
#error This file should only be included in SSL-enabled builds
#endif

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

namespace mongo {
namespace crypto {
/*
 * Computes a SHA-1 hash of 'input'.
 */
bool sha1(const unsigned char* input, const size_t inputLen, unsigned char* output) {
    EVP_MD_CTX digestCtx;
    EVP_MD_CTX_init(&digestCtx);
    ON_BLOCK_EXIT(EVP_MD_CTX_cleanup, &digestCtx);

    if (1 != EVP_DigestInit_ex(&digestCtx, EVP_sha1(), NULL)) {
        return false;
    }

    if (1 != EVP_DigestUpdate(&digestCtx, input, inputLen)) {
        return false;
    }

    return (1 == EVP_DigestFinal_ex(&digestCtx, output, NULL));
}

/*
 * Computes a HMAC SHA-1 keyed hash of 'input' using the key 'key'
 */
bool hmacSha1(const unsigned char* key,
              const size_t keyLen,
              const unsigned char* input,
              const size_t inputLen,
              unsigned char* output,
              unsigned int* outputLen) {
    return HMAC(EVP_sha1(), key, keyLen, input, inputLen, output, outputLen);
}

}  // namespace crypto
}  // namespace mongo
