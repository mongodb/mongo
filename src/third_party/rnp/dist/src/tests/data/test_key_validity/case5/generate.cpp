/*
 * Copyright (c) 2020 [Ribose Inc](https://www.ribose.com).
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

#include <iostream>
#include <cstdint>
#include "librepgp/stream-key.h"
#include "librepgp/stream-packet.h"
#include "fingerprint.h"
#include "key.hpp"
#include "crypto/signatures.h"

static bool
load_transferable_key(pgp_transferable_key_t *key, const char *fname)
{
    pgp_source_t src = {};
    bool         res = !init_file_src(&src, fname) && !process_pgp_key(src, key, false);
    src.close();
    return res;
}

bool calculate_primary_binding(const pgp_key_pkt_t &key,
                               const pgp_key_pkt_t &subkey,
                               pgp_hash_alg_t       halg,
                               pgp::pkt::Signature &sig,
                               rnp::Hash &          hash,
                               rnp::RNG &           rng);

int
main(int argc, char **argv)
{
    if (argc < 3) {
        std::cout << "Generate test file with subkey, signed by the other key.\n Usage: "
                     "./generate ../alice-sub-sec.asc ../basil-sec.asc\n";
        return 1;
    }

    pgp_transferable_key_t tpkey = {};
    pgp_transferable_key_t tskey = {};

    if (!load_transferable_key(&tpkey, argv[1])) {
        std::cout << "Failed to load first key.\n";
        return 1;
    }

    if (!load_transferable_key(&tskey, argv[2])) {
        std::cout << "Failed to load second key.\n";
        return 1;
    }

    pgp_transferable_subkey_t *subkey =
      (pgp_transferable_subkey_t *) list_front(tpkey.subkeys);
    pgp::pkt::Signature *binding = (pgp::pkt::Signature *) list_front(subkey->signatures);

    if (decrypt_secret_key(&tskey.key, "password")) {
        RNP_LOG("Failed to decrypt secret key");
        return 1;
    }
    if (decrypt_secret_key(&subkey->subkey, "password")) {
        RNP_LOG("Failed to decrypt secret subkey");
        return 1;
    }

    /* now let's rebuild binding using the other key */
    uint8_t          keyid[PGP_KEY_ID_SIZE];
    pgp::Fingerprint keyfp;

    free(binding->hashed_data);
    binding->hashed_data = NULL;
    binding->hashed_len = 0;

    pgp_keyid(keyid, sizeof(keyid), tskey.key);
    pgp_fingerprint(&keyfp, tskey.key);

    binding->halg = tskey.key.material->adjust_hash(binding->halg);
    binding->palg = tskey.key.alg;
    binding->set_keyfp(keyfp);

    /* This requires transition to rnp::Hash once will be used */
    rnp::Hash hash;
    rnp::Hash hashcp;

    binding->fill_hashed_data();
    if (!signature_hash_binding(binding, &tpkey.key, &subkey->subkey, &hash) ||
        !pgp_hash_copy(&hashcp, &hash)) {
        RNP_LOG("failed to hash signature");
        return 1;
    }

    rnp::RNG rng(rnp::RNG::Type::System);
    if (signature_calculate(binding, &tskey.key.material, &hash, &rng)) {
        RNP_LOG("failed to calculate signature");
        return 1;
    }

    pgp_key_flags_t realkf = (pgp_key_flags_t) binding.key_flags();
    if (!realkf) {
        realkf = pgp_pk_alg_capabilities(subkey->subkey.alg);
    }
    if (realkf & PGP_KF_SIGN) {
        pgp::pkt::Signature embsig;
        bool                embres;

        if (!calculate_primary_binding(
              &tpkey.key, &subkey->subkey, binding->halg, &embsig, &hashcp, &rng)) {
            RNP_LOG("failed to calculate primary key binding signature");
            return 1;
        }
        embres = signature_set_embedded_sig(binding, &embsig);
        free_signature(&embsig);
        if (!embres) {
            RNP_LOG("failed to add primary key binding signature");
            return 1;
        }
    }

    try {
        binding->set_keyid(keyid);
    } catch (const std::exception &e) {
        RNP_LOG("failed to set issuer key id: %s", e.what());
        return 1;
    }

    if (!transferable_key_to_public(&tpkey)) {
        RNP_LOG("Failed to extract public key part.");
        return 1;
    }

    pgp_dest_t dst = {};
    init_stdout_dest(&dst);
    write_transferable_key(tpkey, dst, true);
    dst_close(&dst, false);

    transferable_key_destroy(&tpkey);
    transferable_key_destroy(&tskey);

    return 0;
}