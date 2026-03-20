/*
 * Copyright (c) 2018-2020, 2023 [Ribose Inc](https://www.ribose.com).
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

#include "config.h"
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <cassert>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#else
#include "uniwin.h"
#endif
#include <string.h>
#include "time-utils.h"
#include "stream-def.h"
#include "stream-dump.h"
#include "stream-armor.h"
#include "stream-packet.h"
#include "stream-parse.h"
#include "types.h"
#include "ctype.h"
#include "crypto/symmetric.h"
#include "crypto/s2k.h"
#include "fingerprint.hpp"
#include "key.hpp"
#include "json-utils.h"
#include <algorithm>
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif
#include <cinttypes>

static const id_str_pair packet_tag_map[] = {
  {PGP_PKT_RESERVED, "Reserved"},
  {PGP_PKT_PK_SESSION_KEY, "Public-Key Encrypted Session Key"},
  {PGP_PKT_SIGNATURE, "Signature"},
  {PGP_PKT_SK_SESSION_KEY, "Symmetric-Key Encrypted Session Key"},
  {PGP_PKT_ONE_PASS_SIG, "One-Pass Signature"},
  {PGP_PKT_SECRET_KEY, "Secret Key"},
  {PGP_PKT_PUBLIC_KEY, "Public Key"},
  {PGP_PKT_SECRET_SUBKEY, "Secret Subkey"},
  {PGP_PKT_COMPRESSED, "Compressed Data"},
  {PGP_PKT_SE_DATA, "Symmetrically Encrypted Data"},
  {PGP_PKT_MARKER, "Marker"},
  {PGP_PKT_LITDATA, "Literal Data"},
  {PGP_PKT_TRUST, "Trust"},
  {PGP_PKT_USER_ID, "User ID"},
  {PGP_PKT_PUBLIC_SUBKEY, "Public Subkey"},
  {PGP_PKT_RESERVED2, "reserved2"},
  {PGP_PKT_RESERVED3, "reserved3"},
  {PGP_PKT_USER_ATTR, "User Attribute"},
  {PGP_PKT_SE_IP_DATA, "Symmetric Encrypted and Integrity Protected Data"},
  {PGP_PKT_MDC, "Modification Detection Code"},
  {PGP_PKT_AEAD_ENCRYPTED, "AEAD Encrypted Data Packet"},
  {0x00, NULL},
};

static const id_str_pair sig_type_map[] = {
  {PGP_SIG_BINARY, "Signature of a binary document"},
  {PGP_SIG_TEXT, "Signature of a canonical text document"},
  {PGP_SIG_STANDALONE, "Standalone signature"},
  {PGP_CERT_GENERIC, "Generic User ID certification"},
  {PGP_CERT_PERSONA, "Personal User ID certification"},
  {PGP_CERT_CASUAL, "Casual User ID certification"},
  {PGP_CERT_POSITIVE, "Positive User ID certification"},
  {PGP_SIG_SUBKEY, "Subkey Binding Signature"},
  {PGP_SIG_PRIMARY, "Primary Key Binding Signature"},
  {PGP_SIG_DIRECT, "Direct-key signature"},
  {PGP_SIG_REV_KEY, "Key revocation signature"},
  {PGP_SIG_REV_SUBKEY, "Subkey revocation signature"},
  {PGP_SIG_REV_CERT, "Certification revocation signature"},
  {PGP_SIG_TIMESTAMP, "Timestamp signature"},
  {PGP_SIG_3RD_PARTY, "Third-Party Confirmation signature"},
  {0x00, NULL},
};

static const id_str_pair sig_subpkt_type_map[] = {
  {PGP_SIG_SUBPKT_CREATION_TIME, "signature creation time"},
  {PGP_SIG_SUBPKT_EXPIRATION_TIME, "signature expiration time"},
  {PGP_SIG_SUBPKT_EXPORT_CERT, "exportable certification"},
  {PGP_SIG_SUBPKT_TRUST, "trust signature"},
  {PGP_SIG_SUBPKT_REGEXP, "regular expression"},
  {PGP_SIG_SUBPKT_REVOCABLE, "revocable"},
  {PGP_SIG_SUBPKT_KEY_EXPIRY, "key expiration time"},
  {PGP_SIG_SUBPKT_PREFERRED_SKA, "preferred symmetric algorithms"},
  {PGP_SIG_SUBPKT_REVOCATION_KEY, "revocation key"},
  {PGP_SIG_SUBPKT_ISSUER_KEY_ID, "issuer key ID"},
  {PGP_SIG_SUBPKT_NOTATION_DATA, "notation data"},
  {PGP_SIG_SUBPKT_PREFERRED_HASH, "preferred hash algorithms"},
  {PGP_SIG_SUBPKT_PREF_COMPRESS, "preferred compression algorithms"},
  {PGP_SIG_SUBPKT_KEYSERV_PREFS, "key server preferences"},
  {PGP_SIG_SUBPKT_PREF_KEYSERV, "preferred key server"},
  {PGP_SIG_SUBPKT_PRIMARY_USER_ID, "primary user ID"},
  {PGP_SIG_SUBPKT_POLICY_URI, "policy URI"},
  {PGP_SIG_SUBPKT_KEY_FLAGS, "key flags"},
  {PGP_SIG_SUBPKT_SIGNERS_USER_ID, "signer's user ID"},
  {PGP_SIG_SUBPKT_REVOCATION_REASON, "reason for revocation"},
  {PGP_SIG_SUBPKT_FEATURES, "features"},
  {PGP_SIG_SUBPKT_SIGNATURE_TARGET, "signature target"},
  {PGP_SIG_SUBPKT_EMBEDDED_SIGNATURE, "embedded signature"},
  {PGP_SIG_SUBPKT_ISSUER_FPR, "issuer fingerprint"},
  {PGP_SIG_SUBPKT_PREFERRED_AEAD, "preferred AEAD algorithms"},
  {0x00, NULL},
};

static const id_str_pair key_type_map[] = {
  {PGP_PKT_SECRET_KEY, "Secret key"},
  {PGP_PKT_PUBLIC_KEY, "Public key"},
  {PGP_PKT_SECRET_SUBKEY, "Secret subkey"},
  {PGP_PKT_PUBLIC_SUBKEY, "Public subkey"},
  {0x00, NULL},
};

static const id_str_pair pubkey_alg_map[] = {
  {PGP_PKA_RSA, "RSA (Encrypt or Sign)"},
  {PGP_PKA_RSA_ENCRYPT_ONLY, "RSA (Encrypt-Only)"},
  {PGP_PKA_RSA_SIGN_ONLY, "RSA (Sign-Only)"},
  {PGP_PKA_ELGAMAL, "Elgamal (Encrypt-Only)"},
  {PGP_PKA_DSA, "DSA"},
  {PGP_PKA_ECDH, "ECDH"},
  {PGP_PKA_ECDSA, "ECDSA"},
  {PGP_PKA_ELGAMAL_ENCRYPT_OR_SIGN, "Elgamal"},
  {PGP_PKA_RESERVED_DH, "Reserved for DH (X9.42)"},
  {PGP_PKA_EDDSA, "EdDSA"},
  {PGP_PKA_SM2, "SM2"},
#if defined(ENABLE_CRYPTO_REFRESH)
  {PGP_PKA_ED25519, "Ed25519"},
  {PGP_PKA_X25519, "X25519"},
#endif
#if defined(ENABLE_PQC)
  {PGP_PKA_KYBER768_X25519, "ML-KEM-768 + X25519"},
  //{PGP_PKA_KYBER1024_X448, "Kyber1024 + X448"},
  {PGP_PKA_KYBER768_P256, "ML-KEM-768 + NIST P-256"},
  {PGP_PKA_KYBER1024_P384, "ML-KEM-1024 + NIST P-384"},
  {PGP_PKA_KYBER768_BP256, "ML-KEM-768 + Brainpool256"},
  {PGP_PKA_KYBER1024_BP384, "ML-KEM-1024 + Brainpool384"},
  {PGP_PKA_DILITHIUM3_ED25519, "ML-DSA-65 + ED25519"},
  //{PGP_PKA_DILITHIUM5_ED448, "Dilithium + X448"},
  {PGP_PKA_DILITHIUM3_P256, "ML-DSA-65 + NIST P-256"},
  {PGP_PKA_DILITHIUM5_P384, "ML-DSA-87 + NIST P-384"},
  {PGP_PKA_DILITHIUM3_BP256, "ML-DSA-65 + Brainpool256"},
  {PGP_PKA_DILITHIUM5_BP384, "ML-DSA-87 + Brainpool384"},
  {PGP_PKA_SPHINCSPLUS_SHA2, "SLH-DSA-SHA2"},
  {PGP_PKA_SPHINCSPLUS_SHAKE, "SLH-DSA-SHAKE"},
#endif
  {0x00, NULL},
};

static const id_str_pair symm_alg_map[] = {
  {PGP_SA_PLAINTEXT, "Plaintext"},
  {PGP_SA_IDEA, "IDEA"},
  {PGP_SA_TRIPLEDES, "TripleDES"},
  {PGP_SA_CAST5, "CAST5"},
  {PGP_SA_BLOWFISH, "Blowfish"},
  {PGP_SA_AES_128, "AES-128"},
  {PGP_SA_AES_192, "AES-192"},
  {PGP_SA_AES_256, "AES-256"},
  {PGP_SA_TWOFISH, "Twofish"},
  {PGP_SA_CAMELLIA_128, "Camellia-128"},
  {PGP_SA_CAMELLIA_192, "Camellia-192"},
  {PGP_SA_CAMELLIA_256, "Camellia-256"},
  {PGP_SA_SM4, "SM4"},
  {0x00, NULL},
};

static const id_str_pair hash_alg_map[] = {
  {PGP_HASH_MD5, "MD5"},
  {PGP_HASH_SHA1, "SHA1"},
  {PGP_HASH_RIPEMD, "RIPEMD160"},
  {PGP_HASH_SHA256, "SHA256"},
  {PGP_HASH_SHA384, "SHA384"},
  {PGP_HASH_SHA512, "SHA512"},
  {PGP_HASH_SHA224, "SHA224"},
  {PGP_HASH_SM3, "SM3"},
  {PGP_HASH_SHA3_256, "SHA3-256"},
  {PGP_HASH_SHA3_512, "SHA3-512"},
  {0x00, NULL},
};

static const id_str_pair z_alg_map[] = {
  {PGP_C_NONE, "Uncompressed"},
  {PGP_C_ZIP, "ZIP"},
  {PGP_C_ZLIB, "ZLib"},
  {PGP_C_BZIP2, "BZip2"},
  {0x00, NULL},
};

static const id_str_pair aead_alg_map[] = {
  {PGP_AEAD_NONE, "None"},
  {PGP_AEAD_EAX, "EAX"},
  {PGP_AEAD_OCB, "OCB"},
  {0x00, NULL},
};

static const id_str_pair revoc_reason_map[] = {
  {PGP_REVOCATION_NO_REASON, "No reason"},
  {PGP_REVOCATION_SUPERSEDED, "Superseded"},
  {PGP_REVOCATION_COMPROMISED, "Compromised"},
  {PGP_REVOCATION_RETIRED, "Retired"},
  {PGP_REVOCATION_NO_LONGER_VALID, "No longer valid"},
  {0x00, NULL},
};

typedef struct pgp_dest_indent_param_t {
    int         level;
    bool        lstart;
    pgp_dest_t *writedst;
} pgp_dest_indent_param_t;

static rnp_result_t
indent_dst_write(pgp_dest_t *dst, const void *buf, size_t len)
{
    pgp_dest_indent_param_t *param = (pgp_dest_indent_param_t *) dst->param;
    const char *             line = (const char *) buf;
    char                     indent[4] = {' ', ' ', ' ', ' '};

    if (!len) {
        return RNP_SUCCESS;
    }

    do {
        if (param->lstart) {
            for (int i = 0; i < param->level; i++) {
                dst_write(param->writedst, indent, sizeof(indent));
            }
            param->lstart = false;
        }

        for (size_t i = 0; i < len; i++) {
            if ((line[i] == '\n') || (i == len - 1)) {
                dst_write(param->writedst, line, i + 1);
                param->lstart = line[i] == '\n';
                line += i + 1;
                len -= i + 1;
                break;
            }
        }
    } while (len > 0);

    return RNP_SUCCESS;
}

static void
indent_dst_close(pgp_dest_t *dst, bool discard)
{
    free(dst->param);
}

static rnp_result_t
init_indent_dest(pgp_dest_t &dst, pgp_dest_t *origdst)
{
    pgp_dest_indent_param_t *param;

    if (!init_dst_common(&dst, sizeof(*param))) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }

    dst.write = indent_dst_write;
    dst.close = indent_dst_close;
    dst.finish = NULL;
    dst.no_cache = true;
    param = (pgp_dest_indent_param_t *) dst.param;
    param->writedst = origdst;
    param->lstart = true;
    param->level = 0;

    return RNP_SUCCESS;
}

static void
indent_dest_increase(pgp_dest_t &dst)
{
    ((pgp_dest_indent_param_t *) dst.param)->level++;
}

static void
indent_dest_decrease(pgp_dest_t &dst)
{
    pgp_dest_indent_param_t *param = (pgp_dest_indent_param_t *) dst.param;
    if (param->level > 0) {
        param->level--;
    }
}

static size_t
vsnprinthex(char *str, size_t slen, const uint8_t *buf, size_t buflen)
{
    static const char *hexes = "0123456789abcdef";
    size_t             idx = 0;

    for (size_t i = 0; (i < buflen) && (i < (slen - 1) / 2); i++) {
        str[idx++] = hexes[buf[i] >> 4];
        str[idx++] = hexes[buf[i] & 0xf];
    }
    str[idx] = '\0';
    return buflen * 2;
}

static void
dst_print_mpi(pgp_dest_t &dst, const char *name, const pgp::mpi &mpi, bool dumpbin)
{
    if (!dumpbin) {
        dst_printf(dst, "%s: %zu bits\n", name, mpi.bits());
    } else {
        char hex[5000];
        vsnprinthex(hex, sizeof(hex), mpi.data(), mpi.size());
        dst_printf(dst, "%s: %zu bits, %s\n", name, mpi.bits(), hex);
    }
}

#if defined(ENABLE_CRYPTO_REFRESH) || defined(ENABLE_PQC)
static void
dst_print_vec(pgp_dest_t &                dst,
              const char *                name,
              std::vector<uint8_t> const &data,
              bool                        dumpbin)
{
    if (!dumpbin) {
        dst_printf(dst, "%s\n", name);
    } else {
        std::vector<char> hex(2 * data.size());
        vsnprinthex(hex.data(), hex.size(), data.data(), data.size());
        dst_printf(dst, "%s, %s\n", name, hex.data());
    }
}
#endif

static void
dst_print_palg(pgp_dest_t &dst, const char *name, pgp_pubkey_alg_t palg)
{
    const char *palg_name = id_str_pair::lookup(pubkey_alg_map, palg, "Unknown");
    if (!name) {
        name = "public key algorithm";
    }

    dst_printf(dst, "%s: %d (%s)\n", name, (int) palg, palg_name);
}

static void
dst_print_halg(pgp_dest_t &dst, const char *name, pgp_hash_alg_t halg)
{
    const char *halg_name = id_str_pair::lookup(hash_alg_map, halg, "Unknown");
    if (!name) {
        name = "hash algorithm";
    }

    dst_printf(dst, "%s: %d (%s)\n", name, (int) halg, halg_name);
}

static void
dst_print_salg(pgp_dest_t &dst, const char *name, pgp_symm_alg_t salg)
{
    const char *salg_name = id_str_pair::lookup(symm_alg_map, salg, "Unknown");
    if (!name) {
        name = "symmetric algorithm";
    }

    dst_printf(dst, "%s: %d (%s)\n", name, (int) salg, salg_name);
}

static void
dst_print_aalg(pgp_dest_t &dst, const char *name, pgp_aead_alg_t aalg)
{
    const char *aalg_name = id_str_pair::lookup(aead_alg_map, aalg, "Unknown");
    if (!name) {
        name = "aead algorithm";
    }

    dst_printf(dst, "%s: %d (%s)\n", name, (int) aalg, aalg_name);
}

static void
dst_print_zalg(pgp_dest_t &dst, const char *name, pgp_compression_type_t zalg)
{
    const char *zalg_name = id_str_pair::lookup(z_alg_map, zalg, "Unknown");
    if (!name) {
        name = "compression algorithm";
    }

    dst_printf(dst, "%s: %d (%s)\n", name, (int) zalg, zalg_name);
}

static void
dst_print_str(pgp_dest_t &dst, const char *name, const std::string &str)
{
    dst_printf(dst, "%s: ", name);
    dst_write(&dst, str.data(), str.size());
    dst_printf(dst, "\n");
}

static void
dst_print_algs(pgp_dest_t &                dst,
               const std::string &         name,
               const std::vector<uint8_t> &algs,
               const id_str_pair           map[])
{
    dst_printf(dst, "%s: ", name.c_str());
    for (size_t i = 0; i < algs.size(); i++) {
        auto comma = i + 1 < algs.size() ? ", " : "";
        dst_printf(dst, "%s%s", id_str_pair::lookup(map, algs[i], "Unknown"), comma);
    }
    dst_printf(dst, " (");
    for (size_t i = 0; i < algs.size(); i++) {
        auto comma = i + 1 < algs.size() ? ", " : "";
        dst_printf(dst, "%" PRIu8 "%s", algs[i], comma);
    }
    dst_printf(dst, ")\n");
}

static void
dst_print_sig_type(pgp_dest_t &dst, const char *name, pgp_sig_type_t sigtype)
{
    const char *sig_name = id_str_pair::lookup(sig_type_map, sigtype, "Unknown");
    if (!name) {
        name = "signature type";
    }
    dst_printf(dst, "%s: %d (%s)\n", name, (int) sigtype, sig_name);
}

static void
dst_print_hex(
  pgp_dest_t &dst, const std::string &name, const uint8_t *data, size_t len, bool bytes)
{
    char hex[512];
    vsnprinthex(hex, sizeof(hex), data, len);
    if (bytes) {
        dst_printf(dst, "%s: 0x%s (%d bytes)\n", name.c_str(), hex, (int) len);
    } else {
        dst_printf(dst, "%s: 0x%s\n", name.c_str(), hex);
    }
}

static void
dst_print_keyid(pgp_dest_t &dst, const std::string &name, const pgp::KeyID &keyid)
{
    dst_print_hex(dst, name, keyid.data(), keyid.size(), false);
}

static void
dst_print_fp(pgp_dest_t &            dst,
             const std::string &     name,
             const pgp::Fingerprint &fp,
             bool                    size = true)
{
    dst_print_hex(dst, name, fp.data(), fp.size(), size);
}

static void
dst_print_s2k(pgp_dest_t &dst, pgp_s2k_t &s2k)
{
    dst_printf(dst, "s2k specifier: %d\n", (int) s2k.specifier);
    if ((s2k.specifier == PGP_S2KS_EXPERIMENTAL) && s2k.gpg_ext_num) {
        dst_printf(dst, "GPG extension num: %d\n", (int) s2k.gpg_ext_num);
        if (s2k.gpg_ext_num == PGP_S2K_GPG_SMARTCARD) {
            static_assert(sizeof(s2k.gpg_serial) == 16, "invalid s2k->gpg_serial size");
            size_t slen = s2k.gpg_serial_len > 16 ? 16 : s2k.gpg_serial_len;
            dst_print_hex(dst, "card serial number", s2k.gpg_serial, slen, true);
        }
        return;
    }
    if (s2k.specifier == PGP_S2KS_EXPERIMENTAL) {
        dst_print_hex(dst,
                      "Unknown experimental s2k",
                      s2k.experimental.data(),
                      s2k.experimental.size(),
                      true);
        return;
    }
    dst_print_halg(dst, "s2k hash algorithm", s2k.hash_alg);
    if ((s2k.specifier == PGP_S2KS_SALTED) ||
        (s2k.specifier == PGP_S2KS_ITERATED_AND_SALTED)) {
        dst_print_hex(dst, "s2k salt", s2k.salt, PGP_SALT_SIZE, false);
    }
    if (s2k.specifier == PGP_S2KS_ITERATED_AND_SALTED) {
        size_t real_iter = pgp_s2k_decode_iterations(s2k.iterations);
        dst_printf(dst, "s2k iterations: %zu (encoded as %u)\n", real_iter, s2k.iterations);
    }
}

static void
dst_print_time(pgp_dest_t &dst, const char *name, uint32_t time)
{
    auto str = rnp_ctime(time).substr(0, 24);
    dst_printf(dst,
               "%s: %zu (%s%s)\n",
               name,
               (size_t) time,
               rnp_y2k38_warning(time) ? ">=" : "",
               str.c_str());
}

static void
dst_print_expiration(pgp_dest_t &dst, const char *name, uint32_t seconds)
{
    if (seconds) {
        int days = seconds / (24 * 60 * 60);
        dst_printf(dst, "%s: %" PRIu32 " seconds (%d days)\n", name, seconds, days);
    } else {
        dst_printf(dst, "%s: 0 (never)\n", name);
    }
}

#define LINELEN 16

static void
dst_hexdump(pgp_dest_t &dst, const uint8_t *src, size_t length)
{
    size_t i;
    char   line[LINELEN + 1];

    for (i = 0; i < length; i++) {
        if (i % LINELEN == 0) {
            dst_printf(dst, "%.5zu | ", i);
        }
        dst_printf(dst, "%.02x ", (uint8_t) src[i]);
        line[i % LINELEN] = (isprint(src[i])) ? src[i] : '.';
        if (i % LINELEN == LINELEN - 1) {
            line[LINELEN] = 0x0;
            dst_printf(dst, " | %s\n", line);
        }
    }
    if (i % LINELEN != 0) {
        for (; i % LINELEN != 0; i++) {
            dst_printf(dst, "   ");
            line[i % LINELEN] = ' ';
        }
        line[LINELEN] = 0x0;
        dst_printf(dst, " | %s\n", line);
    }
}

static void
dst_hexdump(pgp_dest_t &dst, const std::vector<uint8_t> &data)
{
    dst_hexdump(dst, data.data(), data.size());
}

namespace rnp {
using namespace pgp;

void
DumpContext::copy_params(const DumpContext &ctx)
{
    dump_mpi = ctx.dump_mpi;
    dump_packets = ctx.dump_packets;
    dump_grips = ctx.dump_grips;
    /* this could be called only from upper layer dumper */
    layers = ctx.layers;
    stream_pkts = ctx.stream_pkts;
    failures = ctx.failures;
}

