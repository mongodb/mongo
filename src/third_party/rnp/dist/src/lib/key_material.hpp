/*
 * Copyright (c) 2024 [Ribose Inc](https://www.ribose.com).
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

#ifndef RNP_KEY_MATERIAL_HPP_
#define RNP_KEY_MATERIAL_HPP_

#include "types.h"
#include "defaults.h"
#include "enc_material.hpp"
#include "sig_material.hpp"
#include "fingerprint.hpp"

typedef struct pgp_packet_body_t pgp_packet_body_t;

namespace pgp {

class KeyParams {
  public:
    virtual ~KeyParams();
    virtual size_t bits() const noexcept = 0;
    virtual pgp_hash_alg_t
    min_hash() const noexcept
    {
        return PGP_HASH_UNKNOWN;
    }

    virtual void
    check_defaults() noexcept
    {
    }

    static std::unique_ptr<KeyParams> create(pgp_pubkey_alg_t alg);
};

class BitsKeyParams : public KeyParams {
  private:
    size_t bits_;

  public:
    BitsKeyParams(size_t bits) : bits_(bits){};

    size_t
    bits() const noexcept override
    {
        return bits_;
    };

    void
    set_bits(size_t value) noexcept
    {
        bits_ = value;
    };
};

class RSAKeyParams : public BitsKeyParams {
  public:
    RSAKeyParams() : BitsKeyParams(DEFAULT_RSA_NUMBITS){};
};

class DSAKeyParams : public BitsKeyParams {
  private:
    size_t qbits_;

  public:
    DSAKeyParams() : BitsKeyParams(DSA_DEFAULT_P_BITLEN), qbits_(0){};

    size_t
    qbits() const noexcept
    {
        return qbits_;
    };

    void
    set_qbits(size_t value) noexcept
    {
        qbits_ = value;
    };

    void check_defaults() noexcept override;

    pgp_hash_alg_t min_hash() const noexcept override;
};

class EGKeyParams : public BitsKeyParams {
  public:
    EGKeyParams() : BitsKeyParams(DEFAULT_ELGAMAL_NUMBITS){};
};

class ECCKeyParams : public KeyParams {
  private:
    pgp_curve_t curve_;

  public:
    ECCKeyParams(pgp_curve_t curve = PGP_CURVE_UNKNOWN) : curve_(curve){};

    pgp_curve_t
    curve() const noexcept
    {
        return curve_;
    }

    void
    set_curve(const pgp_curve_t value) noexcept
    {
        curve_ = value;
    }

    size_t bits() const noexcept override;
};

class ECDSAKeyParams : public ECCKeyParams {
  public:
    pgp_hash_alg_t min_hash() const noexcept override;
};

#if defined(ENABLE_PQC)
class MlkemEcdhKeyParams : public KeyParams {
  private:
    pgp_pubkey_alg_t alg_;

  public:
    MlkemEcdhKeyParams(pgp_pubkey_alg_t alg) : alg_(alg){};
    size_t bits() const noexcept override;
};

class DilithiumEccKeyParams : public KeyParams {
  private:
    pgp_pubkey_alg_t alg_;

  public:
    DilithiumEccKeyParams(pgp_pubkey_alg_t alg) : alg_(alg){};
    size_t bits() const noexcept override;
};

class SlhdsaKeyParams : public KeyParams {
  private:
    sphincsplus_parameter_t param_;

  public:
    SlhdsaKeyParams() : param_(sphincsplus_simple_128f){};

    sphincsplus_parameter_t
    param() const noexcept
    {
        return param_;
    }

    void
    set_param(sphincsplus_parameter_t value) noexcept
    {
        param_ = value;
    }

    size_t bits() const noexcept override;
};
#endif

class KeyMaterial {
    pgp_validity_t validity_; /* key material validation status */
  protected:
    pgp_pubkey_alg_t alg_;    /* algorithm of the key */
    bool             secret_; /* secret part of the key material is populated */

    virtual void grip_update(rnp::Hash &hash) const = 0;
    virtual bool validate_material(rnp::SecurityContext &ctx, bool reset = true) = 0;
    bool         finish_generate();

  public:
    KeyMaterial(pgp_pubkey_alg_t kalg = PGP_PKA_NOTHING, bool secret = false)
        : validity_({}), alg_(kalg), secret_(secret){};
    virtual ~KeyMaterial();
    virtual std::unique_ptr<KeyMaterial> clone() = 0;

    pgp_pubkey_alg_t      alg() const noexcept;
    bool                  secret() const noexcept;
    void                  validate(rnp::SecurityContext &ctx, bool reset = true);
    const pgp_validity_t &validity() const noexcept;
    void                  set_validity(const pgp_validity_t &val);
    void                  reset_validity();
    bool                  valid() const;
    virtual void          clear_secret() noexcept;
    virtual bool          parse(pgp_packet_body_t &pkt) noexcept = 0;
    virtual bool          parse_secret(pgp_packet_body_t &pkt) noexcept = 0;
    virtual void          write(pgp_packet_body_t &pkt) const = 0;
    virtual void          write_secret(pgp_packet_body_t &pkt) const = 0;
    virtual bool          generate(rnp::SecurityContext &ctx, const KeyParams &params);
    virtual rnp_result_t  encrypt(rnp::SecurityContext &   ctx,
                                  EncMaterial &            out,
                                  const rnp::secure_bytes &data) const;
    virtual rnp_result_t  decrypt(rnp::SecurityContext &ctx,
                                  rnp::secure_bytes &   out,
                                  const EncMaterial &   in) const;
    virtual rnp_result_t  verify(const rnp::SecurityContext &ctx,
                                 const SigMaterial &         sig,
                                 const rnp::secure_bytes &   hash) const;
    virtual rnp_result_t  sign(rnp::SecurityContext &   ctx,
                               SigMaterial &            sig,
                               const rnp::secure_bytes &hash) const;

    /* Pick up hash algorithm, used for signing, to be compatible with key material. */
    virtual pgp_hash_alg_t adjust_hash(pgp_hash_alg_t hash) const;
    virtual bool           sig_hash_allowed(pgp_hash_alg_t hash) const;
    virtual size_t         bits() const noexcept = 0;
    virtual pgp_curve_t    curve() const noexcept;
    KeyGrip                grip() const;

    static std::unique_ptr<KeyMaterial> create(pgp_pubkey_alg_t alg);
    static std::unique_ptr<KeyMaterial> create(pgp_pubkey_alg_t alg, const rsa::Key &key);
    static std::unique_ptr<KeyMaterial> create(const dsa::Key &key);
    static std::unique_ptr<KeyMaterial> create(pgp_pubkey_alg_t alg, const eg::Key &key);
    static std::unique_ptr<KeyMaterial> create(pgp_pubkey_alg_t alg, const ec::Key &key);
};

