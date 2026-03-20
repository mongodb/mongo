/*
 * Copyright (c) 2017-2024 [Ribose Inc](https://www.ribose.com).
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

#ifndef RNP_KEY_HPP
#define RNP_KEY_HPP

#include <stdbool.h>
#include <stdio.h>
#include <vector>
#include <unordered_map>
#include "pass-provider.h"
#include "../librepgp/stream-key.h"
#include "../librepgp/stream-packet.h"
#include "crypto/symmetric.h"
#include "types.h"
#include "rawpacket.hpp"
#include "signature.hpp"
#include "userid.hpp"
#include "sec_profile.hpp"

namespace rnp {
using SignatureMap = std::unordered_map<pgp::SigID, Signature>;
class KeyStore;
class CertParams;
class BindingParams;

enum class KeyFormat : int { Unknown, GPG, KBX, G10 };

/* describes a user's key */
class Key {
  private:
    SignatureMap        sigs_map_;     /* map with subsigs stored by their id */
    pgp::SigIDs         sigs_;         /* subsig ids to lookup actual sig in map */
    pgp::SigIDs         keysigs_;      /* direct-key signature ids in the original order */
    std::vector<UserID> uids_{};       /* array of user ids */
    pgp_key_pkt_t       pkt_{};        /* pubkey/seckey data packet */
    uint8_t             flags_{};      /* key flags */
    uint32_t            expiration_{}; /* key expiration time, if available */
    pgp::Fingerprint    fingerprint_;
    pgp::KeyGrip        grip_{};
    pgp::Fingerprint    primary_fp_; /* fingerprint of the primary key (for subkeys) */
    bool                primary_fp_set_{};
    pgp::Fingerprints   subkey_fps_; /* array of subkey fingerprints (for primary keys) */
    RawPacket           rawpkt_;     /* key raw packet */
    uint32_t            uid0_{};     /* primary uid index in uids array */
    bool                uid0_set_{}; /* flag for the above */
    bool                revoked_{};  /* key has been revoked */
    Revocation          revocation_; /* revocation reason */
    pgp::Fingerprints   revokers_;
    pgp_validity_t      validity_{};   /* key's validity */
    uint64_t            valid_till_{}; /* date till which key is/was valid */

    Signature *latest_uid_selfcert(uint32_t uid);
    void       validate_primary(KeyStore &keyring);
    void       merge_validity(const pgp_validity_t &src);
    uint64_t   valid_till_common(bool expiry) const;
    bool       write_sec_pgp(pgp_dest_t &       dst,
                             pgp_key_pkt_t &    seckey,
                             const std::string &password,
                             RNG &              rng);

  public:
    KeyFormat format = KeyFormat::Unknown; /* the format of the key in packets[0] */

    Key() = default;
    Key(const pgp_key_pkt_t &pkt);
    Key(const pgp_key_pkt_t &pkt, Key &primary);
    Key(const Key &src, bool pubonly = false);
    Key(const pgp_transferable_key_t &src);
    Key(const pgp_transferable_subkey_t &src, Key *primary);
    Key &operator=(const Key &) = default;
    Key &operator=(Key &&) = default;

    size_t            sig_count() const;
    Signature &       get_sig(size_t idx);
    const Signature & get_sig(size_t idx) const;
    bool              has_sig(const pgp::SigID &id) const;
    Signature &       replace_sig(const pgp::SigID &id, const pgp::pkt::Signature &newsig);
    Signature &       get_sig(const pgp::SigID &id);
    const Signature & get_sig(const pgp::SigID &id) const;
    Signature &       add_sig(const pgp::pkt::Signature &sig,
                              size_t                     uid = UserID::None,
                              bool                       begin = false);
    bool              del_sig(const pgp::SigID &sigid);
    size_t            del_sigs(const pgp::SigIDs &sigs);
    size_t            keysig_count() const;
    Signature &       get_keysig(size_t idx);
    size_t            uid_count() const;
    UserID &          get_uid(size_t idx);
    const UserID &    get_uid(size_t idx) const;
    UserID &          add_uid(const pgp_transferable_userid_t &uid);
    bool              has_uid(const std::string &uid) const;
    uint32_t          uid_idx(const pgp_userid_pkt_t &uid) const;
    void              del_uid(size_t idx);
    bool              has_primary_uid() const;
    uint32_t          get_primary_uid() const;
    bool              revoked() const;
    const Revocation &revocation() const;
    void              clear_revokes();
    void              add_revoker(const pgp::Fingerprint &revoker);
    bool              has_revoker(const pgp::Fingerprint &revoker) const;
    size_t            revoker_count() const;
    const pgp::Fingerprint &get_revoker(size_t idx) const;