bool
DumpContext::get_aead_hdr(pgp_aead_hdr_t &hdr)
{
    uint8_t    encpkt[64] = {0};
    MemoryDest encdst(encpkt, sizeof(encpkt));

    mem_dest_discard_overflow(&encdst.dst(), true);

    if (stream_read_packet(&src, &encdst.dst())) {
        return false;
    }
    size_t len = std::min(encdst.writeb(), sizeof(encpkt));

    MemorySource memsrc(encpkt, len, false);
    return get_aead_src_hdr(&memsrc.src(), &hdr);
}

bool
DumpContext::skip_cleartext()
{
    char   buf[4096];
    size_t read = 0;
    size_t siglen = strlen(ST_SIG_BEGIN);
    char * hdrpos;

    while (!src.eof()) {
        if (!src.peek(buf, sizeof(buf) - 1, &read) || (read <= siglen)) {
            return false;
        }
        buf[read] = '\0';

        if ((hdrpos = strstr(buf, ST_SIG_BEGIN))) {
            /* +1 here is to skip \n on the beginning of ST_SIG_BEGIN */
            src.skip(hdrpos - buf + 1);
            return true;
        }
        src.skip(read - siglen + 1);
    }
    return false;
}

DumpContextDst::DumpContextDst(pgp_source_t &asrc, pgp_dest_t &adst) : DumpContext(asrc)
{
    auto ret = init_indent_dest(dst, &adst);
    if (ret) {
        RNP_LOG("failed to init indent dest");
        throw rnp_exception(RNP_ERROR_OUT_OF_MEMORY);
    }
}

DumpContextDst::~DumpContextDst()
{
    if (dst.param) {
        dst_close(&dst, false);
    }
}