class RSAKeyMaterial : public KeyMaterial {
  protected:
    rsa::Key key_;

    void grip_update(rnp::Hash &hash) const override;
    bool validate_material(rnp::SecurityContext &ctx, bool reset) override;

  public:
    RSAKeyMaterial(pgp_pubkey_alg_t kalg) : KeyMaterial(kalg), key_{} {};
    RSAKeyMaterial(pgp_pubkey_alg_t kalg, const rsa::Key &key, bool secret = false)
        : KeyMaterial(kalg, secret), key_(key){};
    std::unique_ptr<KeyMaterial> clone() override;

    void         clear_secret() noexcept override;
    bool         parse(pgp_packet_body_t &pkt) noexcept override;
    bool         parse_secret(pgp_packet_body_t &pkt) noexcept override;
    void         write(pgp_packet_body_t &pkt) const override;
    void         write_secret(pgp_packet_body_t &pkt) const override;
    bool         generate(rnp::SecurityContext &ctx, const KeyParams &params) override;
    rnp_result_t encrypt(rnp::SecurityContext &   ctx,
                         EncMaterial &            out,
                         const rnp::secure_bytes &data) const override;
    rnp_result_t decrypt(rnp::SecurityContext &ctx,
                         rnp::secure_bytes &   out,
                         const EncMaterial &   in) const override;
    rnp_result_t verify(const rnp::SecurityContext &ctx,
                        const SigMaterial &         sig,
                        const rnp::secure_bytes &   hash) const override;
    rnp_result_t sign(rnp::SecurityContext &   ctx,
                      SigMaterial &            sig,
                      const rnp::secure_bytes &hash) const override;

