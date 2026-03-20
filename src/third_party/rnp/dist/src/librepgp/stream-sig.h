/*
 * Copyright (c) 2018-2022, [Ribose Inc](https://www.ribose.com).
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

#ifndef STREAM_SIG_H_
#define STREAM_SIG_H_

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include "rnp.h"
#include "stream-common.h"
#include "stream-packet.h"
#include "sig_subpacket.hpp"
#include "sig_material.hpp"

namespace pgp {
namespace pkt {

class Signature {
  private:
    pgp_sig_type_t       type_;
    std::vector<uint8_t> preferred(sigsub::Type type) const;
    void                 set_preferred(const std::vector<uint8_t> &data, sigsub::Type type);
    rnp_result_t         parse_v2v3(pgp_packet_body_t &pkt);
    rnp_result_t         parse_v4up(pgp_packet_body_t &pkt);
    bool                 get_subpkt_len(pgp_packet_body_t &pkt, size_t &len);
    bool                 parse_subpackets(uint8_t *buf, size_t len, bool hashed);
    static bool          version_supported(pgp_version_t version);

    const sigsub::RevocationKey *revoker_subpkt() const noexcept;

  public:
    pgp_version_t version;
    /* common v3 and v4 fields */
    pgp_pubkey_alg_t       palg;
    pgp_hash_alg_t         halg;
    std::array<uint8_t, 2> lbits{};
    std::vector<uint8_t>   hashed_data;
    std::vector<uint8_t>   material_buf; /* raw signature material */

    /* v3 - only fields */
    uint32_t creation_time;
    KeyID    signer{};

    /* common v4, v5 and v6 fields */
    sigsub::List subpkts;

#if defined(ENABLE_CRYPTO_REFRESH)
    /* v6 - only fields */
    std::vector<uint8_t> salt;