void
DumpContextDst::dump_signature_subpacket(const pkt::sigsub::Raw &subpkt)
{
    auto sname = id_str_pair::lookup(sig_subpkt_type_map, subpkt.raw_type(), "Unknown");

    switch (subpkt.type()) {
    case pkt::sigsub::Type::CreationTime: {
        auto &sub = dynamic_cast<const pkt::sigsub::CreationTime &>(subpkt);
        dst_print_time(dst, sname, sub.time());
        break;
    }
    case pkt::sigsub::Type::ExpirationTime: {
        auto &sub = dynamic_cast<const pkt::sigsub::ExpirationTime &>(subpkt);
        dst_print_expiration(dst, sname, sub.time());
        break;
    }
    case pkt::sigsub::Type::ExportableCert: {
        auto &sub = dynamic_cast<const pkt::sigsub::ExportableCert &>(subpkt);
        dst_printf(dst, "%s: %d\n", sname, sub.exportable());
        break;
    }
    case pkt::sigsub::Type::Trust: {
        auto &sub = dynamic_cast<const pkt::sigsub::Trust &>(subpkt);
        dst_printf(
          dst, "%s: amount %" PRIu8 ", level %" PRIu8 "\n", sname, sub.amount(), sub.level());
        break;
    }
    case pkt::sigsub::Type::RegExp: {
        auto &sub = dynamic_cast<const pkt::sigsub::RegExp &>(subpkt);
        dst_print_str(dst, sname, sub.regexp());
        break;
    }
    case pkt::sigsub::Type::Revocable: {
        auto &sub = dynamic_cast<const pkt::sigsub::Revocable &>(subpkt);
        dst_printf(dst, "%s: %d\n", sname, sub.revocable());
        break;
    }
    case pkt::sigsub::Type::KeyExpirationTime: {
        auto &sub = dynamic_cast<const pkt::sigsub::KeyExpirationTime &>(subpkt);
        dst_print_expiration(dst, sname, sub.time());
        break;
    }
    case pkt::sigsub::Type::PreferredSymmetric: {
        auto &sub = dynamic_cast<const pkt::sigsub::PreferredSymmetric &>(subpkt);
        dst_print_algs(dst, "preferred symmetric algorithms", sub.algs(), symm_alg_map);
        break;
    }
    case pkt::sigsub::Type::RevocationKey: {
        auto &sub = dynamic_cast<const pkt::sigsub::RevocationKey &>(subpkt);
        dst_printf(dst, "%s\n", sname);
        dst_printf(dst, "class: %" PRIu8 "\n", sub.rev_class());
        dst_print_palg(dst, NULL, sub.alg());
        dst_print_fp(dst, "fingerprint", sub.fp());
        break;
    }
    case pkt::sigsub::Type::IssuerKeyID: {
        auto &sub = dynamic_cast<const pkt::sigsub::IssuerKeyID &>(subpkt);
        dst_print_keyid(dst, sname, sub.keyid());
        break;
    }
    case pkt::sigsub::Type::NotationData: {
        auto &sub = dynamic_cast<const pkt::sigsub::NotationData &>(subpkt);
        if (sub.human_readable()) {
            dst_printf(dst, "%s: %s = ", sname, sub.name().c_str());
            dst_printf(
              dst, "%.*s\n", (int) sub.value().size(), (const char *) sub.value().data());
        } else {
            char hex[64];
            vsnprinthex(hex, sizeof(hex), sub.value().data(), sub.value().size());
            dst_printf(dst, "%s: %s = ", sname, sub.name().c_str());
            dst_printf(dst, "0x%s (%zu bytes)\n", hex, sub.value().size());
        }
        break;
    }
    case pkt::sigsub::Type::PreferredHash: {
        auto &sub = dynamic_cast<const pkt::sigsub::PreferredHash &>(subpkt);
        dst_print_algs(dst, "preferred hash algorithms", sub.algs(), hash_alg_map);
        break;
    }
    case pkt::sigsub::Type::PreferredCompress: {
        auto &sub = dynamic_cast<const pkt::sigsub::PreferredCompress &>(subpkt);
        dst_print_algs(dst, "preferred compression algorithms", sub.algs(), z_alg_map);
        break;
    }
    case pkt::sigsub::Type::KeyserverPrefs: {
        auto &sub = dynamic_cast<const pkt::sigsub::KeyserverPrefs &>(subpkt);
        dst_printf(dst, "%s\n", sname);
        dst_printf(dst, "no-modify: %d\n", sub.no_modify());
        break;
    }
    case pkt::sigsub::Type::PreferredKeyserver: {
        auto &sub = dynamic_cast<const pkt::sigsub::PreferredKeyserver &>(subpkt);
        dst_print_str(dst, sname, sub.keyserver());
        break;
    }
    case pkt::sigsub::Type::PrimaryUserID: {
        auto &sub = dynamic_cast<const pkt::sigsub::PrimaryUserID &>(subpkt);
        dst_printf(dst, "%s: %d\n", sname, sub.primary());
        break;
    }
    case pkt::sigsub::Type::PolicyURI: {
        auto &sub = dynamic_cast<const pkt::sigsub::PolicyURI &>(subpkt);
        dst_print_str(dst, sname, sub.URI());
        break;
    }
    case pkt::sigsub::Type::KeyFlags: {
        auto &  sub = dynamic_cast<const pkt::sigsub::KeyFlags &>(subpkt);
        uint8_t flg = sub.flags();
        dst_printf(dst, "%s: 0x%02x ( ", sname, flg);
        dst_printf(dst, "%s", flg ? "" : "none");
        dst_printf(dst, "%s", flg & PGP_KF_CERTIFY ? "certify " : "");
        dst_printf(dst, "%s", flg & PGP_KF_SIGN ? "sign " : "");
        dst_printf(dst, "%s", flg & PGP_KF_ENCRYPT_COMMS ? "encrypt_comm " : "");
        dst_printf(dst, "%s", flg & PGP_KF_ENCRYPT_STORAGE ? "encrypt_storage " : "");
        dst_printf(dst, "%s", flg & PGP_KF_SPLIT ? "split " : "");
        dst_printf(dst, "%s", flg & PGP_KF_AUTH ? "auth " : "");
        dst_printf(dst, "%s", flg & PGP_KF_SHARED ? "shared " : "");
        dst_printf(dst, ")\n");
        break;
    }
    case pkt::sigsub::Type::SignersUserID: {
        auto &sub = dynamic_cast<const pkt::sigsub::SignersUserID &>(subpkt);
        dst_print_str(dst, sname, sub.signer());
        break;
    }
    case pkt::sigsub::Type::RevocationReason: {
        auto &sub = dynamic_cast<const pkt::sigsub::RevocationReason &>(subpkt);
        auto  reason = id_str_pair::lookup(revoc_reason_map, sub.code(), "Unknown");
        dst_printf(dst, "%s: %" PRIu8 " (%s)\n", sname, sub.code(), reason);
        dst_print_str(dst, "message", sub.reason());
        break;
    }
    case pkt::sigsub::Type::Features: {
        auto &sub = dynamic_cast<const pkt::sigsub::Features &>(subpkt);
        dst_printf(dst, "%s: 0x%02x ( ", sname, sub.features());
        dst_printf(dst, "%s", sub.features() & PGP_KEY_FEATURE_MDC ? "mdc " : "");
        dst_printf(dst, "%s", sub.features() & PGP_KEY_FEATURE_AEAD ? "aead " : "");
        dst_printf(dst, "%s", sub.features() & PGP_KEY_FEATURE_V5 ? "v5 keys " : "");
#if defined(ENABLE_CRYPTO_REFRESH)
        dst_printf(dst, "%s", sub.features() & PGP_KEY_FEATURE_SEIPDV2 ? "SEIPD v2 " : "");
#endif
        dst_printf(dst, ")\n");
        break;
    }
    case pkt::sigsub::Type::EmbeddedSignature: {
        auto &sub = dynamic_cast<const pkt::sigsub::EmbeddedSignature &>(subpkt);
        dst_printf(dst, "%s:\n", sname);
        pkt::Signature sig(*sub.signature());
        dump_signature_pkt(sig);
        break;
    }
    case pkt::sigsub::Type::IssuerFingerprint: {
        auto &sub = dynamic_cast<const pkt::sigsub::IssuerFingerprint &>(subpkt);
        dst_print_fp(dst, sname, sub.fp());
        break;
    }
    case pkt::sigsub::Type::PreferredAEAD: {
        auto &sub = dynamic_cast<const pkt::sigsub::PreferredAEAD &>(subpkt);
        dst_print_algs(dst, "preferred aead algorithms", sub.algs(), aead_alg_map);
        break;
    }
    default:
        if (!dump_packets) {
            indent_dest_increase(dst);
            dst_hexdump(dst, subpkt.data());
            indent_dest_decrease(dst);
        }
    }
}

void
DumpContextDst::dump_signature_subpackets(const pkt::Signature &sig, bool hashed)
{
    bool empty = true;

    for (auto &subpkt : sig.subpkts) {
        if (subpkt->hashed() != hashed) {
            continue;
        }
        empty = false;
        dst_printf(
          dst, ":type %" PRIu8 ", len %zu", subpkt->raw_type(), subpkt->data().size());
        dst_printf(dst, "%s\n", subpkt->critical() ? ", critical" : "");
        if (dump_packets) {
            dst_printf(dst, ":subpacket contents:\n");
            indent_dest_increase(dst);
            dst_hexdump(dst, subpkt->data());
            indent_dest_decrease(dst);
        }
        dump_signature_subpacket(*subpkt);
    }

    if (empty) {
        dst_printf(dst, "none\n");
    }
}

void
DumpContextDst::dump_signature_pkt(const pkt::Signature &sig)
{
    indent_dest_increase(dst);

    dst_printf(dst, "version: %d\n", (int) sig.version);
    dst_print_sig_type(dst, "type", sig.type());
    if (sig.version < PGP_V4) {
        dst_print_time(dst, "creation time", sig.creation_time);
        dst_print_keyid(dst, "signing key id", sig.signer);
    }
    dst_print_palg(dst, NULL, sig.palg);
    dst_print_halg(dst, NULL, sig.halg);

    if (sig.version >= PGP_V4) {
        dst_printf(dst, "hashed subpackets:\n");
        indent_dest_increase(dst);
        dump_signature_subpackets(sig, true);
        indent_dest_decrease(dst);

        dst_printf(dst, "unhashed subpackets:\n");
        indent_dest_increase(dst);
        dump_signature_subpackets(sig, false);
        indent_dest_decrease(dst);
    }

    dst_print_hex(dst, "lbits", sig.lbits.data(), sig.lbits.size(), false);
    dst_printf(dst, "signature material:\n");
    indent_dest_increase(dst);

    auto material = sig.parse_material();
    assert(material);
    /* LCOV_EXCL_START */
    if (!material) {
        indent_dest_decrease(dst);
        indent_dest_decrease(dst);
        return;
    }
    /* LCOV_EXCL_END */
    switch (sig.palg) {
    case PGP_PKA_RSA:
    case PGP_PKA_RSA_ENCRYPT_ONLY:
    case PGP_PKA_RSA_SIGN_ONLY: {
        auto &rsa = dynamic_cast<const RSASigMaterial &>(*material);
        dst_print_mpi(dst, "rsa s", rsa.sig.s, dump_mpi);
        break;
    }
    case PGP_PKA_DSA: {
        auto &dsa = dynamic_cast<const DSASigMaterial &>(*material);
        dst_print_mpi(dst, "dsa r", dsa.sig.r, dump_mpi);
        dst_print_mpi(dst, "dsa s", dsa.sig.s, dump_mpi);
        break;
    }
    case PGP_PKA_EDDSA:
    case PGP_PKA_ECDSA:
    case PGP_PKA_SM2:
    case PGP_PKA_ECDH: {
        auto &ec = dynamic_cast<const ECSigMaterial &>(*material);
        dst_print_mpi(dst, "ecc r", ec.sig.r, dump_mpi);
        dst_print_mpi(dst, "ecc s", ec.sig.s, dump_mpi);
        break;
    }
    /* Wasn't able to find ElGamal sig artifacts so let's ignore this for coverage */
    /* LCOV_EXCL_START */
    case PGP_PKA_ELGAMAL:
    case PGP_PKA_ELGAMAL_ENCRYPT_OR_SIGN: {
        auto &eg = dynamic_cast<const EGSigMaterial &>(*material);
        dst_print_mpi(dst, "eg r", eg.sig.r, dump_mpi);
        dst_print_mpi(dst, "eg s", eg.sig.s, dump_mpi);
        break;
    }
    /* LCOV_EXCL_END */
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_PKA_ED25519: {
        auto &ed = dynamic_cast<const Ed25519SigMaterial &>(*material);
        dst_print_vec(dst, "ed25519 sig", ed.sig.sig, dump_mpi);
        break;
    }
#endif
#if defined(ENABLE_PQC)
    case PGP_PKA_DILITHIUM3_ED25519:
        FALLTHROUGH_STATEMENT;
    // TODO: Add case for PGP_PKA_DILITHIUM5_ED448 with FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_P384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_BP256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_BP384: {
        auto &dilithium = dynamic_cast<const DilithiumSigMaterial &>(*material);
        dst_print_vec(dst, "mldsa-ecdsa/eddsa sig", dilithium.sig.sig, dump_mpi);
        break;
    }
    case PGP_PKA_SPHINCSPLUS_SHA2:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_SPHINCSPLUS_SHAKE: {
        auto &slhdsa = dynamic_cast<const SlhdsaSigMaterial &>(*material);
        dst_print_vec(dst, "slhdsa sig", slhdsa.sig.sig, dump_mpi);
        break;
    }
#endif
    default:
        dst_printf(dst, "unknown algorithm\n");
    }
    indent_dest_decrease(dst);
    indent_dest_decrease(dst);
}