    void   set_secret(const mpi &d, const mpi &p, const mpi &q, const mpi &u);
    size_t bits() const noexcept override;

    const mpi &n() const noexcept;
    const mpi &e() const noexcept;
    const mpi &d() const noexcept;
    const mpi &p() const noexcept;
    const mpi &q() const noexcept;
    const mpi &u() const noexcept;
};

class DSAKeyMaterial : public KeyMaterial {
  protected:
    dsa::Key key_;

    void grip_update(rnp::Hash &hash) const override;
    bool validate_material(rnp::SecurityContext &ctx, bool reset) override;

  public:
    DSAKeyMaterial() : KeyMaterial(PGP_PKA_DSA), key_{} {};
    DSAKeyMaterial(const dsa::Key &key, bool secret = false)
        : KeyMaterial(PGP_PKA_DSA, secret), key_(key){};
    std::unique_ptr<KeyMaterial> clone() override;

    void           clear_secret() noexcept override;
    bool           parse(pgp_packet_body_t &pkt) noexcept override;
    bool           parse_secret(pgp_packet_body_t &pkt) noexcept override;
    void           write(pgp_packet_body_t &pkt) const override;
    void           write_secret(pgp_packet_body_t &pkt) const override;
    bool           generate(rnp::SecurityContext &ctx, const KeyParams &params) override;
    rnp_result_t   verify(const rnp::SecurityContext &ctx,
                          const SigMaterial &         sig,
                          const rnp::secure_bytes &   hash) const override;
    rnp_result_t   sign(rnp::SecurityContext &   ctx,
                        SigMaterial &            sig,
                        const rnp::secure_bytes &hash) const override;
    pgp_hash_alg_t adjust_hash(pgp_hash_alg_t hash) const override;
    void           set_secret(const mpi &x);
    size_t         bits() const noexcept override;
    size_t         qbits() const noexcept;

    const mpi &p() const noexcept;
    const mpi &q() const noexcept;
    const mpi &g() const noexcept;
    const mpi &y() const noexcept;
    const mpi &x() const noexcept;
};

class EGKeyMaterial : public KeyMaterial {
  protected:
    eg::Key key_;

    void grip_update(rnp::Hash &hash) const override;
    bool validate_material(rnp::SecurityContext &ctx, bool reset) override;

  public:
    EGKeyMaterial(pgp_pubkey_alg_t kalg) : KeyMaterial(kalg), key_{} {};
    EGKeyMaterial(pgp_pubkey_alg_t kalg, const eg::Key &key, bool secret = false)
        : KeyMaterial(kalg, secret), key_(key){};
    std::unique_ptr<KeyMaterial> clone() override;

    void         clear_secret() noexcept override;
    bool         parse(pgp_packet_body_t &pkt) noexcept override;
    bool         parse_secret(pgp_packet_body_t &pkt) noexcept override;
    void         write(pgp_packet_body_t &pkt) const override;
    void         write_secret(pgp_packet_body_t &pkt) const override;
    bool         generate(rnp::SecurityContext &ctx, const KeyParams &params) override;
    rnp_result_t encrypt(rnp::SecurityContext &   ctx,
                         EncMaterial &            out,
                         const rnp::secure_bytes &data) const override;
    rnp_result_t decrypt(rnp::SecurityContext &ctx,
                         rnp::secure_bytes &   out,
                         const EncMaterial &   in) const override;
    rnp_result_t verify(const rnp::SecurityContext &ctx,
                        const SigMaterial &         sig,
                        const rnp::secure_bytes &   hash) const override;

    void   set_secret(const mpi &x);
    size_t bits() const noexcept override;

    const mpi &p() const noexcept;
    const mpi &g() const noexcept;
    const mpi &y() const noexcept;
    const mpi &x() const noexcept;
};

class ECKeyMaterial : public KeyMaterial {
  protected:
    ec::Key key_;

    void         grip_update(rnp::Hash &hash) const override;
    rnp_result_t check_curve(size_t hash_len) const;

