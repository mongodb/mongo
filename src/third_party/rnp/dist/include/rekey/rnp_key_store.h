/*
 * Copyright (c) 2017-2023 [Ribose Inc](https://www.ribose.com).
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

#ifndef KEY_STORE_H_
#define KEY_STORE_H_

#include <stdint.h>
#include <stdbool.h>
#include "rnp.h"
#include "librepgp/stream-common.h"
#include "key.hpp"
#include <string>
#include <list>
#include <map>
#include <unordered_map>
#include <memory>
#include "librekey/kbx_blob.hpp"
#include "sec_profile.hpp"

/* Key import status. Order of elements is important. */
typedef enum pgp_key_import_status_t {
    PGP_KEY_IMPORT_STATUS_UNKNOWN = 0,
    PGP_KEY_IMPORT_STATUS_UNCHANGED,
    PGP_KEY_IMPORT_STATUS_UPDATED,
    PGP_KEY_IMPORT_STATUS_NEW,
} pgp_key_import_status_t;

typedef enum pgp_sig_import_status_t {
    PGP_SIG_IMPORT_STATUS_UNKNOWN = 0,
    PGP_SIG_IMPORT_STATUS_UNKNOWN_KEY,
    PGP_SIG_IMPORT_STATUS_UNCHANGED,
    PGP_SIG_IMPORT_STATUS_NEW
} pgp_sig_import_status_t;

namespace rnp {

typedef std::unordered_map<pgp::Fingerprint, std::list<Key>::iterator> KeyFingerprintMap;

class KeyStore {
  private:
    Key *                   add_subkey(Key &srckey, Key *oldkey);
    pgp_sig_import_status_t import_subkey_signature(Key &key, const pgp::pkt::Signature &sig);
    bool                    refresh_subkey_grips(Key &key);

  public:
    std::string      path;
    KeyFormat        format;
    SecurityContext &secctx;
    bool             disable_validation =
      false; /* do not automatically validate keys, added to this key store */

    std::list<Key>                           keys;
    KeyFingerprintMap                        keybyfp;
    std::vector<std::unique_ptr<kbx_blob_t>> blobs;

    ~KeyStore();
    KeyStore(SecurityContext &ctx) : path(""), format(KeyFormat::Unknown), secctx(ctx){};
    KeyStore(const std::string &path, SecurityContext &ctx, KeyFormat format = KeyFormat::GPG);
    /* make sure we use only empty constructor */
    KeyStore(KeyStore &&src) = delete;
    KeyStore &operator=(KeyStore &&) = delete;
    KeyStore(const KeyStore &src) = delete;
    KeyStore &operator=(const KeyStore &) = delete;

    /**
     * @brief Try to load key store from path.
     */
    bool load(const KeyProvider *key_provider = nullptr);

    /**
     * @brief Try to load key store from source.
     */
    bool load(pgp_source_t &src, const KeyProvider *key_provider = nullptr);

    /**
     * @brief Load all keys from the source, assuming openpgp format.
     *
     * @param src source to load the keys from.
     * @param skiperrors ignore key parsing errors, allowing to skip malformed/unsupported
     *                   keys.
     */
    rnp_result_t load_pgp(pgp_source_t &src, bool skiperrors = false);

    /**
     * @brief Load single key (including subkeys) from the source, assuming openpgp format.
     *
     * @param src source to load the key from.
     * @param skiperrors ignore key parsing errors, allowing to skip malformed/unknown subkeys.
     */
    rnp_result_t load_pgp_key(pgp_source_t &src, bool skiperrors = false);

    /**
     * @brief Load keystore in kbx format.
     */
    bool load_kbx(pgp_source_t &src, const KeyProvider *key_provider = nullptr);

    /**
     * @brief Load keystore in g10 format.
     */
    bool load_g10(pgp_source_t &src, const KeyProvider *key_provider = nullptr);

    /**
     * @brief Write keystore to the path.
     */
    bool write();

    /**
     * @brief Write keystore to the dest.
     */
    bool write(pgp_dest_t &dst);

    /**
     * @brief Write keystore to the dest in pgp format.
     */
    bool write_pgp(pgp_dest_t &dst);

    /**
     * @brief Write keystore to the dest in kbx format.
     *
     */
    bool write_kbx(pgp_dest_t &dst);

    void clear();

    size_t key_count() const;

    Key *      get_key(const pgp::Fingerprint &fpr);
    const Key *get_key(const pgp::Fingerprint &fpr) const;

    /**
     * @brief Get the key's subkey by its index
     *
     * @param key primary key
     * @param idx index of the subkey
     * @return pointer to the subkey or nullptr if subkey was found
     */
    Key *get_subkey(const Key &key, size_t idx);

    /**
     * @brief Get the signer's key for signature
     *
     * @param sig signature
     * @param prov key provider to request needed key.
     * @return pointer to the key or nullptr if signer's key was not found.
     */
    Key *get_signer(const pgp::pkt::Signature &sig, const KeyProvider *prov = nullptr);

    /**
     * @brief Add key to the keystore, copying it.
     * @return pointer to the added key or nullptr if failed.
     */
    Key *add_key(Key &key);

    /**
     * @brief Add signature of the specific key to the keystore, revalidating and refreshing
     *        key's data.
     *
     * @param keyfp key's fingerprint.
     * @param sig signature packet.
     * @param uid userid to which signature should be attached. If NULL then signature will be
     *            attached directly to the key.
     * @param front set to true if signature should be added to the beginning of the signature
     *              list.
     * @return pointer to the newly added signature or nullptr if error occurred (key not
     *         found, whatever else).
     */
    Signature *add_key_sig(const pgp::Fingerprint &   keyfp,
                           const pgp::pkt::Signature &sig,
                           const pgp_userid_pkt_t *   uid,
                           bool                       front);

    /**
     * @brief Add transferable key to the keystore.
     *
     * @param tkey parsed key.
     */
    bool add_ts_key(pgp_transferable_key_t &tkey);

    /**
     * @brief Add transferable subkey to the keystore.
     *
     * @param tskey parsed subkey.
     * @param pkey primary key, may be nullptr.
     */
    bool add_ts_subkey(const pgp_transferable_subkey_t &tskey, Key *pkey = nullptr);

    /**
     * @brief Import key to the keystore.
     *
     * @param srckey source key.
     * @param pubkey import just public key part.
     * @param status if not nullptr then import status will be stored here.
     * @return Key*
     */
    Key *import_key(Key &srckey, bool pubkey, pgp_key_import_status_t *status = nullptr);

    /**
     * @brief Import signature for the specified key.
     */
    pgp_sig_import_status_t import_signature(Key &key, const pgp::pkt::Signature &sig);

    /**
     * @brief Import revocation or direct-key signature to the keystore.
     *
     * @param sig signature to import.
     * @param status signature import status will be put here, if not nullptr.
     * @return pointer to the key to which this signature belongs (or nullptr if key was not
     * found)
     */
    Key *import_signature(const pgp::pkt::Signature &sig, pgp_sig_import_status_t *status);

    /**
     * @brief Remove key from the keystore.
     *
     * @param key key to remove. Must be from this keystore.
     * @param subkeys remove subkeys or not.
     * @return true if key was successfully removed, or false if key was not found in keystore.
     */
    bool remove_key(const Key &key, bool subkeys = false);

    /**
     * @brief Get primary key for the subkey, if any.
     */
    Key *primary_key(const Key &subkey);

    Key *search(const KeySearch &search, Key *after = nullptr);
};
} // namespace rnp

#endif /* KEY_STORE_H_ */