rnp_result_t
DumpContextDst::dump_signature()
{
    dst_printf(dst, "Signature packet\n");
    pkt::Signature sig;
    auto           ret = sig.parse(src);
    if (ret) {
        indent_dest_increase(dst);
        dst_printf(dst, "failed to parse\n");
        indent_dest_decrease(dst);
        return ret;
    }
    dump_signature_pkt(sig);
    return RNP_SUCCESS;
}

void
DumpContextDst::dump_key_material(const KeyMaterial *material)
{
    if (!material) {
        return;
    }
    switch (material->alg()) {
    case PGP_PKA_RSA:
    case PGP_PKA_RSA_ENCRYPT_ONLY:
    case PGP_PKA_RSA_SIGN_ONLY: {
        auto &rsa = dynamic_cast<const RSAKeyMaterial &>(*material);
        dst_print_mpi(dst, "rsa n", rsa.n(), dump_mpi);
        dst_print_mpi(dst, "rsa e", rsa.e(), dump_mpi);
        return;
    }
    case PGP_PKA_DSA: {
        auto &dsa = dynamic_cast<const DSAKeyMaterial &>(*material);
        dst_print_mpi(dst, "dsa p", dsa.p(), dump_mpi);
        dst_print_mpi(dst, "dsa q", dsa.q(), dump_mpi);
        dst_print_mpi(dst, "dsa g", dsa.g(), dump_mpi);
        dst_print_mpi(dst, "dsa y", dsa.y(), dump_mpi);
        return;
    }
    case PGP_PKA_ELGAMAL:
    case PGP_PKA_ELGAMAL_ENCRYPT_OR_SIGN: {
        auto &eg = dynamic_cast<const EGKeyMaterial &>(*material);
        dst_print_mpi(dst, "eg p", eg.p(), dump_mpi);
        dst_print_mpi(dst, "eg g", eg.g(), dump_mpi);
        dst_print_mpi(dst, "eg y", eg.y(), dump_mpi);
        return;
    }
    case PGP_PKA_ECDSA:
    case PGP_PKA_EDDSA:
    case PGP_PKA_SM2: {
        auto &ec = dynamic_cast<const ECKeyMaterial &>(*material);
        auto  cdesc = ec::Curve::get(ec.curve());
        dst_print_mpi(dst, "ecc p", ec.p(), dump_mpi);
        dst_printf(dst, "ecc curve: %s\n", cdesc ? cdesc->pgp_name : "unknown");
        return;
    }
    case PGP_PKA_ECDH: {
        auto &ec = dynamic_cast<const ECDHKeyMaterial &>(*material);
        auto  cdesc = ec::Curve::get(ec.curve());
        /* Common EC fields */
        dst_print_mpi(dst, "ecdh p", ec.p(), dump_mpi);
        dst_printf(dst, "ecdh curve: %s\n", cdesc ? cdesc->pgp_name : "unknown");
        /* ECDH-only fields */
        dst_print_halg(dst, "ecdh hash algorithm", ec.kdf_hash_alg());
        dst_printf(dst, "ecdh key wrap algorithm: %d\n", (int) ec.key_wrap_alg());
        return;
    }
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_PKA_ED25519: {
        auto &ed25519 = dynamic_cast<const Ed25519KeyMaterial &>(*material);
        dst_print_vec(dst, "ed25519", ed25519.pub(), dump_mpi);
        return;
    }
    case PGP_PKA_X25519: {
        auto &x25519 = dynamic_cast<const X25519KeyMaterial &>(*material);
        dst_print_vec(dst, "x25519", x25519.pub(), dump_mpi);
        return;
    }
#endif
#if defined(ENABLE_PQC)
    case PGP_PKA_KYBER768_X25519:
        FALLTHROUGH_STATEMENT;
    // TODO: Add case for PGP_PKA_KYBER1024_X448 with FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER768_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER1024_P384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER768_BP256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER1024_BP384: {
        auto &kyber = dynamic_cast<const MlkemEcdhKeyMaterial &>(*material);
        dst_print_vec(dst, "mlkem-ecdh encoded pubkey", kyber.pub().get_encoded(), dump_mpi);
        return;
    }
    case PGP_PKA_DILITHIUM3_ED25519:
        FALLTHROUGH_STATEMENT;
    // TODO: Add case for PGP_PKA_DILITHIUM5_ED448 with FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_P384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_BP256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_BP384: {
        auto &dilithium = dynamic_cast<const DilithiumEccKeyMaterial &>(*material);
        dst_print_vec(
          dst, "mldsa-ecdsa/eddsa encodced pubkey", dilithium.pub().get_encoded(), dump_mpi);
        return;
    }
    case PGP_PKA_SPHINCSPLUS_SHA2:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_SPHINCSPLUS_SHAKE: {
        auto &sphincs = dynamic_cast<const SlhdsaKeyMaterial &>(*material);
        dst_print_vec(dst, "slhdsa encoded pubkey", sphincs.pub().get_encoded(), dump_mpi);
        return;
    }
#endif
    default:
        dst_printf(dst, "unknown public key algorithm\n");
    }
}

rnp_result_t
DumpContextDst::dump_key()
{
    pgp_key_pkt_t key;
    auto          ret = key.parse(src);
    if (ret) {
        return ret;
    }

    dst_printf(dst, "%s packet\n", id_str_pair::lookup(key_type_map, key.tag, "Unknown"));
    indent_dest_increase(dst);

    dst_printf(dst, "version: %d\n", (int) key.version);
    dst_print_time(dst, "creation time", key.creation_time);
    if (key.version < PGP_V4) {
        dst_printf(dst, "v3 validity days: %d\n", (int) key.v3_days);
    }
    dst_print_palg(dst, NULL, key.alg);
    if (key.version == PGP_V5) {
        dst_printf(dst, "v5 public key material length: %" PRIu32 "\n", key.v5_pub_len);
    }
    dst_printf(dst, "public key material:\n");
    indent_dest_increase(dst);
    dump_key_material(key.material.get());
    indent_dest_decrease(dst);

    if (is_secret_key_pkt(key.tag)) {
        dst_printf(dst, "secret key material:\n");
        indent_dest_increase(dst);

        dst_printf(dst, "s2k usage: %d\n", (int) key.sec_protection.s2k.usage);
        if (key.version == PGP_V5) {
            dst_printf(dst, "v5 s2k length: %" PRIu8 "\n", key.v5_s2k_len);
        }
        if ((key.sec_protection.s2k.usage == PGP_S2KU_ENCRYPTED) ||
            (key.sec_protection.s2k.usage == PGP_S2KU_ENCRYPTED_AND_HASHED)) {
            dst_print_salg(dst, NULL, key.sec_protection.symm_alg);
            dst_print_s2k(dst, key.sec_protection.s2k);
            if (key.sec_protection.s2k.specifier != PGP_S2KS_EXPERIMENTAL) {
                size_t bl_size = pgp_block_size(key.sec_protection.symm_alg);
                if (bl_size) {
                    dst_print_hex(dst, "cipher iv", key.sec_protection.iv, bl_size, true);
                } else {
                    dst_printf(dst, "cipher iv: unknown algorithm\n");
                }
            }
        }

        if (key.version == PGP_V5) {
            dst_printf(dst, "v5 secret key data length: %" PRIu32 "\n", key.v5_sec_len);
        }
        if (!key.sec_protection.s2k.usage) {
            dst_printf(dst, "cleartext secret key data: %zu bytes\n", key.sec_data.size());
        } else {
            dst_printf(dst, "encrypted secret key data: %zu bytes\n", key.sec_data.size());
        }
        indent_dest_decrease(dst);
    }

    try {
        Fingerprint fp(key);
        dst_print_keyid(dst, "keyid", fp.keyid());

        if (dump_grips) {
            dst_print_fp(dst, "fingerprint", fp, false);
        }
    } catch (const std::exception &e) {
        dst_printf(dst, "failed to calculate fingerprint and/or keyid\n");
    }

    if (dump_grips) {
        if (key.material) {
            KeyGrip grip = key.material->grip();
            dst_print_hex(dst, "grip", grip.data(), grip.size(), false);
        } else {
            dst_printf(dst, "grip: failed to calculate\n");
        }
    }

    indent_dest_decrease(dst);
    return RNP_SUCCESS;
}

rnp_result_t
DumpContextDst::dump_userid()
{
    pgp_userid_pkt_t uid;
    auto             ret = uid.parse(src);
    if (ret) {
        return ret;
    }

    const char *utype = NULL;
    switch (uid.tag) {
    case PGP_PKT_USER_ID:
        utype = "UserID";
        break;
    case PGP_PKT_USER_ATTR:
        utype = "UserAttr";
        break;
    default:
        utype = "Unknown user id";
    }

    dst_printf(dst, "%s packet\n", utype);
    indent_dest_increase(dst);

    switch (uid.tag) {
    case PGP_PKT_USER_ID:
        dst_printf(dst, "id: ");
        dst_write(dst, uid.uid);
        dst_printf(dst, "\n");
        break;
    case PGP_PKT_USER_ATTR:
        dst_printf(dst, "id: (%zu bytes of data)\n", uid.uid.size());
        break;
    default:;
    }

    indent_dest_decrease(dst);
    return RNP_SUCCESS;
}

rnp_result_t
DumpContextDst::dump_pk_session_key()
{
    pgp_pk_sesskey_t pkey;
    auto             ret = pkey.parse(src);
    if (ret) {
        return ret;
    }
    auto material = pkey.parse_material();
    if (!material) {
        return RNP_ERROR_BAD_FORMAT;
    }

    dst_printf(dst, "Public-key encrypted session key packet\n");
    indent_dest_increase(dst);

    dst_printf(dst, "version: %d\n", (int) pkey.version);
#if defined(ENABLE_CRYPTO_REFRESH)
    if (pkey.version == PGP_PKSK_V6) {
        dst_print_fp(dst, "fingerprint", pkey.fp);
    } else {
        dst_print_keyid(dst, "key id", pkey.key_id);
    }
#else
    dst_print_keyid(dst, "key id", pkey.key_id);
#endif
    dst_print_palg(dst, NULL, pkey.alg);
    dst_printf(dst, "encrypted material:\n");
    indent_dest_increase(dst);

    switch (pkey.alg) {
    case PGP_PKA_RSA:
    case PGP_PKA_RSA_ENCRYPT_ONLY:
    case PGP_PKA_RSA_SIGN_ONLY: {
        auto &rsa = dynamic_cast<const RSAEncMaterial &>(*material).enc;
        dst_print_mpi(dst, "rsa m", rsa.m, dump_mpi);
        break;
    }
    case PGP_PKA_ELGAMAL:
    case PGP_PKA_ELGAMAL_ENCRYPT_OR_SIGN: {
        auto &eg = dynamic_cast<const EGEncMaterial &>(*material).enc;
        dst_print_mpi(dst, "eg g", eg.g, dump_mpi);
        dst_print_mpi(dst, "eg m", eg.m, dump_mpi);
        break;
    }
    case PGP_PKA_SM2: {
        auto &sm2 = dynamic_cast<const SM2EncMaterial &>(*material).enc;
        dst_print_mpi(dst, "sm2 m", sm2.m, dump_mpi);
        break;
    }
    case PGP_PKA_ECDH: {
        auto &ecdh = dynamic_cast<const ECDHEncMaterial &>(*material).enc;
        dst_print_mpi(dst, "ecdh p", ecdh.p, dump_mpi);
        if (dump_mpi) {
            dst_print_hex(dst, "ecdh m", ecdh.m.data(), ecdh.m.size(), true);
        } else {
            dst_printf(dst, "ecdh m: %zu bytes\n", ecdh.m.size());
        }
        break;
    }
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_PKA_X25519: {
        auto &x25519 = dynamic_cast<const X25519EncMaterial &>(*material).enc;
        dst_print_vec(dst, "x25519 ephemeral public key", x25519.eph_key, dump_mpi);
        dst_print_vec(dst, "x25519 encrypted session key", x25519.enc_sess_key, dump_mpi);
        break;
    }
#endif
#if defined(ENABLE_PQC)
    case PGP_PKA_KYBER768_X25519:
        FALLTHROUGH_STATEMENT;
    // TODO: Add case for PGP_PKA_KYBER1024_X448 with FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER768_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER1024_P384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER768_BP256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER1024_BP384: {
        auto &mlkem = dynamic_cast<const MlkemEcdhEncMaterial &>(*material).enc;
        dst_print_vec(
          dst, "mlkem-ecdh composite ciphertext", mlkem.composite_ciphertext, dump_mpi);
        dst_print_vec(dst, "mlkem-ecdh wrapped session key", mlkem.wrapped_sesskey, dump_mpi);
        break;
    }
#endif
    default:
        dst_printf(dst, "unknown public key algorithm\n");
    }

    indent_dest_decrease(dst);
    indent_dest_decrease(dst);
    return RNP_SUCCESS;
}