  public:
    ECKeyMaterial(pgp_pubkey_alg_t kalg) : KeyMaterial(kalg), key_{} {};
    ECKeyMaterial(pgp_pubkey_alg_t kalg, const ec::Key &key, bool secret = false)
        : KeyMaterial(kalg, secret), key_(key){};

    void        clear_secret() noexcept override;
    bool        parse(pgp_packet_body_t &pkt) noexcept override;
    bool        parse_secret(pgp_packet_body_t &pkt) noexcept override;
    void        write(pgp_packet_body_t &pkt) const override;
    void        write_secret(pgp_packet_body_t &pkt) const override;
    bool        generate(rnp::SecurityContext &ctx, const KeyParams &params) override;
    void        set_secret(const mpi &x);
    size_t      bits() const noexcept override;
    pgp_curve_t curve() const noexcept override;

    const mpi &p() const noexcept;
    const mpi &x() const noexcept;
};

class ECDSAKeyMaterial : public ECKeyMaterial {
  protected:
    bool validate_material(rnp::SecurityContext &ctx, bool reset) override;

  public:
    ECDSAKeyMaterial() : ECKeyMaterial(PGP_PKA_ECDSA){};
    ECDSAKeyMaterial(const ec::Key &key, bool secret = false)
        : ECKeyMaterial(PGP_PKA_ECDSA, key, secret){};
    std::unique_ptr<KeyMaterial> clone() override;

    rnp_result_t   verify(const rnp::SecurityContext &ctx,
                          const SigMaterial &         sig,
                          const rnp::secure_bytes &   hash) const override;
    rnp_result_t   sign(rnp::SecurityContext &   ctx,
                        SigMaterial &            sig,
                        const rnp::secure_bytes &hash) const override;
    pgp_hash_alg_t adjust_hash(pgp_hash_alg_t hash) const override;
};

class ECDHKeyMaterial : public ECKeyMaterial {
  protected:
    bool validate_material(rnp::SecurityContext &ctx, bool reset) override;

  public:
    ECDHKeyMaterial() : ECKeyMaterial(PGP_PKA_ECDH){};
    ECDHKeyMaterial(const ec::Key &key, bool secret = false)
        : ECKeyMaterial(PGP_PKA_ECDH, key, secret){};
    std::unique_ptr<KeyMaterial> clone() override;

    bool         parse(pgp_packet_body_t &pkt) noexcept override;
    void         write(pgp_packet_body_t &pkt) const override;
    bool         generate(rnp::SecurityContext &ctx, const KeyParams &params) override;
    rnp_result_t encrypt(rnp::SecurityContext &   ctx,
                         EncMaterial &            out,
                         const rnp::secure_bytes &data) const override;
    rnp_result_t decrypt(rnp::SecurityContext &ctx,
                         rnp::secure_bytes &   out,
                         const EncMaterial &   in) const override;

    pgp_hash_alg_t kdf_hash_alg() const noexcept;
    pgp_symm_alg_t key_wrap_alg() const noexcept;
    bool           x25519_bits_tweaked() const noexcept;
    bool           x25519_tweak_bits() noexcept;
};

class EDDSAKeyMaterial : public ECKeyMaterial {
  protected:
    bool validate_material(rnp::SecurityContext &ctx, bool reset) override;

  public:
    EDDSAKeyMaterial() : ECKeyMaterial(PGP_PKA_EDDSA){};
    EDDSAKeyMaterial(const ec::Key &key, bool secret = false)
        : ECKeyMaterial(PGP_PKA_EDDSA, key, secret){};
    std::unique_ptr<KeyMaterial> clone() override;

    bool         generate(rnp::SecurityContext &ctx, const KeyParams &params) override;
    rnp_result_t verify(const rnp::SecurityContext &ctx,
                        const SigMaterial &         sig,
                        const rnp::secure_bytes &   hash) const override;
    rnp_result_t sign(rnp::SecurityContext &   ctx,
                      SigMaterial &            sig,
                      const rnp::secure_bytes &hash) const override;
};

class SM2KeyMaterial : public ECKeyMaterial {
  protected:
    bool validate_material(rnp::SecurityContext &ctx, bool reset) override;