#endif

    Signature()
        : type_(PGP_SIG_BINARY), version(PGP_VUNKNOWN), palg(PGP_PKA_NOTHING),
          halg(PGP_HASH_UNKNOWN), creation_time(0){};
    bool operator==(const Signature &src) const;
    bool operator!=(const Signature &src) const;

    /* @brief Get signature's type */
    pgp_sig_type_t
    type() const
    {
        return type_;
    };
    void
    set_type(pgp_sig_type_t atype)
    {
        type_ = atype;
    };

    bool
    is_document() const
    {
        return (type_ == PGP_SIG_BINARY) || (type_ == PGP_SIG_TEXT);
    };

    /** @brief Calculate the unique signature identifier by hashing signature's fields. */
    SigID get_id() const;

    size_t find_subpkt(uint8_t type, bool hashed = true, size_t skip = 0) const;
    size_t find_subpkt(sigsub::Type type, bool hashed = true, size_t skip = 0) const;
    /**
     * @brief Get v4 and up signature's subpacket of the specified type and hashedness.
     * @param stype subpacket type.
     * @param hashed whether subpacket must be in hashed area on in any area.
     * @return pointer to the subpacket, or nullptr if subpacket was not found.
     */
    sigsub::Raw *      get_subpkt(uint8_t stype, bool hashed = true);
    const sigsub::Raw *get_subpkt(uint8_t stype, bool hashed = true) const;
    sigsub::Raw *      get_subpkt(sigsub::Type type, bool hashed = true);
    const sigsub::Raw *get_subpkt(sigsub::Type type, bool hashed = true) const;

    /* @brief Check whether v4 signature has subpacket of the specified type/hashedness */
    bool has_subpkt(uint8_t stype, bool hashed = true) const;
    /* @brief Check whether signature has signing key id (via v3 field, or v4 key id/key fp
     * subpacket) */
    bool has_keyid() const;
    /**
     * @brief Get signer's key id if available. Availability may be checked via has_keyid().
     * @return signer's key id if available, or empty (zero-filled) keyid otherwise.
     */
    KeyID keyid() const noexcept;
    /** @brief Set the signer's key id for the signature being populated. Version should be set
     *         prior of setting key id. */
    void set_keyid(const KeyID &id);
    /**
     * @brief Check whether signature has valid issuer fingerprint subpacket.
     * @return true if there is one, and it can be safely returned via keyfp() method or false
     *         otherwise.
     */
    bool has_keyfp() const;
    /**
     * @brief Get signing key's fingerprint if it is available. Availability may be checked via
     *        has_keyfp() method.
     * @return fingerprint (or empty zero-size fp in case it is unavailable)
     */
    Fingerprint keyfp() const noexcept;

    /** @brief Set signing key's fingerprint. Works only for signatures with version 4 and up,
     *         so version should be set prior to fingerprint. */
    void set_keyfp(const Fingerprint &fp);

    /**
     * @brief Get signature's creation time
     * @return time in seconds since the Jan 1, 1970 UTC. 0 is the default value and returned
     *         even if creation time is not available
     */
    uint32_t creation() const;

    /**
     * @brief Set signature's creation time
     * @param ctime creation time in seconds since the Jan 1, 1970 UTC.
     */
    void set_creation(uint32_t ctime);

    /**
     * @brief Get the signature's expiration time
     * @return expiration time in seconds since the creation time. 0 if signature never
     * expires.
     */
    uint32_t expiration() const;

    /**
     * @brief Set the signature's expiration time
     * @param etime expiration time
     */
    void set_expiration(uint32_t etime);

    /**
     * @brief Get the key expiration time
     * @return expiration time in seconds since the creation time. 0 if key never expires.
     */
    uint32_t key_expiration() const;

    /**
     * @brief Set the key expiration time
     * @param etime expiration time
     */
    void set_key_expiration(uint32_t etime);

    /**
     * @brief Get the key flags
     * @return byte of key flags. If there is no corresponding subpackets then 0 is returned.
     */
    uint8_t key_flags() const;

    /**
     * @brief Set the key flags
     * @param flags byte of key flags
     */
    void set_key_flags(uint8_t flags);

    /**
     * @brief Get the primary user id flag
     * @return true if user id is marked as primary or false otherwise
     */
    bool primary_uid() const;

    /**
     * @brief Set the primary user id flag
     * @param primary true if user id should be marked as primary
     */
    void set_primary_uid(bool primary);

    /** @brief Get preferred symmetric algorithms if any. If there are no ones then empty
     *         vector is returned. */
    std::vector<uint8_t> preferred_symm_algs() const;

    /** @brief Set the preferred symmetric algorithms. If empty vector is passed then
     *         corresponding subpacket is deleted. */
    void set_preferred_symm_algs(const std::vector<uint8_t> &algs);

    /** @brief Get preferred hash algorithms if any. If there are no ones then empty vector is
     *         returned.*/
    std::vector<uint8_t> preferred_hash_algs() const;

    /** @brief Set the preferred hash algorithms. If empty vector is passed then corresponding
     *         subpacket is deleted. */
    void set_preferred_hash_algs(const std::vector<uint8_t> &algs);

    /** @brief Get preferred compression algorithms if any. If there are no ones then empty
     *         vector is returned.*/
    std::vector<uint8_t> preferred_z_algs() const;

    /** @brief Set the preferred compression algorithms. If empty vector is passed then
     *         corresponding subpacket is deleted. */
    void set_preferred_z_algs(const std::vector<uint8_t> &algs);

#if defined(ENABLE_CRYPTO_REFRESH)
    /** @brief Get preferred AEAD algorithms if any. If there are no ones then empty
     *         vector is returned.*/
    std::vector<uint8_t> preferred_aead_algs() const;

    /** @brief Set the preferred AEAD algorithms. If empty vector is passed then
     *         corresponding subpacket is deleted. */
    void set_preferred_aead_algs(const std::vector<uint8_t> &algs);