rnp_result_t
DumpContextDst::dump_sk_session_key()
{
    pgp_sk_sesskey_t skey;
    auto             ret = skey.parse(src);
    if (ret) {
        return ret;
    }

    dst_printf(dst, "Symmetric-key encrypted session key packet\n");
    indent_dest_increase(dst);
    dst_printf(dst, "version: %d\n", (int) skey.version);
    dst_print_salg(dst, NULL, skey.alg);
    if (skey.version == PGP_SKSK_V5) {
        dst_print_aalg(dst, NULL, skey.aalg);
    }
    dst_print_s2k(dst, skey.s2k);
    if (skey.version == PGP_SKSK_V5) {
        dst_print_hex(dst, "aead iv", skey.iv, skey.ivlen, true);
    }
    dst_print_hex(dst, "encrypted key", skey.enckey, skey.enckeylen, true);
    indent_dest_decrease(dst);

    return RNP_SUCCESS;
}

rnp_result_t
DumpContextDst::dump_aead_encrypted()
{
    dst_printf(dst, "AEAD-encrypted data packet\n");

    pgp_aead_hdr_t aead{};
    if (!get_aead_hdr(aead)) {
        dst_printf(dst, "ERROR: failed to read AEAD header\n");
        return RNP_ERROR_READ;
    }

    indent_dest_increase(dst);

    dst_printf(dst, "version: %d\n", (int) aead.version);
    dst_print_salg(dst, NULL, aead.ealg);
    dst_print_aalg(dst, NULL, aead.aalg);
    dst_printf(dst, "chunk size: %d\n", (int) aead.csize);
    dst_print_hex(dst, "initialization vector", aead.iv, aead.ivlen, true);

    indent_dest_decrease(dst);
    return RNP_SUCCESS;
}

rnp_result_t
DumpContextDst::dump_encrypted(int tag)
{
    switch (tag) {
    case PGP_PKT_SE_DATA:
        dst_printf(dst, "Symmetrically-encrypted data packet\n\n");
        break;
    case PGP_PKT_SE_IP_DATA:
        dst_printf(dst, "Symmetrically-encrypted integrity protected data packet\n\n");
        break;
    case PGP_PKT_AEAD_ENCRYPTED:
        return dump_aead_encrypted();
    default:
        dst_printf(dst, "Unknown encrypted data packet\n\n");
        break;
    }

    return stream_skip_packet(&src);
}

rnp_result_t
DumpContextDst::dump_one_pass()
{
    pgp_one_pass_sig_t onepass;
    auto               ret = onepass.parse(src);
    if (ret) {
        return ret;
    }

    dst_printf(dst, "One-pass signature packet\n");
    indent_dest_increase(dst);

    dst_printf(dst, "version: %d\n", (int) onepass.version);
    dst_print_sig_type(dst, NULL, onepass.type);
    dst_print_halg(dst, NULL, onepass.halg);
    dst_print_palg(dst, NULL, onepass.palg);
    dst_print_keyid(dst, "signing key id", onepass.keyid);
    dst_printf(dst, "nested: %d\n", (int) onepass.nested);

    indent_dest_decrease(dst);
    return RNP_SUCCESS;
}

rnp_result_t
DumpContextDst::dump_compressed()
{
    std::unique_ptr<Source> zsrc(new Source());
    auto                    ret = init_compressed_src(&zsrc->src(), &src);
    if (ret) {
        return ret;
    }

    dst_printf(dst, "Compressed data packet\n");
    indent_dest_increase(dst);

    uint8_t zalg = 0;
    get_compressed_src_alg(&zsrc->src(), &zalg);
    dst_print_zalg(dst, NULL, (pgp_compression_type_t) zalg);
    dst_printf(dst, "Decompressed contents:\n");

    std::unique_ptr<DumpContextDst> ctx(new DumpContextDst(zsrc->src(), dst));
    ctx->copy_params(*this);
    ret = ctx->dump(true);
    copy_params(*ctx);
    indent_dest_decrease(dst);
    return ret;
}

rnp_result_t
DumpContextDst::dump_literal()
{
    Source lsrc;
    auto   ret = init_literal_src(&lsrc.src(), &src);
    if (ret) {
        return ret;
    }

    dst_printf(dst, "Literal data packet\n");
    indent_dest_increase(dst);

    auto &lhdr = get_literal_src_hdr(lsrc.src());
    dst_printf(dst, "data format: '%c'\n", lhdr.format);
    dst_printf(dst, "filename: %s (len %" PRIu8 ")\n", lhdr.fname, lhdr.fname_len);
    dst_print_time(dst, "timestamp", lhdr.timestamp);

    ret = RNP_SUCCESS;
    while (!lsrc.eof()) {
        uint8_t readbuf[16384];
        size_t  read = 0;
        if (!lsrc.src().read(readbuf, sizeof(readbuf), &read)) {
            ret = RNP_ERROR_READ;
            break;
        }
    }

    dst_printf(dst, "data bytes: %zu\n", lsrc.readb());
    indent_dest_decrease(dst);
    return ret;
}

rnp_result_t
DumpContextDst::dump_marker()
{
    dst_printf(dst, "Marker packet\n");
    indent_dest_increase(dst);
    auto ret = stream_parse_marker(src);
    dst_printf(dst, "contents: %s\n", ret ? "invalid" : PGP_MARKER_CONTENTS);
    indent_dest_decrease(dst);
    return ret;
}

rnp_result_t
DumpContextDst::dump_raw_packets()
{
    char         msg[1024 + PGP_MAX_HEADER_SIZE] = {0};
    char         smsg[128] = {0};
    rnp_result_t ret = RNP_ERROR_GENERIC;

    if (src.eof()) {
        return RNP_SUCCESS;
    }

    /* do not allow endless recursion */
    if (++layers > MAXIMUM_NESTING_LEVEL) {
        RNP_LOG("Too many OpenPGP nested layers during the dump.");
        dst_printf(dst, ":too many OpenPGP packet layers, stopping.\n");
        return RNP_SUCCESS;
    }

    while (!src.eof()) {
        pgp_packet_hdr_t hdr{};
        size_t           off = src.readb;
        rnp_result_t     hdrret = stream_peek_packet_hdr(&src, &hdr);
        if (hdrret) {
            return hdrret;
        }

        if (hdr.partial) {
            snprintf(msg, sizeof(msg), "partial len");
        } else if (hdr.indeterminate) {
            snprintf(msg, sizeof(msg), "indeterminate len");
        } else {
            snprintf(msg, sizeof(msg), "len %zu", hdr.pkt_len);
        }
        vsnprinthex(smsg, sizeof(smsg), hdr.hdr, hdr.hdr_len);
        dst_printf(
          dst, ":off %zu: packet header 0x%s (tag %d, %s)\n", off, smsg, hdr.tag, msg);

        if (dump_packets) {
            size_t rlen = hdr.pkt_len + hdr.hdr_len;
            bool   part = false;

            if (!hdr.pkt_len || (rlen > 1024 + hdr.hdr_len)) {
                rlen = 1024 + hdr.hdr_len;
                part = true;
            }

            dst_printf(dst, ":off %zu: packet contents ", off + hdr.hdr_len);
            if (!src.peek(msg, rlen, &rlen)) {
                dst_printf(dst, "- failed to read\n");
            } else {
                rlen -= hdr.hdr_len;
                if (part || (rlen < hdr.pkt_len)) {
                    dst_printf(dst, "(first %zu bytes)\n", rlen);
                } else {
                    dst_printf(dst, "(%zu bytes)\n", rlen);
                }
                indent_dest_increase(dst);
                dst_hexdump(dst, (uint8_t *) msg + hdr.hdr_len, rlen);
                indent_dest_decrease(dst);
            }
            dst_printf(dst, "\n");
        }

        switch (hdr.tag) {
        case PGP_PKT_SIGNATURE:
            ret = dump_signature();
            break;
        case PGP_PKT_SECRET_KEY:
        case PGP_PKT_PUBLIC_KEY:
        case PGP_PKT_SECRET_SUBKEY:
        case PGP_PKT_PUBLIC_SUBKEY:
            ret = dump_key();
            break;
        case PGP_PKT_USER_ID:
        case PGP_PKT_USER_ATTR:
            ret = dump_userid();
            break;
        case PGP_PKT_PK_SESSION_KEY:
            ret = dump_pk_session_key();
            break;
        case PGP_PKT_SK_SESSION_KEY:
            ret = dump_sk_session_key();
            break;
        case PGP_PKT_SE_DATA:
        case PGP_PKT_SE_IP_DATA:
        case PGP_PKT_AEAD_ENCRYPTED:
            stream_pkts++;
            ret = dump_encrypted(hdr.tag);
            break;
        case PGP_PKT_ONE_PASS_SIG:
            ret = dump_one_pass();
            break;
        case PGP_PKT_COMPRESSED:
            stream_pkts++;
            ret = dump_compressed();
            break;
        case PGP_PKT_LITDATA:
            stream_pkts++;
            ret = dump_literal();
            break;
        case PGP_PKT_MARKER:
            ret = dump_marker();
            break;
        case PGP_PKT_TRUST:
        case PGP_PKT_MDC:
            dst_printf(dst, "Skipping unhandled pkt: %d\n\n", (int) hdr.tag);
            ret = stream_skip_packet(&src);
            break;
        default:
            dst_printf(dst, "Skipping Unknown pkt: %d\n\n", (int) hdr.tag);
            ret = stream_skip_packet(&src);
            if (ret) {
                return ret;
            }
            if (++failures > MAXIMUM_ERROR_PKTS) {
                RNP_LOG("too many packet dump errors or unknown packets.");
                return ret;
            }
        }

        if (ret) {
            RNP_LOG("failed to process packet");
            if (++failures > MAXIMUM_ERROR_PKTS) {
                RNP_LOG("too many packet dump errors.");
                return ret;
            }
        }

        if (stream_pkts > MAXIMUM_STREAM_PKTS) {
            RNP_LOG("Too many OpenPGP stream packets during the dump.");
            dst_printf(dst, ":too many OpenPGP stream packets, stopping.\n");
            return RNP_SUCCESS;
        }
    }
    return RNP_SUCCESS;
}

rnp_result_t
DumpContextDst::dump(bool raw_only)
{
    /* check whether source is cleartext - then skip till the signature */
    if (!raw_only && src.is_cleartext()) {
        dst_printf(dst, ":cleartext signed data\n");
        if (!skip_cleartext()) {
            RNP_LOG("malformed cleartext signed data");
            return RNP_ERROR_BAD_FORMAT;
        }
    }

    /* check whether source is armored */
    if (!raw_only && src.is_armored()) {
        std::unique_ptr<Source> armor(new Source());
        auto                    ret = init_armored_src(&armor->src(), &src);
        if (ret) {
            RNP_LOG("failed to parse armored data");
            return ret;
        }
        dst_printf(dst, ":armored input\n");

        std::unique_ptr<DumpContextDst> ctx(new DumpContextDst(armor->src(), dst));
        ctx->copy_params(*this);
        return ctx->dump(true);
    }

    if (src.eof()) {
        dst_printf(dst, ":empty input\n");
        return RNP_SUCCESS;
    }

    return dump_raw_packets();
}