    const pgp_key_pkt_t &   pkt() const noexcept;
    pgp_key_pkt_t &         pkt() noexcept;
    void                    set_pkt(const pgp_key_pkt_t &pkt);
    const pgp::KeyMaterial *material() const noexcept;
    pgp::KeyMaterial *      material() noexcept;

    pgp_pubkey_alg_t alg() const noexcept;
    pgp_curve_t      curve() const;
    pgp_version_t    version() const noexcept;
    pgp_pkt_type_t   type() const noexcept;
    bool             encrypted() const noexcept;
    uint8_t          flags() const noexcept;
    bool             can_sign() const noexcept;
    bool             can_certify() const noexcept;
    bool             can_encrypt() const noexcept;
    bool             has_secret() const noexcept;
#if defined(ENABLE_PQC)
    bool is_pqc_alg() const;
#endif
    /**
     * @brief Check whether key is usable for the specified operation.
     *
     * @param op operation to check.
     * @param if_secret check whether secret part of this key could be usable for op.
     * @return true if key (or corresponding secret key) is usable or false otherwise.
     */
    bool usable_for(pgp_op_t op, bool if_secret = false) const;
    /** @brief Get key's expiration time in seconds. If 0 then it doesn't expire. */
    uint32_t expiration() const noexcept;
    /** @brief Check whether key is expired. Must be validated before that. */
    bool expired() const noexcept;
    /** @brief Get key's creation time in seconds since Jan, 1 1970. */
    uint32_t creation() const noexcept;
    bool     is_public() const noexcept;
    bool     is_secret() const noexcept;
    bool     is_primary() const noexcept;
    bool     is_subkey() const noexcept;
    /** @brief check if a key is currently locked, i.e. secret fields are not decrypted.
     *  Note: Key locking does not apply to unprotected keys.
     */
    bool is_locked() const noexcept;
    /** @brief check if a key is currently protected, i.e. its secret data is encrypted */
    bool is_protected() const noexcept;

    bool valid() const noexcept;
    bool validated() const noexcept;
    /** @brief return time till which key is considered to be valid */
    uint64_t valid_till() const noexcept;
    /** @brief check whether key was/will be valid at the specified time */
    bool valid_at(uint64_t timestamp) const noexcept;

    /** @brief Get key's id */
    const pgp::KeyID &keyid() const noexcept;
    /** @brief Get key's fingerprint */
    const pgp::Fingerprint &fp() const noexcept;
    /** @brief Get key's grip */
    const pgp::KeyGrip &grip() const noexcept;
    /** @brief Get primary key's fingerprint for the subkey, if it is available.
     *         Note: will throw if it is not available, use has_primary_fp() to check.
     */
    const pgp::Fingerprint &primary_fp() const;
    /** @brief Check whether key has primary key's fingerprint */
    bool has_primary_fp() const noexcept;
    /** @brief Clean primary_fp */
    void unset_primary_fp() noexcept;
    /** @brief Link key with subkey via primary_fp and subkey_fps list */
    void link_subkey_fp(Key &subkey);
    /**
     * @brief Add subkey fp to key's list.
     *        Note: this function will check for duplicates.
     */
    void add_subkey_fp(const pgp::Fingerprint &fp);
    /** @brief Get the number of pgp key's subkeys. */
    size_t subkey_count() const noexcept;
    /** @brief Remove subkey fingerprint from key's list. */
    void remove_subkey_fp(const pgp::Fingerprint &fp);
    /**
     *  @brief Get the pgp key's subkey fingerprint
     *  @return fingerprint or throws std::out_of_range exception
     */
    const pgp::Fingerprint & get_subkey_fp(size_t idx) const;
    const pgp::Fingerprints &subkey_fps() const;

    size_t           rawpkt_count() const;
    RawPacket &      rawpkt();
    const RawPacket &rawpkt() const;
    void             set_rawpkt(const RawPacket &src);
    /** @brief write secret key data to the rawpkt, optionally encrypting with password */
    bool write_sec_rawpkt(pgp_key_pkt_t &    seckey,
                          const std::string &password,
                          SecurityContext &  ctx);