  public:
    SM2KeyMaterial() : ECKeyMaterial(PGP_PKA_SM2){};
    SM2KeyMaterial(const ec::Key &key, bool secret = false)
        : ECKeyMaterial(PGP_PKA_SM2, key, secret){};
    std::unique_ptr<KeyMaterial> clone() override;

    rnp_result_t encrypt(rnp::SecurityContext &   ctx,
                         EncMaterial &            out,
                         const rnp::secure_bytes &data) const override;
    rnp_result_t decrypt(rnp::SecurityContext &ctx,
                         rnp::secure_bytes &   out,
                         const EncMaterial &   in) const override;
    rnp_result_t verify(const rnp::SecurityContext &ctx,
                        const SigMaterial &         sig,
                        const rnp::secure_bytes &   hash) const override;
    rnp_result_t sign(rnp::SecurityContext &   ctx,
                      SigMaterial &            sig,
                      const rnp::secure_bytes &hash) const override;
    void         compute_za(rnp::Hash &hash) const;
};

#if defined(ENABLE_CRYPTO_REFRESH)
class Ed25519KeyMaterial : public KeyMaterial {
    pgp_ed25519_key_t key_;

  protected:
    void grip_update(rnp::Hash &hash) const override;
    bool validate_material(rnp::SecurityContext &ctx, bool reset) override;

  public:
    Ed25519KeyMaterial() : KeyMaterial(PGP_PKA_ED25519), key_{} {};
    std::unique_ptr<KeyMaterial> clone() override;

    void         clear_secret() noexcept override;
    bool         parse(pgp_packet_body_t &pkt) noexcept override;
    bool         parse_secret(pgp_packet_body_t &pkt) noexcept override;
    void         write(pgp_packet_body_t &pkt) const override;
    void         write_secret(pgp_packet_body_t &pkt) const override;
    bool         generate(rnp::SecurityContext &ctx, const KeyParams &params) override;
    rnp_result_t verify(const rnp::SecurityContext &ctx,
                        const SigMaterial &         sig,
                        const rnp::secure_bytes &   hash) const override;
    rnp_result_t sign(rnp::SecurityContext &   ctx,
                      SigMaterial &            sig,
                      const rnp::secure_bytes &hash) const override;
    size_t       bits() const noexcept override;
    pgp_curve_t  curve() const noexcept override;

    const std::vector<uint8_t> &pub() const noexcept;
    const std::vector<uint8_t> &priv() const noexcept;
};

class X25519KeyMaterial : public KeyMaterial {
    pgp_x25519_key_t key_;

  protected:
    void grip_update(rnp::Hash &hash) const override;
    bool validate_material(rnp::SecurityContext &ctx, bool reset) override;

  public:
    X25519KeyMaterial() : KeyMaterial(PGP_PKA_X25519), key_{} {};
    std::unique_ptr<KeyMaterial> clone() override;

    void         clear_secret() noexcept override;
    bool         parse(pgp_packet_body_t &pkt) noexcept override;
    bool         parse_secret(pgp_packet_body_t &pkt) noexcept override;
    void         write(pgp_packet_body_t &pkt) const override;
    void         write_secret(pgp_packet_body_t &pkt) const override;
    bool         generate(rnp::SecurityContext &ctx, const KeyParams &params) override;
    rnp_result_t encrypt(rnp::SecurityContext &   ctx,
                         EncMaterial &            out,
                         const rnp::secure_bytes &data) const override;
    rnp_result_t decrypt(rnp::SecurityContext &ctx,
                         rnp::secure_bytes &   out,
                         const EncMaterial &   in) const override;
    size_t       bits() const noexcept override;
    pgp_curve_t  curve() const noexcept override;

    const std::vector<uint8_t> &pub() const noexcept;
    const std::vector<uint8_t> &priv() const noexcept;
};
#endif

#if defined(ENABLE_PQC)
class MlkemEcdhKeyMaterial : public KeyMaterial {
    pgp_kyber_ecdh_key_t key_;

  protected:
    void grip_update(rnp::Hash &hash) const override;
    bool validate_material(rnp::SecurityContext &ctx, bool reset) override;

  public:
    MlkemEcdhKeyMaterial(pgp_pubkey_alg_t kalg) : KeyMaterial(kalg), key_{} {};
    std::unique_ptr<KeyMaterial> clone() override;