static bool
obj_add_intstr_json(json_object *obj, const char *name, int val, const id_str_pair map[])
{
    if (!json_add(obj, name, val)) {
        return false; // LCOV_EXCL_LINE
    }
    if (!map) {
        return true;
    }
    char        namestr[64] = {0};
    const char *str = id_str_pair::lookup(map, val, "Unknown");
    snprintf(namestr, sizeof(namestr), "%s.str", name);
    return json_add(obj, namestr, str);
}

static bool
obj_add_mpi_json(json_object *obj, const char *name, const mpi &mpi, bool contents)
{
    char strname[64] = {0};
    snprintf(strname, sizeof(strname), "%s.bits", name);
    if (!json_add(obj, strname, (int) mpi.bits())) {
        return false; // LCOV_EXCL_LINE
    }
    if (!contents) {
        return true;
    }
    snprintf(strname, sizeof(strname), "%s.raw", name);
    return json_add_hex(obj, strname, mpi.data(), mpi.size());
}

static bool
subpacket_obj_add_algs(json_object *               obj,
                       const char *                name,
                       const std::vector<uint8_t> &algs,
                       const id_str_pair           map[])
{
    json_object *jso_algs = json_object_new_array();
    if (!jso_algs || !json_add(obj, name, jso_algs)) {
        return false; // LCOV_EXCL_LINE
    }
    for (auto &alg : algs) {
        if (!json_array_add(jso_algs, json_object_new_int(alg))) {
            return false; // LCOV_EXCL_LINE
        }
    }
    if (!map) {
        return true;
    }

    char strname[64] = {0};
    snprintf(strname, sizeof(strname), "%s.str", name);

    jso_algs = json_object_new_array();
    if (!jso_algs || !json_add(obj, strname, jso_algs)) {
        return false; // LCOV_EXCL_LINE
    }
    for (auto &alg : algs) {
        if (!json_array_add(jso_algs, id_str_pair::lookup(map, alg, "Unknown"))) {
            return false; // LCOV_EXCL_LINE
        }
    }
    return true;
}

static bool
obj_add_s2k_json(json_object *obj, pgp_s2k_t *s2k)
{
    json_object *s2k_obj = json_object_new_object();
    if (!json_add(obj, "s2k", s2k_obj)) {
        return false; // LCOV_EXCL_LINE
    }
    if (!json_add(s2k_obj, "specifier", (int) s2k->specifier)) {
        return false; // LCOV_EXCL_LINE
    }
    if ((s2k->specifier == PGP_S2KS_EXPERIMENTAL) && s2k->gpg_ext_num) {
        if (!json_add(s2k_obj, "gpg extension", (int) s2k->gpg_ext_num)) {
            return false; // LCOV_EXCL_LINE
        }
        if (s2k->gpg_ext_num == PGP_S2K_GPG_SMARTCARD) {
            size_t slen = s2k->gpg_serial_len > 16 ? 16 : s2k->gpg_serial_len;
            if (!json_add_hex(s2k_obj, "card serial number", s2k->gpg_serial, slen)) {
                return false; // LCOV_EXCL_LINE
            }
        }
    }
    if (s2k->specifier == PGP_S2KS_EXPERIMENTAL) {
        return json_add_hex(s2k_obj, "unknown experimental", s2k->experimental);
    }
    if (!obj_add_intstr_json(s2k_obj, "hash algorithm", s2k->hash_alg, hash_alg_map)) {
        return false; // LCOV_EXCL_LINE
    }
    if (((s2k->specifier == PGP_S2KS_SALTED) ||
         (s2k->specifier == PGP_S2KS_ITERATED_AND_SALTED)) &&
        !json_add_hex(s2k_obj, "salt", s2k->salt, PGP_SALT_SIZE)) {
        return false; // LCOV_EXCL_LINE
    }
    if (s2k->specifier == PGP_S2KS_ITERATED_AND_SALTED) {
        size_t real_iter = pgp_s2k_decode_iterations(s2k->iterations);
        if (!json_add(s2k_obj, "iterations", (uint64_t) real_iter)) {
            return false; // LCOV_EXCL_LINE
        }
    }
    return true;
}

bool
DumpContextJson::dump_signature_subpacket(const pkt::sigsub::Raw &subpkt, json_object *obj)
{
    switch (subpkt.type()) {
    case pkt::sigsub::Type::CreationTime: {
        auto &sub = dynamic_cast<const pkt::sigsub::CreationTime &>(subpkt);
        return json_add(obj, "creation time", (uint64_t) sub.time());
    }
    case pkt::sigsub::Type::ExpirationTime: {
        auto &sub = dynamic_cast<const pkt::sigsub::ExpirationTime &>(subpkt);
        return json_add(obj, "expiration time", (uint64_t) sub.time());
    }
    case pkt::sigsub::Type::ExportableCert: {
        auto &sub = dynamic_cast<const pkt::sigsub::ExportableCert &>(subpkt);
        return json_add(obj, "exportable", sub.exportable());
    }
    case pkt::sigsub::Type::Trust: {
        auto &sub = dynamic_cast<const pkt::sigsub::Trust &>(subpkt);
        return json_add(obj, "amount", (int) sub.amount()) &&
               json_add(obj, "level", (int) sub.level());
    }
    case pkt::sigsub::Type::RegExp: {
        auto &sub = dynamic_cast<const pkt::sigsub::RegExp &>(subpkt);
        return json_add(obj, "regexp", sub.regexp());
    }
    case pkt::sigsub::Type::Revocable: {
        auto &sub = dynamic_cast<const pkt::sigsub::Revocable &>(subpkt);
        return json_add(obj, "revocable", sub.revocable());
    }
    case pkt::sigsub::Type::KeyExpirationTime: {
        auto &sub = dynamic_cast<const pkt::sigsub::KeyExpirationTime &>(subpkt);
        return json_add(obj, "key expiration", (uint64_t) sub.time());
    }
    case pkt::sigsub::Type::PreferredSymmetric: {
        auto &sub = dynamic_cast<const pkt::sigsub::PreferredSymmetric &>(subpkt);
        return subpacket_obj_add_algs(obj, "algorithms", sub.algs(), symm_alg_map);
    }
    case pkt::sigsub::Type::PreferredHash: {
        auto &sub = dynamic_cast<const pkt::sigsub::PreferredHash &>(subpkt);
        return subpacket_obj_add_algs(obj, "algorithms", sub.algs(), hash_alg_map);
    }
    case pkt::sigsub::Type::PreferredCompress: {
        auto &sub = dynamic_cast<const pkt::sigsub::PreferredCompress &>(subpkt);
        return subpacket_obj_add_algs(obj, "algorithms", sub.algs(), z_alg_map);
    }
    case pkt::sigsub::Type::PreferredAEAD: {
        auto &sub = dynamic_cast<const pkt::sigsub::PreferredAEAD &>(subpkt);
        return subpacket_obj_add_algs(obj, "algorithms", sub.algs(), aead_alg_map);
    }
    case pkt::sigsub::Type::RevocationKey: {
        auto &sub = dynamic_cast<const pkt::sigsub::RevocationKey &>(subpkt);
        return json_add(obj, "class", (int) sub.rev_class()) &&
               json_add(obj, "algorithm", (int) sub.alg()) &&
               json_add(obj, "fingerprint", sub.fp());
    }
    case pkt::sigsub::Type::IssuerKeyID: {
        auto &sub = dynamic_cast<const pkt::sigsub::IssuerKeyID &>(subpkt);
        return json_add(obj, "issuer keyid", sub.keyid());
    }
    case pkt::sigsub::Type::KeyserverPrefs: {
        auto &sub = dynamic_cast<const pkt::sigsub::KeyserverPrefs &>(subpkt);
        return json_add(obj, "no-modify", sub.no_modify());
    }
    case pkt::sigsub::Type::PreferredKeyserver: {
        auto &sub = dynamic_cast<const pkt::sigsub::PreferredKeyserver &>(subpkt);
        return json_add(obj, "uri", sub.keyserver());
    }
    case pkt::sigsub::Type::PrimaryUserID: {
        auto &sub = dynamic_cast<const pkt::sigsub::PrimaryUserID &>(subpkt);
        return json_add(obj, "primary", sub.primary());
    }
    case pkt::sigsub::Type::PolicyURI: {
        auto &sub = dynamic_cast<const pkt::sigsub::PolicyURI &>(subpkt);
        return json_add(obj, "uri", sub.URI());
    }
    case pkt::sigsub::Type::KeyFlags: {
        auto &  sub = dynamic_cast<const pkt::sigsub::KeyFlags &>(subpkt);
        uint8_t flg = sub.flags();
        if (!json_add(obj, "flags", (int) flg)) {
            return false; // LCOV_EXCL_LINE
        }
        json_object *jso_flg = json_object_new_array();
        if (!jso_flg || !json_add(obj, "flags.str", jso_flg)) {
            return false; // LCOV_EXCL_LINE
        }
        if ((flg & PGP_KF_CERTIFY) && !json_array_add(jso_flg, "certify")) {
            return false; // LCOV_EXCL_LINE
        }
        if ((flg & PGP_KF_SIGN) && !json_array_add(jso_flg, "sign")) {
            return false; // LCOV_EXCL_LINE
        }
        if ((flg & PGP_KF_ENCRYPT_COMMS) && !json_array_add(jso_flg, "encrypt_comm")) {
            return false; // LCOV_EXCL_LINE
        }
        if ((flg & PGP_KF_ENCRYPT_STORAGE) && !json_array_add(jso_flg, "encrypt_storage")) {
            return false; // LCOV_EXCL_LINE
        }
        if ((flg & PGP_KF_SPLIT) && !json_array_add(jso_flg, "split")) {
            return false; // LCOV_EXCL_LINE
        }
        if ((flg & PGP_KF_AUTH) && !json_array_add(jso_flg, "auth")) {
            return false; // LCOV_EXCL_LINE
        }
        if ((flg & PGP_KF_SHARED) && !json_array_add(jso_flg, "shared")) {
            return false; // LCOV_EXCL_LINE
        }
        return true;
    }
    case pkt::sigsub::Type::SignersUserID: {
        auto &sub = dynamic_cast<const pkt::sigsub::SignersUserID &>(subpkt);
        return json_add(obj, "uid", sub.signer());
    }
    case pkt::sigsub::Type::RevocationReason: {
        auto &sub = dynamic_cast<const pkt::sigsub::RevocationReason &>(subpkt);
        if (!obj_add_intstr_json(obj, "code", sub.code(), revoc_reason_map)) {
            return false;
        }
        return json_add(obj, "message", sub.reason());
    }
    case pkt::sigsub::Type::Features: {
        auto &sub = dynamic_cast<const pkt::sigsub::Features &>(subpkt);
        return json_add(obj, "mdc", (bool) (sub.features() & PGP_KEY_FEATURE_MDC)) &&
               json_add(obj, "aead", (bool) (sub.features() & PGP_KEY_FEATURE_AEAD)) &&
               json_add(obj, "v5 keys", (bool) (sub.features() & PGP_KEY_FEATURE_V5));
    }
    case pkt::sigsub::Type::EmbeddedSignature: {
        auto &       sub = dynamic_cast<const pkt::sigsub::EmbeddedSignature &>(subpkt);
        json_object *sig = json_object_new_object();
        if (!sub.signature() || !sig || !json_add(obj, "signature", sig)) {
            return false; // LCOV_EXCL_LINE
        }
        return !dump_signature_pkt(*sub.signature(), sig);
    }
    case pkt::sigsub::Type::IssuerFingerprint: {
        auto &sub = dynamic_cast<const pkt::sigsub::IssuerFingerprint &>(subpkt);
        return json_add(obj, "fingerprint", sub.fp());
    }
    case pkt::sigsub::Type::NotationData: {
        auto &sub = dynamic_cast<const pkt::sigsub::NotationData &>(subpkt);
        if (!json_add(obj, "human", sub.human_readable()) ||
            !json_add(obj, "name", sub.name())) {
            return false; // LCOV_EXCL_LINE
        }
        if (sub.human_readable()) {
            return json_add(obj, "value", (char *) sub.value().data(), sub.value().size());
        }
        return json_add_hex(obj, "value", sub.value());
    }
    default:
        if (!dump_packets) {
            return json_add_hex(obj, "raw", subpkt.data());
        }
        return true;
    }
    return true;
}