    /** @brief Unlock a key, i.e. decrypt its secret data so it can be used for
     *         signing/decryption.
     *         Note: Key locking does not apply to unprotected keys.
     *
     *  @param pass_provider the password provider that may be used to unlock the key
     *  @param op operation for which secret key should be unloacked
     *  @return true if the key was unlocked, false otherwise
     **/
    bool unlock(const pgp_password_provider_t &provider, pgp_op_t op = PGP_OP_UNLOCK);
    /** @brief Lock a key, i.e. cleanup decrypted secret data.
     *  Note: Key locking does not apply to unprotected keys.
     *
     *  @param key the key
     *  @return true if the key was locked, false otherwise
     **/
    bool lock() noexcept;
    /** @brief Add protection to an unlocked key, i.e. encrypt its secret data with specified
     *         parameters. */
    bool protect(const rnp_key_protection_params_t &protection,
                 const pgp_password_provider_t &    password_provider,
                 SecurityContext &                  ctx);
    /** @brief Add/change protection of a key */
    bool protect(pgp_key_pkt_t &                    decrypted,
                 const rnp_key_protection_params_t &protection,
                 const std::string &                new_password,
                 SecurityContext &                  ctx);
    /** @brief Remove protection from a key, i.e. leave secret fields unencrypted */
    bool unprotect(const pgp_password_provider_t &password_provider, SecurityContext &ctx);

    /** @brief Write key's packets to the output. */
    void write(pgp_dest_t &dst) const;
    /**
     * @brief Write OpenPGP key packets (including subkeys) to the specified stream
     *
     * @param dst stream to write packets
     * @param keyring keyring, which will be searched for subkeys. Pass NULL to skip subkeys.
     * @return void, but error may be checked via dst.werr
     */
    void write_xfer(pgp_dest_t &dst, const KeyStore *keyring = NULL) const;
    /**
     * @brief Export key with subkey as it is required by Autocrypt (5-packet sequence: key,
     * uid, sig, subkey, sig).
     *
     * @param dst stream to write packets
     * @param sub subkey
     * @param uid index of uid to export
     * @return true on success or false otherwise
     */
    bool write_autocrypt(pgp_dest_t &dst, Key &sub, uint32_t uid);
    /**
     * @brief Write key to vector.
     */
    std::vector<uint8_t> write_vec() const;
    /**
     * @brief Get the latest valid self-signature with information about the primary key for
     *        the specified uid (including the special cases). It could be userid certification
     *        or direct-key signature.
     *
     * @param uid uid for which latest self-signature should be returned,
     *            UserID::None for direct-key signature,
     *            UserID::Primary for any primary key,
     *            UserID::Any for any uid.
     * @param validated set to true whether signature must be validated
     * @return pointer to signature object or NULL if failed/not found.
     */
    Signature *latest_selfsig(uint32_t uid, bool validated = true);

    /**
     * @brief Get the latest valid subkey binding. Should be called on subkey.
     *
     * @param validated set to true whether binding signature must be validated
     * @return pointer to signature object or NULL if failed/not found.
     */
    Signature *latest_binding(bool validated = true);

    /** @brief Returns true if signature is produced by the key itself. */
    bool is_signer(const Signature &sig) const;

    /** @brief Returns true if key is expired according to sig. */
    bool expired_with(const Signature &sig, uint64_t at) const;

    /** @brief Check whether signature is key's self certification. */
    bool is_self_cert(const Signature &sig) const;

    /** @brief Check whether signature is key's direct-key self-signature */
    bool is_direct_self(const Signature &sig) const;

    /** @brief Check whether signature is key's/subkey's revocation */
    bool is_revocation(const Signature &sig) const;

    /** @brief Check whether signature is userid revocation */
    bool is_uid_revocation(const Signature &sig) const;

    /** @brief Check whether signature is subkey binding */
    bool is_binding(const Signature &sig) const;

    /**
     * @brief Validate key's signature, assuming that 'this' is a signing key.
     *
     * @param key key or subkey to which signature belongs.
     * @param sig signature to validate.
     * @param ctx Populated security context.
     */
    void validate_sig(const Key &            key,
                      Signature &            sig,
                      const SecurityContext &ctx) const noexcept;

    /**
     * @brief Validate signature, assuming that 'this' is a signing key.
     *
     * @param sinfo populated signature info. Validation results will be stored here.
     * @param hash hash, feed with all signed data except signature trailer.
     * @param ctx Populated security context.
     * @param hdr literal packet header for attached document signatures or NULL otherwise.
     */
    void validate_sig(SignatureInfo &          sinfo,
                      Hash &                   hash,
                      const SecurityContext &  ctx,
                      const pgp_literal_hdr_t *hdr = NULL) const noexcept;

