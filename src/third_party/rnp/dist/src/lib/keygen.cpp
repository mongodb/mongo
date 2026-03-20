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

#include "keygen.hpp"
#include <cassert>
#include <algorithm>
#include "librekey/key_store_g10.h"

namespace rnp {
KeygenParams::KeygenParams(pgp_pubkey_alg_t alg, SecurityContext &ctx)
    : alg_(alg), hash_(PGP_HASH_UNKNOWN), version_(PGP_V4), ctx_(ctx)
{
    key_params_ = pgp::KeyParams::create(alg);
}

void
KeygenParams::check_defaults() noexcept
{
    if (hash_ == PGP_HASH_UNKNOWN) {
        hash_ = alg_ == PGP_PKA_SM2 ? PGP_HASH_SM3 : DEFAULT_PGP_HASH_ALG;
    }
    pgp_hash_alg_t min_hash = key_params_->min_hash();
    if (Hash::size(hash_) < Hash::size(min_hash)) {
        hash_ = min_hash;
    }
    key_params_->check_defaults();
}

bool
KeygenParams::validate() const noexcept
{
#if defined(ENABLE_PQC)
    switch (alg()) {
    case PGP_PKA_SPHINCSPLUS_SHA2:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_SPHINCSPLUS_SHAKE: {
        auto &slhdsa = dynamic_cast<const pgp::SlhdsaKeyParams &>(key_params());
        if (!sphincsplus_hash_allowed(alg(), slhdsa.param(), hash())) {
            RNP_LOG("invalid hash algorithm for the slhdsa key");
            return false;
        }
        break;
    }
    case PGP_PKA_DILITHIUM3_ED25519:
        FALLTHROUGH_STATEMENT;
    // TODO: Add case PGP_PKA_DILITHIUM5_ED448: FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_P384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_BP256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_BP384:
        if (!dilithium_hash_allowed(hash())) {
            RNP_LOG("invalid hash algorithm for the dilithium key");
            return false;
        }
        break;
    default:
        break;
    }
#endif
    return true;
}

bool
KeygenParams::validate(const CertParams &cert) const noexcept
{
    /* Confirm that the specified pk alg can certify.
     * gpg requires this, though the RFC only says that a V4 primary
     * key SHOULD be a key capable of certification.
     */
    if (!(pgp_pk_alg_capabilities(alg()) & PGP_KF_CERTIFY)) {
        RNP_LOG("primary key alg (%d) must be able to sign", alg());
        return false;
    }

    // check key flags
    if (!cert.flags) {
        // these are probably not *technically* required
        RNP_LOG("key flags are required");
        return false;
    }
    if (cert.flags & ~pgp_pk_alg_capabilities(alg())) {
        // check the flags against the alg capabilities
        RNP_LOG("usage not permitted for pk algorithm");
        return false;
    }
    // require a userid
    if (cert.userid.empty()) {
        RNP_LOG("userid is required for primary key");
        return false;
    }
    return validate();
}

bool
KeygenParams::validate(const BindingParams &binding) const noexcept
{
    if (!binding.flags) {
        RNP_LOG("key flags are required");
        return false;
    }
    if (binding.flags & ~pgp_pk_alg_capabilities(alg())) {
        // check the flags against the alg capabilities
        RNP_LOG("usage not permitted for pk algorithm");
        return false;
    }
    return validate();
}

static const id_str_pair pubkey_alg_map[] = {{PGP_PKA_RSA, "RSA (Encrypt or Sign)"},
                                             {PGP_PKA_RSA_ENCRYPT_ONLY, "RSA Encrypt-Only"},
                                             {PGP_PKA_RSA_SIGN_ONLY, "RSA Sign-Only"},
                                             {PGP_PKA_ELGAMAL, "Elgamal (Encrypt-Only)"},
                                             {PGP_PKA_DSA, "DSA"},
                                             {PGP_PKA_ECDH, "ECDH"},
                                             {PGP_PKA_ECDSA, "ECDSA"},
                                             {PGP_PKA_EDDSA, "EdDSA"},
                                             {PGP_PKA_SM2, "SM2"},
#if defined(ENABLE_CRYPTO_REFRESH)
                                             {PGP_PKA_ED25519, "ED25519"},
                                             {PGP_PKA_X25519, "X25519"},
#endif
#if defined(ENABLE_PQC)
                                             {PGP_PKA_KYBER768_X25519, "ML-KEM-768_X25519"},
                                             //{PGP_PKA_KYBER1024_X448, "Kyber-X448"},
                                             {PGP_PKA_KYBER768_P256, "ML-KEM-768_P256"},
                                             {PGP_PKA_KYBER1024_P384, "ML-KEM-1024_P384"},
                                             {PGP_PKA_KYBER768_BP256, "ML-KEM-768_BP256"},
                                             {PGP_PKA_KYBER1024_BP384, "ML-KEM-1024_BP384"},
                                             {PGP_PKA_DILITHIUM3_ED25519, "ML-DSA-65_ED25519"},
                                             //{PGP_PKA_DILITHIUM5_ED448, "Dilithium-ED448"},
                                             {PGP_PKA_DILITHIUM3_P256, "ML-DSA-65_P256"},
                                             {PGP_PKA_DILITHIUM5_P384, "ML-DSA-87_P384"},
                                             {PGP_PKA_DILITHIUM3_BP256, "ML-DSA-65_BP256"},
                                             {PGP_PKA_DILITHIUM5_BP384, "ML-DSA-87_BP384"},
                                             {PGP_PKA_SPHINCSPLUS_SHA2, "SLH-DSA-SHA2"},
                                             {PGP_PKA_SPHINCSPLUS_SHAKE, "SLH-DSA-SHAKE"},
#endif
                                             {0, NULL}};

bool
KeygenParams::generate(pgp_key_pkt_t &seckey, bool primary)
{
    /* populate pgp key structure */
    seckey = {};
    seckey.version = version();
    seckey.creation_time = ctx().time();
    seckey.alg = alg();
    seckey.material = pgp::KeyMaterial::create(alg());
    if (!seckey.material) {
        RNP_LOG("Unsupported key algorithm: %d", alg());
        return false;
    }
    seckey.tag = primary ? PGP_PKT_SECRET_KEY : PGP_PKT_SECRET_SUBKEY;

    if (!seckey.material->generate(ctx(), key_params())) {
        return false;
    }

    seckey.sec_protection.s2k.usage = PGP_S2KU_NONE;
    /* fill the sec_data/sec_len */
    if (encrypt_secret_key(&seckey, NULL, ctx().rng)) {
        RNP_LOG("failed to fill sec_data");
        return false;
    }
    return true;
}

static bool
load_generated_g10_key(
  Key *dst, pgp_key_pkt_t *newkey, Key *primary_key, Key *pubkey, SecurityContext &ctx)
{
    // this should generally be zeroed
    assert(dst->type() == 0);
    // if a primary is provided, make sure it's actually a primary key
    assert(!primary_key || primary_key->is_primary());
    // if a pubkey is provided, make sure it's actually a public key
    assert(!pubkey || pubkey->is_public());
    // G10 always needs pubkey here
    assert(pubkey);

    // this would be better on the stack but the key store does not allow it
    std::unique_ptr<KeyStore> key_store(new (std::nothrow) KeyStore(ctx));
    if (!key_store) {
        return false;
    }
    /* Write g10 seckey */
    MemoryDest memdst(NULL, 0);
    if (!g10_write_seckey(&memdst.dst(), newkey, NULL, ctx)) {
        RNP_LOG("failed to write generated seckey");
        return false;
    }

    std::vector<Key *> key_ptrs; /* holds primary and pubkey, when used */
    // if this is a subkey, add the primary in first
    if (primary_key) {
        key_ptrs.push_back(primary_key);
    }
    // G10 needs the pubkey for copying some attributes (key version, creation time, etc)
    key_ptrs.push_back(pubkey);

    MemorySource memsrc(memdst.memory(), memdst.writeb(), false);
    KeyProvider  prov(rnp_key_provider_key_ptr_list, &key_ptrs);
    if (!key_store.get()->load_g10(memsrc.src(), &prov)) {
        return false;
    }
    if (key_store.get()->key_count() != 1) {
        return false;
    }
    // if a primary key is provided, it should match the sub with regards to type
    assert(!primary_key || (primary_key->is_secret() == key_store->keys.front().is_secret()));
    *dst = Key(key_store->keys.front());
    return true;
}

bool
KeygenParams::generate(CertParams &cert,
                       Key &       primary_sec,
                       Key &       primary_pub,
                       KeyFormat   secformat)
{
    primary_sec = {};
    primary_pub = {};

    // merge some defaults in
    check_defaults();
    cert.check_defaults(*this);
    // now validate the keygen fields
    if (!validate(cert)) {
        return false;
    }

    // generate the raw key and fill tag/secret fields
    pgp_key_pkt_t secpkt;
    if (!generate(secpkt, true)) {
        return false;
    }

    Key sec(secpkt);
    Key pub(secpkt, true);
#if defined(ENABLE_CRYPTO_REFRESH)
    // for v6 packets, a direct-key sig is mandatory.
    if (sec.version() == PGP_V6) {
        sec.add_direct_sig(cert, hash(), ctx(), &pub);
    }
#endif
    sec.add_uid_cert(cert, hash(), ctx(), &pub);

    switch (secformat) {
    case KeyFormat::GPG:
    case KeyFormat::KBX:
        primary_sec = std::move(sec);
        primary_pub = std::move(pub);
        break;
    case KeyFormat::G10:
        primary_pub = std::move(pub);
        if (!load_generated_g10_key(&primary_sec, &secpkt, NULL, &primary_pub, ctx())) {
            RNP_LOG("failed to load generated key");
            return false;
        }
        break;
    default:
        RNP_LOG("invalid format");
        return false;
    }

    /* mark it as valid */
    primary_pub.mark_valid();
    primary_sec.mark_valid();
    /* refresh key's data */
    return primary_pub.refresh_data(ctx()) && primary_sec.refresh_data(ctx());
}

bool
KeygenParams::generate(BindingParams &                binding,
                       Key &                          primary_sec,
                       Key &                          primary_pub,
                       Key &                          subkey_sec,
                       Key &                          subkey_pub,
                       const pgp_password_provider_t &password_provider,
                       KeyFormat                      secformat)
{
    // validate args
    if (!primary_sec.is_primary() || !primary_pub.is_primary() || !primary_sec.is_secret() ||
        !primary_pub.is_public()) {
        RNP_LOG("invalid parameters");
        return false;
    }
    subkey_sec = {};
    subkey_pub = {};

    // merge some defaults in
    check_defaults();
    binding.check_defaults(*this);

    // now validate the keygen fields
    if (!validate(binding)) {
        return false;
    }

    /* decrypt the primary seckey if needed (for signatures) */
    KeyLocker primlock(primary_sec);
    if (primary_sec.encrypted() && !primary_sec.unlock(password_provider, PGP_OP_ADD_SUBKEY)) {
        RNP_LOG("Failed to unlock primary key.");
        return false;
    }
    /* generate the raw subkey */
    pgp_key_pkt_t secpkt;
    if (!generate(secpkt, false)) {
        return false;
    }
    pgp_key_pkt_t pubpkt = pgp_key_pkt_t(secpkt, true);
    Key           sec(secpkt, primary_sec);
    Key           pub(pubpkt, primary_pub);
    /* add binding */
    primary_sec.add_sub_binding(sec, pub, binding, hash(), ctx());
    /* copy to the result */
    subkey_pub = std::move(pub);
    switch (secformat) {
    case KeyFormat::GPG:
    case KeyFormat::KBX:
        subkey_sec = std::move(sec);
        break;
    case KeyFormat::G10:
        if (!load_generated_g10_key(&subkey_sec, &secpkt, &primary_sec, &subkey_pub, ctx())) {
            RNP_LOG("failed to load generated key");
            return false;
        }
        break;
    default:
        RNP_LOG("invalid format");
        return false;
    }

    subkey_pub.mark_valid();
    subkey_sec.mark_valid();
    return subkey_pub.refresh_data(&primary_pub, ctx()) &&
           subkey_sec.refresh_data(&primary_sec, ctx());
}

UserPrefs::UserPrefs(const pgp::pkt::Signature &sig)
{
    symm_algs = sig.preferred_symm_algs();
    hash_algs = sig.preferred_hash_algs();
    z_algs = sig.preferred_z_algs();

    if (sig.has_subpkt(PGP_SIG_SUBPKT_KEYSERV_PREFS)) {
        ks_prefs = {sig.key_server_prefs()};
    }

    if (sig.has_subpkt(PGP_SIG_SUBPKT_PREF_KEYSERV)) {
        key_server = sig.key_server();
    }
}

void
UserPrefs::add_uniq(std::vector<uint8_t> &vec, uint8_t val)
{
    if (std::find(vec.begin(), vec.end(), val) == vec.end()) {
        vec.push_back(val);
    }
}

void
UserPrefs::add_symm_alg(pgp_symm_alg_t alg)
{
    add_uniq(symm_algs, alg);
}

void
UserPrefs::add_hash_alg(pgp_hash_alg_t alg)
{
    add_uniq(hash_algs, alg);
}

void
UserPrefs::add_z_alg(pgp_compression_type_t alg)
{
    add_uniq(z_algs, alg);
}

void
UserPrefs::add_ks_pref(pgp_key_server_prefs_t pref)
{
    add_uniq(ks_prefs, pref);
}

#if defined(ENABLE_CRYPTO_REFRESH)
void
UserPrefs::add_aead_prefs(pgp_symm_alg_t sym_alg, pgp_aead_alg_t aead_alg)
{
    for (size_t i = 0; i < aead_prefs.size(); i += 2) {
        if (aead_prefs[i] == sym_alg && aead_prefs[i + 1] == aead_alg) {
            return;
        }
    }
    aead_prefs.push_back(sym_alg);
    aead_prefs.push_back(aead_alg);
}
#endif

void
UserPrefs::check_defaults(pgp_version_t version)
{
    if (symm_algs.empty()) {
        symm_algs = {PGP_SA_AES_256, PGP_SA_AES_192, PGP_SA_AES_128};
    }
    if (hash_algs.empty()) {
        hash_algs = {PGP_HASH_SHA256, PGP_HASH_SHA384, PGP_HASH_SHA512, PGP_HASH_SHA224};
    }
    if (z_algs.empty()) {
        z_algs = {PGP_C_ZLIB, PGP_C_BZIP2, PGP_C_ZIP, PGP_C_NONE};
    }
#if defined(ENABLE_CRYPTO_REFRESH)
    if (aead_prefs.empty() && (version == PGP_V6)) {
        for (auto sym_alg : symm_algs) {
            aead_prefs.push_back(sym_alg);
            aead_prefs.push_back(PGP_AEAD_OCB);
        }
    }
#endif
}

void
CertParams::check_defaults(const KeygenParams &params)
{
    prefs.check_defaults(params.version());

    if (!flags) {
        // set some default key flags if none are provided
        flags = pgp_pk_alg_capabilities(params.alg());
    }
    if (userid.empty()) {
        std::string alg = id_str_pair::lookup(pubkey_alg_map, params.alg());
        std::string bits = std::to_string(params.key_params().bits());
        std::string name = getenv_logname() ? getenv_logname() : "";
        /* Awkward but better then sprintf */
        userid = alg + " " + bits + "-bit key <" + name + "@localhost>";
    }
}

void
CertParams::populate(pgp_userid_pkt_t &uid) const
{
    uid.tag = PGP_PKT_USER_ID;
    uid.uid.assign(userid.data(), userid.data() + userid.size());
}

void
CertParams::populate(pgp::pkt::Signature &sig) const
{
    if (key_expiration) {
        sig.set_key_expiration(key_expiration);
    }
    if (primary) {
        sig.set_primary_uid(true);
    }
#if defined(ENABLE_CRYPTO_REFRESH)
    if ((sig.version == PGP_V6) && (sig.type() != PGP_SIG_DIRECT)) {
        /* only set key expiraton and primary uid for v6 self-signatures
         * since most information is stored in the direct-key signature of the primary key.
         */

        if (flags && (sig.type() == PGP_SIG_SUBKEY)) {
            /* for v6 subkeys signatures we also add the key flags */
            sig.set_key_flags(flags);
        }
        return;
    } else if ((sig.version == PGP_V6) && (sig.type() == PGP_SIG_DIRECT)) {
        /* set some additional packets for v6 direct-key self signatures */
        sig.set_key_features(PGP_KEY_FEATURE_MDC | PGP_KEY_FEATURE_SEIPDV2);
        if (!prefs.aead_prefs.empty()) {
            sig.set_preferred_aead_algs(prefs.aead_prefs);
        }
    }
#endif
    if (flags) {
        sig.set_key_flags(flags);
    }
    if (!prefs.symm_algs.empty()) {
        sig.set_preferred_symm_algs(prefs.symm_algs);
    }
    if (!prefs.hash_algs.empty()) {
        sig.set_preferred_hash_algs(prefs.hash_algs);
    }
    if (!prefs.z_algs.empty()) {
        sig.set_preferred_z_algs(prefs.z_algs);
    }
    if (!prefs.ks_prefs.empty()) {
        sig.set_key_server_prefs(prefs.ks_prefs[0]);
    }
    if (!prefs.key_server.empty()) {
        sig.set_key_server(prefs.key_server);
    }
}

void
CertParams::populate(pgp_userid_pkt_t &uid, pgp::pkt::Signature &sig) const
{
    sig.set_type(PGP_CERT_POSITIVE);
    populate(sig);
    populate(uid);
}

void
BindingParams::check_defaults(const KeygenParams &params)
{
    if (!flags) {
        // set some default key flags if none are provided
        flags = pgp_pk_alg_capabilities(params.alg());
    }
}

} // namespace rnp