json_object *
DumpContextJson::dump_signature_subpackets(const pkt::Signature &sig)
{
    json_object *res = json_object_new_array();
    if (!res) {
        return NULL; // LCOV_EXCL_LINE
    }
    JSONObject reswrap(res);

    for (auto &subpkt : sig.subpkts) {
        json_object *jso_subpkt = json_object_new_object();
        if (json_object_array_add(res, jso_subpkt)) {
            json_object_put(jso_subpkt);
            return NULL; // LCOV_EXCL_LINE
        }

        if (!obj_add_intstr_json(
              jso_subpkt, "type", subpkt->raw_type(), sig_subpkt_type_map)) {
            return NULL; // LCOV_EXCL_LINE
        }
        if (!json_add(jso_subpkt, "length", (int) subpkt->data().size())) {
            return NULL; // LCOV_EXCL_LINE
        }
        if (!json_add(jso_subpkt, "hashed", subpkt->hashed())) {
            return NULL; // LCOV_EXCL_LINE
        }
        if (!json_add(jso_subpkt, "critical", subpkt->critical())) {
            return NULL; // LCOV_EXCL_LINE
        }
        if (dump_packets && !json_add_hex(jso_subpkt, "raw", subpkt->data())) {
            return NULL; // LCOV_EXCL_LINE
        }
        if (!dump_signature_subpacket(*subpkt, jso_subpkt)) {
            return NULL;
        }
    }
    return reswrap.release();
}