#endif

    /** @brief Get key server preferences flags. If subpacket is not available then 0 is
     *         returned. */
    uint8_t key_server_prefs() const;

    /** @brief Set key server preferences flags. */
    void set_key_server_prefs(uint8_t prefs);

    /** @brief Get preferred key server URI, if available. Otherwise empty string is returned.
     */
    std::string key_server() const;

    /** @brief Set preferred key server URI. If it is empty string then subpacket is deleted if
     *         it is available. */
    void set_key_server(const std::string &uri);

    /** @brief Get trust level, if available. Otherwise will return 0. See RFC 4880, 5.2.3.14.
     *         for the detailed information on trust level and amount.
     */
    uint8_t trust_level() const;

    /** @brief Get trust amount, if available. Otherwise will return 0. See RFC 4880, 5.2.3.14.
     *         for the detailed information on trust level and amount.
     */
    uint8_t trust_amount() const;

    /** @brief Set the trust level and amount. See RFC 4880, 5.2.3.14.
     *         for the detailed information on trust level and amount.
     */
    void set_trust(uint8_t level, uint8_t amount);

    /** @brief check whether signature is revocable. True by default.
     */
    bool revocable() const;

    /** @brief Set the signature's revocability status.
     */
    void set_revocable(bool status);

    /** @brief Get the key/subkey revocation reason in humand-readable form. If there is no
     *         revocation reason subpacket, then empty string will be returned.
     */
    std::string revocation_reason() const;

    /** @brief Get the key/subkey revocation code. If there is no revocation reason subpacket,
     *         then PGP_REVOCATION_NO_REASON will be rerturned. See the RFC 4880, 5.2.3.24 for
     *         the detailed explanation.
     */
    pgp_revocation_type_t revocation_code() const;

    /**
     * @brief Check whether signature has revocation reason and code subpacket.
     */
    bool has_revocation_reason() const;

    /** @brief Set the revocation reason and code for key/subkey revocation signature. See the
     *         RFC 4880, 5.2.3.24 for the detailed explanation.
     */
    void set_revocation_reason(pgp_revocation_type_t code, const std::string &reason);

    uint32_t key_get_features() const;

    /**
     * @brief Check whether signer's key supports certain feature(s). Makes sense only for
     *        self-signature, for more details see the RFC 4880bis, 5.2.3.25. If there is
     *        no corresponding subpacket then false will be returned.
     * @param flags one or more flags, combined via bitwise OR operation.
     * @return true if key is claimed to support all of the features listed in flags, or false
     *         otherwise
     */
    bool key_has_features(uint32_t flags) const;

    /**
     * @brief Set the features supported by the signer's key, makes sense only for
     *        self-signature. For more details see the RFC 4880bis, 5.2.3.25.
     * @param flags one or more flags, combined via bitwise OR operation.
     */
    void set_key_features(uint32_t flags);

    /** @brief Get signer's user id, if available. Otherwise empty string is returned. See the
     *         RFC 4880bis, 5.2.3.23 for details.
     */
    std::string signer_uid() const;

    /**
     * @brief Set the signer's uid, responsible for the signature creation. See the RFC
     *        4880bis, 5.2.3.23 for details.
     */
    void set_signer_uid(const std::string &uid);

    /**
     * @brief Add notation.
     */
    void add_notation(const std::string &         name,
                      const std::vector<uint8_t> &value,
                      bool                        human = true,
                      bool                        critical = false);

    /**
     * @brief Add human-readable notation.
     */
    void add_notation(const std::string &name,
                      const std::string &value,
                      bool               critical = false);

    /**
     * @brief Set the embedded signature.
     * @param esig populated and calculated embedded signature.
     */
    void set_embedded_sig(const Signature &esig);

    /**
     * @brief Check whether signature includes revocation key subpacket.
     */
    bool has_revoker() const noexcept;

    /**
     * @brief Get the revocation key fingerprint, if it is available. Otherwise empty
     * fingerprint will be returned.
     */
    Fingerprint revoker() const noexcept;

    /**
     * @brief Set the revocation key.
     *
     * @param revoker revoker's key packet.
     */
    void set_revoker(const rnp::Key &revoker, bool sensitive = false);

    /**
     * @brief Add subpacket to v4 and up signature
     * @param sub pointer to the subpacket object.
     * @param replace replace already existing subpacket of the specified type if any
     * @return reference to the subpacket structure or throws an exception
     */
    void add_subpkt(std::unique_ptr<sigsub::Raw> &&sub, bool replace = true);

    /**
     * @brief Remove signature's subpacket
     * @param idx Index of the subpacket in list.
     */
    void remove_subpkt(size_t idx);

    /**
     * @brief Check whether signature packet matches one-pass signature packet.
     * @param onepass reference to the read one-pass signature packet
     * @return true if sig corresponds to onepass or false otherwise
     */
    bool matches_onepass(const pgp_one_pass_sig_t &onepass) const;

    /**
     * @brief Parse signature body (i.e. without checking the packet header).
     *
     * @param pkt packet body with data.
     * @return RNP_SUCCESS or error code if failed. May also throw an exception.
     */
    rnp_result_t parse(pgp_packet_body_t &pkt);

    /**
     * @brief Parse signature packet from source.
     *
     * @param src source with data.
     * @return RNP_SUCCESS or error code if failed. May also throw an exception.
     */
    rnp_result_t parse(pgp_source_t &src);

    /**
     * @brief Parse signature material, stored in the signature in raw.
     *
     * @param material on success parsed material will be stored here.
     * @return true on success or false otherwise. May also throw an exception.
     */
    std::unique_ptr<SigMaterial> parse_material() const;

    /**
     * @brief Write signature to the destination. May throw an exception.
     */
    void                 write(pgp_dest_t &dst, bool hdr = true) const;
    std::vector<uint8_t> write(bool hdr = true) const;

    /**
     * @brief Write the signature material's raw representation. May throw an exception.
     *
     * @param material populated signature material.
     */
    void write_material(const SigMaterial &material);

    /**
     * @brief Fill signature's hashed data. This includes all the fields from signature which
     * are hashed after the previous document or key fields.
     */
    void fill_hashed_data();
};

