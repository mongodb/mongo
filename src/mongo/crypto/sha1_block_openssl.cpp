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

#include "mongo/crypto/sha1_block.h"

#include "mongo/config.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#ifndef MONGO_CONFIG_SSL
#error This file should only be included in SSL-enabled builds
#endif

#include <cstring>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#if OPENSSL_VERSION_NUMBER < 0x10100000L
namespace {
// Copies of OpenSSL after 1.1.0 define new EVP digest routines. We must
// polyfill used definitions to interact with older OpenSSL versions.
EVP_MD_CTX* EVP_MD_CTX_new() {
    void* ret = OPENSSL_malloc(sizeof(EVP_MD_CTX));

    if (ret != NULL) {
        memset(ret, 0, sizeof(EVP_MD_CTX));
    }
    return static_cast<EVP_MD_CTX*>(ret);
}

void EVP_MD_CTX_free(EVP_MD_CTX* ctx) {
    EVP_MD_CTX_cleanup(ctx);
    OPENSSL_free(ctx);
}
}  // namespace
#endif

namespace mongo {
/*
 * Computes a SHA-1 hash of 'input'.
 */
SHA1Block SHA1Block::computeHash(const uint8_t* input, size_t inputLen) {
    HashType output;

    EVP_MD_CTX digestCtx;
    EVP_MD_CTX_init(&digestCtx);
    ON_BLOCK_EXIT(EVP_MD_CTX_cleanup, &digestCtx);

    fassert(40379,
            EVP_DigestInit_ex(&digestCtx, EVP_sha1(), NULL) == 1 &&
                EVP_DigestUpdate(&digestCtx, input, inputLen) == 1 &&
                EVP_DigestFinal_ex(&digestCtx, output.data(), NULL) == 1);
    return SHA1Block(output);
}

/*
 * Computes a HMAC SHA-1 keyed hash of 'input' using the key 'key', writes output into 'output'.
 */
void SHA1Block::computeHmac(const uint8_t* key,
                            size_t keyLen,
                            const uint8_t* input,
                            size_t inputLen,
                            SHA1Block* const output) {
    fassert(40380,
            HMAC(EVP_sha1(), key, keyLen, input, inputLen, output->_hash.data(), NULL) != NULL);
}

}  // namespace mongo