    /**
     * @brief Validate certification.
     *
     * @param sinfo populated signature info. Validation results will be stored here.
     * @param key key packet to which certification belongs.
     * @param uid userid which is bound by certification to the key packet.
     */
    void validate_cert(SignatureInfo &         sinfo,
                       const pgp_key_pkt_t &   key,
                       const pgp_userid_pkt_t &uid,
                       const SecurityContext & ctx) const;

    /**
     * @brief Validate subkey binding.
     *
     * @param sinfo populated signature info. Validation results will be stored here.
     * @param subkey subkey packet.
     */
    void validate_binding(SignatureInfo &        sinfo,
                          const Key &            subkey,
                          const SecurityContext &ctx) const;

    /**
     * @brief Validate subkey revocation.
     *
     * @param sinfo populated signature info. Validation results will be stored here.
     * @param subkey subkey packet.
     */
    void validate_sub_rev(SignatureInfo &        sinfo,
                          const pgp_key_pkt_t &  subkey,
                          const SecurityContext &ctx) const;

    /**
     * @brief Validate direct-key signature.
     *
     * @param sinfo populated signature info. Validation results will be stored here.
     */
    void validate_direct(SignatureInfo &sinfo, const SecurityContext &ctx) const;

    /**
     * @brief Validate key revocation.
     *
     * @param sinfo populated signature info. Validation results will be stored here.
     * @param key key to which revocation belongs.
     */
    void validate_key_rev(SignatureInfo &        sinfo,
                          const pgp_key_pkt_t &  key,
                          const SecurityContext &ctx) const;

    void validate_self_signatures(const SecurityContext &ctx);
    void validate_self_signatures(Key &primary, const SecurityContext &ctx);

    /*
     * @brief Validate designated revocations. As those are issued by another key, this is
     *        handled differently from self-signatures as requires access to the whole keyring.
     */
    bool validate_desig_revokes(KeyStore &keyring);
    void validate(KeyStore &keyring);
    void validate_subkey(Key *primary, const SecurityContext &ctx);
    void revalidate(KeyStore &keyring);
    void mark_valid();
    /**
     * @brief Fill common signature parameters, assuming that current key is a signing one.
     * @param sig signature to init.
     * @param hash hash algorithm to use (may be changed if it is not suitable for public key
     *             algorithm).
     * @param creation signature's creation time.
     * @param version signature version
     */
    void sign_init(RNG &                rng,
                   pgp::pkt::Signature &sig,
                   pgp_hash_alg_t       hash,
                   uint64_t             creation,
                   pgp_version_t        version) const;

    /**
     * @brief Calculate a certification and fill signature material.
     *        Note: secret key must be unlocked before calling this function.
     *
     * @param key key packet to sign. May be both public and secret. Could be signing key's
     *            packet for self-signature, or any other one for cross-key certification.
     * @param uid uid to certify.
     * @param sig signature, pre-populated with all of the required data, except the
     *            signature material.
     */
    void sign_cert(const pgp_key_pkt_t &   key,
                   const pgp_userid_pkt_t &uid,
                   pgp::pkt::Signature &   sig,
                   SecurityContext &       ctx);

    /**
     * @brief Calculate direct-key signature.
     *        Note: secret key must be unlocked before calling this function.
     *
     * @param key key packet to sign. May be both public and secret.
     * @param sig signature, pre-populated with all of the required data, except the
     *            signature material.
     */
    void sign_direct(const pgp_key_pkt_t &key, pgp::pkt::Signature &sig, SecurityContext &ctx);

    /**
     * @brief Calculate subkey or primary key binding.
     *        Note: this will not embed primary key binding for the signing subkey, it should
     *        be added by the caller.
     *
     * @param key subkey or primary key packet, may be both public or secret.
     * @param sig signature, pre-populated with all of the required data, except the
     *            signature material.
     */
    void sign_binding(const pgp_key_pkt_t &key,
                      pgp::pkt::Signature &sig,
                      SecurityContext &    ctx);

    /**
     * @brief Calculate subkey binding.
     *        Note: secret key must be unlocked before calling this function. If subsign is
     *        true then subkey must be secret and unlocked as well so function can calculate
     *        primary key binding.
     *
     * @param sub subkey to bind to the primary key. If subsign is true then must be unlocked
     *            secret key.
     * @param sig signature, pre-populated with all of the required data, except the
     *            signature material.
     */
    void sign_subkey_binding(Key &                sub,
                             pgp::pkt::Signature &sig,
                             SecurityContext &    ctx,
                             bool                 subsign = false);