using Signatures = std::vector<Signature>;

} // namespace pkt
} // namespace pgp

/**
 * @brief Hash key packet. Used in signatures and v4 fingerprint calculation.
 *        Throws exception on error.
 * @param key key packet, must be populated
 * @param hash initialized hash context
 * @param pgpver for fingerprint calculation, the key version is required,
 * otherwise the signature version is required
 */
void signature_hash_key(const pgp_key_pkt_t &key, rnp::Hash &hash, pgp_version_t pgpver);

void signature_hash_userid(const pgp_userid_pkt_t &uid, rnp::Hash &hash, pgp_version_t sigver);

std::unique_ptr<rnp::Hash> signature_hash_certification(const pgp::pkt::Signature &sig,
                                                        const pgp_key_pkt_t &      key,
                                                        const pgp_userid_pkt_t &   userid);

std::unique_ptr<rnp::Hash> signature_hash_binding(const pgp::pkt::Signature &sig,
                                                  const pgp_key_pkt_t &      key,
                                                  const pgp_key_pkt_t &      subkey);

std::unique_ptr<rnp::Hash> signature_hash_direct(const pgp::pkt::Signature &sig,
                                                 const pgp_key_pkt_t &      key);

/**
 * @brief Parse stream with signatures to the signatures list.
 *        Can handle binary or armored stream with signatures, including stream with multiple
 * armored signatures.
 *
 * @param src signatures stream, cannot be NULL.
 * @param sigs on success parsed signature structures will be put here.
 * @return RNP_SUCCESS or error code otherwise.
 */
rnp_result_t process_pgp_signatures(pgp_source_t &src, pgp::pkt::Signatures &sigs);

#endif
