/*
 * Copyright (c) 2017-2020, 2023 [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1.  Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 * 2.  Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <string.h>

#include <librepgp/stream-common.h>
#include <librepgp/stream-sig.h>
#include <librepgp/stream-packet.h>
#include <librepgp/stream-key.h>
#include "crypto/mem.h"

#include "types.h"
#include "key.hpp"
#include "rekey/rnp_key_store.h"

namespace rnp {
bool
KeyStore::add_ts_subkey(const pgp_transferable_subkey_t &tskey, Key *pkey)
{
    try {
        /* create subkey */
        Key skey(tskey, pkey);
        /* add it to the storage */
        return add_key(skey);
    } catch (const std::exception &e) {
        /* LCOV_EXCL_START */
        RNP_LOG("failed to create subkey: %s", e.what());
        RNP_LOG_KEY("primary key is %s", pkey);
        return false;
        /* LCOV_EXCL_END */
    }
}

bool
KeyStore::add_ts_key(pgp_transferable_key_t &tkey)
{
    Key *addkey = nullptr;

    /* create key from transferable key */
    try {
        Key key(tkey);
        /* temporary disable key validation */
        disable_validation = true;
        /* add key to the storage before subkeys */
        addkey = add_key(key);
    } catch (const std::exception &e) {
        disable_validation = false;
        RNP_LOG("failed to add key: %s", e.what());
        return false;
    }

    if (!addkey) {
        disable_validation = false;
        RNP_LOG("Failed to add key to key store.");
        return false;
    }

    /* add subkeys */
    for (auto &subkey : tkey.subkeys) {
        if (!add_ts_subkey(subkey, addkey)) {
            RNP_LOG("Failed to add subkey to key store.");
            disable_validation = false;
            /* during key addition all fields are copied so will be cleaned below */
            remove_key(*addkey, false);
            return false;
        }
    }

    /* now validate/refresh the whole key with subkeys */
    disable_validation = false;
    addkey->revalidate(*this);
    return true;
}

rnp_result_t
KeyStore::load_pgp_key(pgp_source_t &src, bool skiperrors)
{
    pgp_transferable_key_t key;
    rnp_result_t           ret = process_pgp_key_auto(src, key, true, skiperrors);

    if (ret && (!skiperrors || (ret != RNP_ERROR_BAD_FORMAT))) {
        return ret;
    }

    /* check whether we have primary key */
    if (key.key.tag != PGP_PKT_RESERVED) {
        return add_ts_key(key) ? RNP_SUCCESS : RNP_ERROR_BAD_STATE;
    }

    /* we just skipped some unexpected packets and read nothing */
    if (key.subkeys.empty()) {
        return RNP_SUCCESS;
    }

    return add_ts_subkey(key.subkeys.front()) ? RNP_SUCCESS : RNP_ERROR_BAD_STATE;
}

rnp_result_t
KeyStore::load_pgp(pgp_source_t &src, bool skiperrors)
{
    /* check whether we have transferable subkey in source */
    if (is_subkey_pkt(stream_pkt_type(src))) {
        pgp_transferable_subkey_t tskey;
        rnp_result_t              ret = process_pgp_subkey(src, tskey, skiperrors);
        if (ret) {
            return ret;
        }
        return add_ts_subkey(tskey) ? RNP_SUCCESS : RNP_ERROR_BAD_STATE;
    }

    /* process armored or raw transferable key packets sequence(s) */
    try {
        pgp_key_sequence_t keys;
        rnp_result_t       ret = process_pgp_keys(src, keys, skiperrors);
        if (ret) {
            return ret;
        }
        for (auto &key : keys.keys) {
            if (!add_ts_key(key)) {
                return RNP_ERROR_BAD_STATE;
            }
        }
        return RNP_SUCCESS;
    } catch (const std::exception &e) {
        /* LCOV_EXCL_START */
        RNP_LOG("%s", e.what());
        return RNP_ERROR_BAD_PARAMETERS;
        /* LCOV_EXCL_END */
    }
}
} // namespace rnp

namespace {
bool
do_write(rnp::KeyStore &key_store, pgp_dest_t &dst, bool secret)
{
    for (auto &key : key_store.keys) {
        if (key.is_secret() != secret) {
            continue;
        }
        // skip subkeys, they are written below (orphans are ignored)
        if (!key.is_primary()) {
            continue;
        }

        if (key.format != rnp::KeyFormat::GPG) {
            RNP_LOG("incorrect format (conversions not supported): %d",
                    static_cast<int>(key.format));
            return false;
        }
        key.write(dst);
        if (dst.werr) {
            return false;
        }
        for (auto &sfp : key.subkey_fps()) {
            auto *subkey = key_store.get_key(sfp);
            if (!subkey) {
                RNP_LOG("Missing subkey");
                continue;
            }
            subkey->write(dst);
            if (dst.werr) {
                return false;
            }
        }
    }
    return true;
}
} // namespace

namespace rnp {
bool
KeyStore::write_pgp(pgp_dest_t &dst)
{
    // two separate passes (public keys, then secret keys)
    return do_write(*this, dst, false) && do_write(*this, dst, true);
}
} // namespace rnp