    /**
     * @brief Generate key or subkey revocation signature.
     *
     * @param revoke revocation information.
     * @param key key or subkey packet to revoke.
     * @param sig object to store revocation signature. Will be populated in method call.
     */
    void gen_revocation(const Revocation &   rev,
                        pgp_hash_alg_t       hash,
                        const pgp_key_pkt_t &key,
                        pgp::pkt::Signature &sig,
                        SecurityContext &    ctx);

#if defined(ENABLE_CRYPTO_REFRESH)
    /**
     * @brief Add a direct-key self signature
     *        Note: secret key must be unlocked before calling this function.
     *
     * @param cert certification parameters.
     * @param hash hash algorithm to use during signing. See sign_init() for more details.
     * @param ctx  security context.
     * @param pubkey if non-NULL then the direct-key signature will be added to this key as
     *               well.
     */
    void add_direct_sig(CertParams &     cert,
                        pgp_hash_alg_t   hash,
                        SecurityContext &ctx,
                        Key *            pubkey = nullptr);
#endif

    /**
     * @brief Add and certify userid.
     *        Note: secret key must be unlocked before calling this function.
     *
     * @param cert certification and userid parameters.
     * @param hash hash algorithm to use during signing. See sign_init() for more details.
     * @param ctx  security context.
     * @param pubkey if non-NULL then userid and certification will be added to this key as
     *               well.
     */
    void add_uid_cert(CertParams &     cert,
                      pgp_hash_alg_t   hash,
                      SecurityContext &ctx,
                      Key *            pubkey = nullptr);

    /**
     * @brief Calculate and add subkey binding signature.
     *        Note: must be called on the unlocked secret primary key. Calculated signature is
     *        added to the subkey.
     *
     * @param subsec secret subkey.
     * @param subpub subkey's public part (so signature is added to both).
     * @param binding information about subkey to put to the signature.
     * @param hash hash algorithm to use (may be adjusted according to key and subkey
     *             algorithms)
     */
    void add_sub_binding(Key &                subsec,
                         Key &                subpub,
                         const BindingParams &binding,
                         pgp_hash_alg_t       hash,
                         SecurityContext &    ctx);

    /** @brief Refresh internal fields after primary key is updated */
    bool refresh_data(const SecurityContext &ctx);
    /** @brief Refresh internal fields after subkey is updated */
    bool refresh_data(Key *primary, const SecurityContext &ctx);
    /** @brief Refresh revocation status. */
    void refresh_revocations();
    /** @brief Merge primary key with the src, i.e. add all new userids/signatures/subkeys */
    bool merge(const Key &src);
    /** @brief Merge subkey with the source, i.e. add all new signatures */
    bool merge(const Key &src, Key *primary);
};

class KeyLocker {
    bool lock_;
    Key &key_;

  public:
    KeyLocker(Key &key) : lock_(key.is_locked()), key_(key)
    {
    }

    ~KeyLocker()
    {
        if (lock_ && !key_.is_locked()) {
            key_.lock();
        }
    }
};

pgp_key_pkt_t *pgp_decrypt_seckey_pgp(const RawPacket &    raw,
                                      const pgp_key_pkt_t &key,
                                      const char *         password);

pgp_key_pkt_t *pgp_decrypt_seckey(const Key &,
                                  const pgp_password_provider_t &,
                                  const pgp_password_ctx_t &);

bool pgp_key_set_expiration(Key *                          key,
                            Key *                          signer,
                            uint32_t                       expiry,
                            const pgp_password_provider_t &prov,
                            SecurityContext &              ctx);

bool pgp_subkey_set_expiration(Key *                          sub,
                               Key *                          primsec,
                               Key *                          secsub,
                               uint32_t                       expiry,
                               const pgp_password_provider_t &prov,
                               SecurityContext &              ctx);

/** Find a key or it's subkey, suitable for a particular operation
 *
 *  If the key passed is suitable, it will be returned.
 *  Otherwise, its subkeys (if it is a primary w/subs)
 *  will be checked. NULL will be returned if no suitable
 *  key is found.
 *
 *  @param op the operation for which the key should be suitable
 *  @param key the key
 *  @param key_provider the key provider. This will be used
 *         if/when subkeys are checked.
 *  @param no_primary set true if only subkeys must be returned
 *
 *  @returns key or last created subkey with desired usage flag
 *           set or NULL if not found
 */
Key *find_suitable_key(pgp_op_t     op,
                       Key *        key,
                       KeyProvider *key_provider,
                       bool         no_primary = false,
                       bool         pref_pqc_sub = false);

} // namespace rnp

pgp_key_flags_t pgp_pk_alg_capabilities(pgp_pubkey_alg_t alg);

#endif // RNP_KEY_HPP