rnp_result_t
DumpContextJson::dump_signature_pkt(const pkt::Signature &sig, json_object *pkt)
{
    json_object *material = NULL;

    if (!json_add(pkt, "version", (int) sig.version)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    if (!obj_add_intstr_json(pkt, "type", sig.type(), sig_type_map)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }

    if (sig.version < PGP_V4) {
        if (!json_add(pkt, "creation time", (uint64_t) sig.creation_time)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        if (!json_add(pkt, "signer", sig.signer)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
    }
    if (!obj_add_intstr_json(pkt, "algorithm", sig.palg, pubkey_alg_map)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    if (!obj_add_intstr_json(pkt, "hash algorithm", sig.halg, hash_alg_map)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }

    if (sig.version >= PGP_V4) {
        json_object *subpkts = dump_signature_subpackets(sig);
        if (!subpkts || !json_add(pkt, "subpackets", subpkts)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
    }

    if (!json_add_hex(pkt, "lbits", sig.lbits.data(), sig.lbits.size())) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }

    material = json_object_new_object();
    if (!material || !json_add(pkt, "material", material)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }

    auto sigmaterial = sig.parse_material();
    if (!sigmaterial) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    switch (sig.palg) {
    case PGP_PKA_RSA:
    case PGP_PKA_RSA_ENCRYPT_ONLY:
    case PGP_PKA_RSA_SIGN_ONLY: {
        auto &rsa = dynamic_cast<const RSASigMaterial &>(*sigmaterial);
        if (!obj_add_mpi_json(material, "s", rsa.sig.s, dump_mpi)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        break;
    }
    case PGP_PKA_DSA: {
        auto &dsa = dynamic_cast<const DSASigMaterial &>(*sigmaterial);
        if (!obj_add_mpi_json(material, "r", dsa.sig.r, dump_mpi) ||
            !obj_add_mpi_json(material, "s", dsa.sig.s, dump_mpi)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        break;
    }
    case PGP_PKA_EDDSA:
    case PGP_PKA_ECDSA:
    case PGP_PKA_SM2:
    case PGP_PKA_ECDH: {
        auto &ec = dynamic_cast<const ECSigMaterial &>(*sigmaterial);
        if (!obj_add_mpi_json(material, "r", ec.sig.r, dump_mpi) ||
            !obj_add_mpi_json(material, "s", ec.sig.s, dump_mpi)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        break;
    }
    /* Wasn't able to find ElGamal sig artifacts so let's ignore this for coverage */
    /* LCOV_EXCL_START */
    case PGP_PKA_ELGAMAL:
    case PGP_PKA_ELGAMAL_ENCRYPT_OR_SIGN: {
        auto &eg = dynamic_cast<const EGSigMaterial &>(*sigmaterial);
        if (!obj_add_mpi_json(material, "r", eg.sig.r, dump_mpi) ||
            !obj_add_mpi_json(material, "s", eg.sig.s, dump_mpi)) {
            return RNP_ERROR_OUT_OF_MEMORY;
        }
        break;
    }
    /* LCOV_EXCL_END */
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_PKA_ED25519:
        /* TODO */
        break;
#endif
#if defined(ENABLE_PQC)
    case PGP_PKA_DILITHIUM3_ED25519:
        FALLTHROUGH_STATEMENT;
    // TODO: Add case for PGP_PKA_DILITHIUM5_ED448 with FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_P384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_BP256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_BP384:
        /* TODO */
        break;
    case PGP_PKA_SPHINCSPLUS_SHA2:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_SPHINCSPLUS_SHAKE:
        /* TODO */
        break;
#endif
    default:
        break;
    }
    return RNP_SUCCESS;
}

rnp_result_t
DumpContextJson::dump_signature(json_object *pkt)
{
    pkt::Signature sig;
    auto           ret = sig.parse(src);
    if (ret) {
        return ret;
    }
    return dump_signature_pkt(sig, pkt);
}

bool
DumpContextJson::dump_key_material(const KeyMaterial *material, json_object *jso)
{
    if (!material) {
        return false; // LCOV_EXCL_LINE
    }
    switch (material->alg()) {
    case PGP_PKA_RSA:
    case PGP_PKA_RSA_ENCRYPT_ONLY:
    case PGP_PKA_RSA_SIGN_ONLY: {
        auto &rsa = dynamic_cast<const RSAKeyMaterial &>(*material);
        if (!obj_add_mpi_json(jso, "n", rsa.n(), dump_mpi) ||
            !obj_add_mpi_json(jso, "e", rsa.e(), dump_mpi)) {
            return false; // LCOV_EXCL_LINE
        }
        return true;
    }
    case PGP_PKA_DSA: {
        auto &dsa = dynamic_cast<const DSAKeyMaterial &>(*material);
        if (!obj_add_mpi_json(jso, "p", dsa.p(), dump_mpi) ||
            !obj_add_mpi_json(jso, "q", dsa.q(), dump_mpi) ||
            !obj_add_mpi_json(jso, "g", dsa.g(), dump_mpi) ||
            !obj_add_mpi_json(jso, "y", dsa.y(), dump_mpi)) {
            return false; // LCOV_EXCL_LINE
        }
        return true;
    }
    case PGP_PKA_ELGAMAL:
    case PGP_PKA_ELGAMAL_ENCRYPT_OR_SIGN: {
        auto &eg = dynamic_cast<const EGKeyMaterial &>(*material);
        if (!obj_add_mpi_json(jso, "p", eg.p(), dump_mpi) ||
            !obj_add_mpi_json(jso, "g", eg.g(), dump_mpi) ||
            !obj_add_mpi_json(jso, "y", eg.y(), dump_mpi)) {
            return false; // LCOV_EXCL_LINE
        }
        return true;
    }
    case PGP_PKA_ECDSA:
    case PGP_PKA_EDDSA:
    case PGP_PKA_SM2:
    case PGP_PKA_ECDH: {
        auto &ec = dynamic_cast<const ECKeyMaterial &>(*material);
        auto  cdesc = ec::Curve::get(ec.curve());
        /* Common EC fields */
        if (!obj_add_mpi_json(jso, "p", ec.p(), dump_mpi)) {
            return false; // LCOV_EXCL_LINE
        }
        if (!json_add(jso, "curve", cdesc ? cdesc->pgp_name : "unknown")) {
            return false; // LCOV_EXCL_LINE
        }
        if (material->alg() != PGP_PKA_ECDH) {
            return true;
        }
        /* ECDH-only fields */
        auto &ecdh = dynamic_cast<const ECDHKeyMaterial &>(*material);
        if (!obj_add_intstr_json(jso, "hash algorithm", ecdh.kdf_hash_alg(), hash_alg_map)) {
            return false; // LCOV_EXCL_LINE
        }
        if (!obj_add_intstr_json(
              jso, "key wrap algorithm", ecdh.key_wrap_alg(), symm_alg_map)) {
            return false; // LCOV_EXCL_LINE
        }
        return true;
    }
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_PKA_ED25519:
    case PGP_PKA_X25519:
        /* TODO */
        return true;
#endif
#if defined(ENABLE_PQC)
    case PGP_PKA_KYBER768_X25519:
        FALLTHROUGH_STATEMENT;
    // TODO: Add case for PGP_PKA_KYBER1024_X448 with FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER768_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER1024_P384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER768_BP256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER1024_BP384:
        // TODO
        return true;
    case PGP_PKA_DILITHIUM3_ED25519:
        FALLTHROUGH_STATEMENT;
    // TODO: Add case for PGP_PKA_DILITHIUM5_ED448 with FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_P384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_BP256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_BP384:
        /* TODO */
        return true;
    case PGP_PKA_SPHINCSPLUS_SHA2:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_SPHINCSPLUS_SHAKE:
        /* TODO */
        return true;
#endif
    default:
        return false;
    }
}

rnp_result_t
DumpContextJson::dump_key(json_object *pkt)
{
    pgp_key_pkt_t key;
    auto          ret = key.parse(src);
    if (ret) {
        return ret;
    }

    if (!json_add(pkt, "version", (int) key.version)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    if (!json_add(pkt, "creation time", (uint64_t) key.creation_time)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    if ((key.version < PGP_V4) && !json_add(pkt, "v3 days", (int) key.v3_days)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    if (!obj_add_intstr_json(pkt, "algorithm", key.alg, pubkey_alg_map)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    if ((key.version == PGP_V5) &&
        !json_add(pkt, "v5 public key material length", (int) key.v5_pub_len)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }

    auto material = json_object_new_object();
    if (!material || !json_add(pkt, "material", material)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    if (!dump_key_material(key.material.get(), material)) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    if (is_secret_key_pkt(key.tag)) {
        if (!json_add(material, "s2k usage", (int) key.sec_protection.s2k.usage)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        if ((key.version == PGP_V5) &&
            !json_add(material, "v5 s2k length", (int) key.v5_s2k_len)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        if (!obj_add_s2k_json(material, &key.sec_protection.s2k)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        if (key.sec_protection.s2k.usage &&
            !obj_add_intstr_json(
              material, "symmetric algorithm", key.sec_protection.symm_alg, symm_alg_map)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        if ((key.version == PGP_V5) &&
            !json_add(material, "v5 secret key data length", (int) key.v5_sec_len)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
    }

    Fingerprint fp(key);
    if (!json_add(pkt, "keyid", fp.keyid())) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }

    if (dump_grips && !json_add(pkt, "fingerprint", fp)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }

    if (dump_grips) {
        if (key.material) {
            KeyGrip grip = key.material->grip();
            if (!json_add_hex(pkt, "grip", grip.data(), grip.size())) {
                return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
            }
        } else {
            return RNP_ERROR_BAD_PARAMETERS; // LCOV_EXCL_LINE
        }
    }
    return RNP_SUCCESS;
}

rnp_result_t
DumpContextJson::dump_user_id(json_object *pkt)
{
    pgp_userid_pkt_t uid;
    auto             ret = uid.parse(src);
    if (ret) {
        return ret;
    }

    switch (uid.tag) {
    case PGP_PKT_USER_ID:
        if (!json_add(pkt, "userid", (char *) uid.uid.data(), uid.uid.size())) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        break;
    case PGP_PKT_USER_ATTR:
        if (!json_add_hex(pkt, "userattr", uid.uid.data(), uid.uid.size())) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        break;
    default:;
    }
    return RNP_SUCCESS;
}

rnp_result_t
DumpContextJson::dump_pk_session_key(json_object *pkt)
{
    pgp_pk_sesskey_t pkey;
    auto             ret = pkey.parse(src);
    if (ret) {
        return ret;
    }
    auto pkmaterial = pkey.parse_material();
    if (!pkmaterial) {
        return RNP_ERROR_BAD_FORMAT;
    }

    if (!json_add(pkt, "version", (int) pkey.version) ||
        !json_add(pkt, "keyid", pkey.key_id) ||
        !obj_add_intstr_json(pkt, "algorithm", pkey.alg, pubkey_alg_map)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }

    json_object *material = json_object_new_object();
    if (!json_add(pkt, "material", material)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }

    switch (pkey.alg) {
    case PGP_PKA_RSA:
    case PGP_PKA_RSA_ENCRYPT_ONLY:
    case PGP_PKA_RSA_SIGN_ONLY: {
        auto &rsa = dynamic_cast<const RSAEncMaterial &>(*pkmaterial).enc;
        if (!obj_add_mpi_json(material, "m", rsa.m, dump_mpi)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        break;
    }
    case PGP_PKA_ELGAMAL:
    case PGP_PKA_ELGAMAL_ENCRYPT_OR_SIGN: {
        auto &eg = dynamic_cast<const EGEncMaterial &>(*pkmaterial).enc;
        if (!obj_add_mpi_json(material, "g", eg.g, dump_mpi) ||
            !obj_add_mpi_json(material, "m", eg.m, dump_mpi)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        break;
    }
    case PGP_PKA_SM2: {
        auto &sm2 = dynamic_cast<const SM2EncMaterial &>(*pkmaterial).enc;
        if (!obj_add_mpi_json(material, "m", sm2.m, dump_mpi)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        break;
    }
    case PGP_PKA_ECDH: {
        auto &ecdh = dynamic_cast<const ECDHEncMaterial &>(*pkmaterial).enc;
        if (!obj_add_mpi_json(material, "p", ecdh.p, dump_mpi) ||
            !json_add(material, "m.bytes", (int) ecdh.m.size())) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        if (dump_mpi && !json_add_hex(material, "m", ecdh.m.data(), ecdh.m.size())) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        break;
    }
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_PKA_ED25519:
    case PGP_PKA_X25519:
        /* TODO */
        break;
#endif
#if defined(ENABLE_PQC)
    case PGP_PKA_KYBER768_X25519:
        FALLTHROUGH_STATEMENT;
    // TODO: Add case for PGP_PKA_KYBER1024_X448 with FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER768_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER1024_P384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER768_BP256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER1024_BP384:
        // TODO
        break;
#endif
    default:;
    }

    return RNP_SUCCESS;
}

rnp_result_t
DumpContextJson::dump_sk_session_key(json_object *pkt)
{
    pgp_sk_sesskey_t skey;
    auto             ret = skey.parse(src);
    if (ret) {
        return ret;
    }

    if (!json_add(pkt, "version", (int) skey.version) ||
        !obj_add_intstr_json(pkt, "algorithm", skey.alg, symm_alg_map)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    if ((skey.version == PGP_SKSK_V5) &&
        !obj_add_intstr_json(pkt, "aead algorithm", skey.aalg, aead_alg_map)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    if (!obj_add_s2k_json(pkt, &skey.s2k)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    if ((skey.version == PGP_SKSK_V5) && !json_add_hex(pkt, "aead iv", skey.iv, skey.ivlen)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    if (!json_add_hex(pkt, "encrypted key", skey.enckey, skey.enckeylen)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    return RNP_SUCCESS;
}

rnp_result_t
DumpContextJson::dump_encrypted(json_object *pkt, pgp_pkt_type_t tag)
{
    if (tag != PGP_PKT_AEAD_ENCRYPTED) {
        /* packet header with tag is already in pkt */
        return stream_skip_packet(&src);
    }

    /* dumping AEAD data */
    pgp_aead_hdr_t aead{};
    if (!get_aead_hdr(aead)) {
        return RNP_ERROR_READ;
    }

    if (!json_add(pkt, "version", (int) aead.version) ||
        !obj_add_intstr_json(pkt, "algorithm", aead.ealg, symm_alg_map) ||
        !obj_add_intstr_json(pkt, "aead algorithm", aead.aalg, aead_alg_map) ||
        !json_add(pkt, "chunk size", (int) aead.csize) ||
        !json_add_hex(pkt, "aead iv", aead.iv, aead.ivlen)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    return RNP_SUCCESS;
}

rnp_result_t
DumpContextJson::dump_one_pass(json_object *pkt)
{
    pgp_one_pass_sig_t onepass;
    auto               ret = onepass.parse(src);
    if (ret) {
        return ret;
    }

    if (!json_add(pkt, "version", (int) onepass.version)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    if (!obj_add_intstr_json(pkt, "type", onepass.type, sig_type_map)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    if (!obj_add_intstr_json(pkt, "hash algorithm", onepass.halg, hash_alg_map)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    if (!obj_add_intstr_json(pkt, "public key algorithm", onepass.palg, pubkey_alg_map)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    if (!json_add(pkt, "signer", onepass.keyid)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    if (!json_add(pkt, "nested", (bool) onepass.nested)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    return RNP_SUCCESS;
}

rnp_result_t
DumpContextJson::dump_marker(json_object *pkt)
{
    auto ret = stream_parse_marker(src);
    if (!json_add(pkt, "contents", ret ? "invalid" : PGP_MARKER_CONTENTS)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    return ret;
}

rnp_result_t
DumpContextJson::dump_compressed(json_object *pkt)
{
    std::unique_ptr<Source> zsrc(new Source());
    auto                    ret = init_compressed_src(&zsrc->src(), &src);
    if (ret) {
        return ret;
    }

    uint8_t zalg;
    get_compressed_src_alg(&zsrc->src(), &zalg);
    if (!obj_add_intstr_json(pkt, "algorithm", zalg, z_alg_map)) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }

    json_object *   contents = NULL;
    DumpContextJson ctx(zsrc->src(), &contents);
    ctx.copy_params(*this);
    ret = ctx.dump(true);
    copy_params(ctx);
    if (!ret && !json_add(pkt, "contents", contents)) {
        json_object_put(contents);
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    return ret;
}

rnp_result_t
DumpContextJson::dump_literal(json_object *pkt)
{
    Source lsrc;
    auto   ret = init_literal_src(&lsrc.src(), &src);
    if (ret) {
        return ret;
    }

    ret = RNP_ERROR_OUT_OF_MEMORY;
    auto &lhdr = get_literal_src_hdr(lsrc.src());
    if (!json_add(pkt, "format", (char *) &lhdr.format, 1) ||
        !json_add(pkt, "filename", (char *) lhdr.fname, lhdr.fname_len) ||
        !json_add(pkt, "timestamp", (uint64_t) lhdr.timestamp)) {
        return ret; // LCOV_EXCL_LINE
    }

    while (!lsrc.eof()) {
        uint8_t readbuf[16384];
        size_t  read = 0;
        if (!lsrc.src().read(readbuf, sizeof(readbuf), &read)) {
            return RNP_ERROR_READ;
        }
    }

    if (!json_add(pkt, "datalen", (uint64_t) lsrc.readb())) {
        return ret; // LCOV_EXCL_LINE
    }
    return RNP_SUCCESS;
}

bool
DumpContextJson::dump_pkt_hdr(pgp_packet_hdr_t &hdr, json_object *pkt)
{
    auto hdrret = stream_peek_packet_hdr(&src, &hdr);
    if (hdrret) {
        return false;
    }

    json_object *jso_hdr = json_object_new_object();
    if (!jso_hdr) {
        return false;
    }
    rnp::JSONObject jso_hdrwrap(jso_hdr);

    if (!json_add(jso_hdr, "offset", (uint64_t) src.readb) ||
        !obj_add_intstr_json(jso_hdr, "tag", hdr.tag, packet_tag_map) ||
        !json_add_hex(jso_hdr, "raw", hdr.hdr, hdr.hdr_len)) {
        return false; // LCOV_EXCL_LINE
    }
    if (!hdr.partial && !hdr.indeterminate &&
        !json_add(jso_hdr, "length", (uint64_t) hdr.pkt_len)) {
        return false; // LCOV_EXCL_LINE
    }
    if (!json_add(jso_hdr, "partial", hdr.partial) ||
        !json_add(jso_hdr, "indeterminate", hdr.indeterminate) ||
        !json_add(pkt, "header", jso_hdr)) {
        return false; // LCOV_EXCL_LINE
    }
    jso_hdrwrap.release();
    return true;
}

rnp_result_t
DumpContextJson::dump_raw_packets()
{
    rnp_result_t ret = RNP_ERROR_GENERIC;

    json_object *pkts = json_object_new_array();
    if (!pkts) {
        return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
    }
    JSONObject pktswrap(pkts);

    if (src.eof()) {
        *json = pktswrap.release();
        return RNP_SUCCESS;
    }

    /* do not allow endless recursion */
    if (++layers > MAXIMUM_NESTING_LEVEL) {
        RNP_LOG("Too many OpenPGP nested layers during the dump.");
        *json = pktswrap.release();
        return RNP_SUCCESS;
    }

    while (!src.eof()) {
        json_object *pkt = json_object_new_object();
        if (!pkt) {
            return RNP_ERROR_OUT_OF_MEMORY;
        }
        JSONObject       pktwrap(pkt);
        pgp_packet_hdr_t hdr{};
        if (!dump_pkt_hdr(hdr, pkt)) {
            return RNP_ERROR_OUT_OF_MEMORY;
        }

        if (dump_packets) {
            size_t  rlen = hdr.pkt_len + hdr.hdr_len;
            uint8_t buf[2048 + sizeof(hdr.hdr)] = {0};

            if (!hdr.pkt_len || (rlen > 2048 + hdr.hdr_len)) {
                rlen = 2048 + hdr.hdr_len;
            }
            if (!src.peek(buf, rlen, &rlen) || (rlen < hdr.hdr_len)) {
                return RNP_ERROR_READ;
            }
            if (!json_add_hex(pkt, "raw", buf + hdr.hdr_len, rlen - hdr.hdr_len)) {
                return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
            }
        }

        switch (hdr.tag) {
        case PGP_PKT_SIGNATURE:
            ret = dump_signature(pkt);
            break;
        case PGP_PKT_SECRET_KEY:
        case PGP_PKT_PUBLIC_KEY:
        case PGP_PKT_SECRET_SUBKEY:
        case PGP_PKT_PUBLIC_SUBKEY:
            ret = dump_key(pkt);
            break;
        case PGP_PKT_USER_ID:
        case PGP_PKT_USER_ATTR:
            ret = dump_user_id(pkt);
            break;
        case PGP_PKT_PK_SESSION_KEY:
            ret = dump_pk_session_key(pkt);
            break;
        case PGP_PKT_SK_SESSION_KEY:
            ret = dump_sk_session_key(pkt);
            break;
        case PGP_PKT_SE_DATA:
        case PGP_PKT_SE_IP_DATA:
        case PGP_PKT_AEAD_ENCRYPTED:
            stream_pkts++;
            ret = dump_encrypted(pkt, hdr.tag);
            break;
        case PGP_PKT_ONE_PASS_SIG:
            ret = dump_one_pass(pkt);
            break;
        case PGP_PKT_COMPRESSED:
            stream_pkts++;
            ret = dump_compressed(pkt);
            break;
        case PGP_PKT_LITDATA:
            stream_pkts++;
            ret = dump_literal(pkt);
            break;
        case PGP_PKT_MARKER:
            ret = dump_marker(pkt);
            break;
        case PGP_PKT_TRUST:
        case PGP_PKT_MDC:
            ret = stream_skip_packet(&src);
            break;
        default:
            ret = stream_skip_packet(&src);
            if (ret) {
                return ret;
            }
            if (++failures > MAXIMUM_ERROR_PKTS) {
                RNP_LOG("too many packet dump errors or unknown packets.");
                return RNP_ERROR_BAD_FORMAT;
            }
        }

        if (ret) {
            RNP_LOG("failed to process packet");
            if (++failures > MAXIMUM_ERROR_PKTS) {
                RNP_LOG("too many packet dump errors.");
                return ret;
            }
        }

        if (json_object_array_add(pkts, pkt)) {
            return RNP_ERROR_OUT_OF_MEMORY; // LCOV_EXCL_LINE
        }
        pktwrap.release();
        if (stream_pkts > MAXIMUM_STREAM_PKTS) {
            RNP_LOG("Too many OpenPGP stream packets during the dump.");
            break;
        }
    }

    *json = pktswrap.release();
    return RNP_SUCCESS;
}

rnp_result_t
DumpContextJson::dump(bool raw_only)
{
    /* check whether source is cleartext - then skip till the signature */
    if (!raw_only && src.is_cleartext()) {
        if (!skip_cleartext()) {
            RNP_LOG("malformed cleartext signed data");
            return RNP_ERROR_BAD_FORMAT;
        }
    }
    /* check whether source is armored */
    if (!raw_only && src.is_armored()) {
        std::unique_ptr<Source> armor(new Source());
        rnp_result_t            ret = init_armored_src(&armor->src(), &src);
        if (ret) {
            RNP_LOG("failed to parse armored data");
            return ret;
        }
        DumpContextJson ctx(armor->src(), json);
        ctx.copy_params(*this);
        return ctx.dump(true);
    }

    if (src.eof()) {
        return RNP_ERROR_NOT_ENOUGH_DATA;
    }
    return dump_raw_packets();
}
} // namespace rnp