    void         clear_secret() noexcept override;
    bool         parse(pgp_packet_body_t &pkt) noexcept override;
    bool         parse_secret(pgp_packet_body_t &pkt) noexcept override;
    void         write(pgp_packet_body_t &pkt) const override;
    void         write_secret(pgp_packet_body_t &pkt) const override;
    bool         generate(rnp::SecurityContext &ctx, const KeyParams &params) override;
    rnp_result_t encrypt(rnp::SecurityContext &   ctx,
                         EncMaterial &            out,
                         const rnp::secure_bytes &data) const override;
    rnp_result_t decrypt(rnp::SecurityContext &ctx,
                         rnp::secure_bytes &   out,
                         const EncMaterial &   in) const override;
    size_t       bits() const noexcept override;

    const pgp_kyber_ecdh_composite_public_key_t & pub() const noexcept;
    const pgp_kyber_ecdh_composite_private_key_t &priv() const noexcept;
};

class DilithiumEccKeyMaterial : public KeyMaterial {
    pgp_dilithium_exdsa_key_t key_;

  protected:
    void grip_update(rnp::Hash &hash) const override;
    bool validate_material(rnp::SecurityContext &ctx, bool reset) override;

  public:
    DilithiumEccKeyMaterial(pgp_pubkey_alg_t kalg) : KeyMaterial(kalg), key_{} {};
    std::unique_ptr<KeyMaterial> clone() override;

    /** @brief Check two key material for equality. Only public part is checked, so this may be
     * called on public/secret key material */
    void           clear_secret() noexcept override;
    bool           parse(pgp_packet_body_t &pkt) noexcept override;
    bool           parse_secret(pgp_packet_body_t &pkt) noexcept override;
    void           write(pgp_packet_body_t &pkt) const override;
    void           write_secret(pgp_packet_body_t &pkt) const override;
    bool           generate(rnp::SecurityContext &ctx, const KeyParams &params) override;
    rnp_result_t   verify(const rnp::SecurityContext &ctx,
                          const SigMaterial &         sig,
                          const rnp::secure_bytes &   hash) const override;
    rnp_result_t   sign(rnp::SecurityContext &   ctx,
                        SigMaterial &            sig,
                        const rnp::secure_bytes &hash) const override;
    pgp_hash_alg_t adjust_hash(pgp_hash_alg_t hash) const override;
    size_t         bits() const noexcept override;

    const pgp_dilithium_exdsa_composite_public_key_t & pub() const noexcept;
    const pgp_dilithium_exdsa_composite_private_key_t &priv() const noexcept;
};

class SlhdsaKeyMaterial : public KeyMaterial {
    pgp_sphincsplus_key_t key_;

  protected:
    void grip_update(rnp::Hash &hash) const override;
    bool validate_material(rnp::SecurityContext &ctx, bool reset) override;

  public:
    SlhdsaKeyMaterial(pgp_pubkey_alg_t kalg) : KeyMaterial(kalg), key_{} {};
    std::unique_ptr<KeyMaterial> clone() override;

    void           clear_secret() noexcept override;
    bool           parse(pgp_packet_body_t &pkt) noexcept override;
    bool           parse_secret(pgp_packet_body_t &pkt) noexcept override;
    void           write(pgp_packet_body_t &pkt) const override;
    void           write_secret(pgp_packet_body_t &pkt) const override;
    bool           generate(rnp::SecurityContext &ctx, const KeyParams &params) override;
    rnp_result_t   verify(const rnp::SecurityContext &ctx,
                          const SigMaterial &         sig,
                          const rnp::secure_bytes &   hash) const override;
    rnp_result_t   sign(rnp::SecurityContext &   ctx,
                        SigMaterial &            sig,
                        const rnp::secure_bytes &hash) const override;
    pgp_hash_alg_t adjust_hash(pgp_hash_alg_t hash) const override;
    bool           sig_hash_allowed(pgp_hash_alg_t hash) const override;
    size_t         bits() const noexcept override;

    const pgp_sphincsplus_public_key_t & pub() const noexcept;
    const pgp_sphincsplus_private_key_t &priv() const noexcept;
};
#endif
} // namespace pgp

#endif // RNP_KEY_MATERIAL_HPP_