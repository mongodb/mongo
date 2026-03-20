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

#include "rnp_tests.h"
#include "support.h"
#include "rnp.h"
#include "crypto/hash.hpp"
#include "crypto/signatures.h"
#include "key.hpp"
#include <time.h>
#include "rnp.h"
#include <librepgp/stream-ctx.h>
#include <librepgp/stream-packet.h>
#include <librepgp/stream-sig.h>
#include <librepgp/stream-key.h>
#include <librepgp/stream-dump.h>
#include <librepgp/stream-armor.h>
#include <librepgp/stream-write.h>
#include <algorithm>
#include "time-utils.h"

static bool
stream_hash_file(rnp::Hash &hash, const char *path)
{
    pgp_source_t src;
    if (init_file_src(&src, path)) {
        return false;
    }

    bool res = false;
    do {
        uint8_t readbuf[1024];
        size_t  read = 0;
        if (!src.read(readbuf, sizeof(readbuf), &read)) {
            goto finish;
        } else if (read == 0) {
            break;
        }
        hash.add(readbuf, read);
    } while (1);

    res = true;
finish:
    src.close();
    return res;
}

TEST_F(rnp_tests, test_stream_memory)
{
    const char *data = "Sample data to test memory streams";
    size_t      datalen;
    pgp_dest_t  memdst;
    void *      mown;
    void *      mcpy;

    datalen = strlen(data) + 1;

    /* populate memory dst and own inner data */
    assert_rnp_success(init_mem_dest(&memdst, NULL, 0));
    assert_rnp_success(memdst.werr);
    dst_write(&memdst, data, datalen);
    assert_rnp_success(memdst.werr);
    assert_int_equal(memdst.writeb, datalen);

    assert_non_null(mcpy = mem_dest_get_memory(&memdst));
    assert_false(memcmp(mcpy, data, datalen));
    assert_non_null(mown = mem_dest_own_memory(&memdst));
    assert_false(memcmp(mown, data, datalen));
    dst_close(&memdst, true);
    /* make sure we own data after close */
    assert_false(memcmp(mown, data, datalen));
    free(mown);
    /* make sure init_mem_src fails with NULL parameter */
    pgp_source_t memsrc;
    assert_rnp_failure(init_mem_src(&memsrc, NULL, 12, false));
    assert_rnp_failure(init_mem_src(&memsrc, NULL, 12, true));
}

TEST_F(rnp_tests, test_stream_memory_discard)
{
    pgp_dest_t   memdst = {};
    char         mem[32];
    const char * hexes = "123456789ABCDEF";
    const size_t hexes_len = 15;

    /* init mem dst and write some data */
    assert_rnp_success(init_mem_dest(&memdst, mem, sizeof(mem)));
    dst_write(&memdst, "Hello", 5);
    assert_int_equal(memdst.writeb, 5);
    dst_write(&memdst, ", ", 2);
    assert_int_equal(memdst.writeb, 7);
    dst_write(&memdst, "world!\n", 7);
    assert_int_equal(memdst.writeb, 14);
    /* Now discard overflowing data and attempt to overflow it */
    mem_dest_discard_overflow(&memdst, true);
    dst_write(&memdst, "Hello, world!\n", 14);
    assert_int_equal(memdst.writeb, 28);
    assert_int_equal(memdst.werr, RNP_SUCCESS);
    dst_write(&memdst, hexes, hexes_len);
    /* While extra data is discarded, writeb is still incremented by hexes_len */
    assert_int_equal(memdst.writeb, 43);
    assert_int_equal(memdst.werr, RNP_SUCCESS);
    assert_int_equal(memcmp(mem, "Hello, world!\nHello, world!\n1234", 32), 0);
    dst_write(&memdst, hexes, hexes_len);
    assert_int_equal(memdst.writeb, 58);
    assert_int_equal(memdst.werr, RNP_SUCCESS);
    assert_int_equal(memcmp(mem, "Hello, world!\nHello, world!\n1234", 32), 0);
    /* Now make sure that error is generated */
    mem_dest_discard_overflow(&memdst, false);
    dst_write(&memdst, hexes, hexes_len);
    assert_int_equal(memdst.writeb, 58);
    assert_int_not_equal(memdst.werr, RNP_SUCCESS);

    dst_close(&memdst, true);
    assert_int_equal(memcmp(mem, "Hello, world!\nHello, world!\n1234", 32), 0);

    /* Now do tests with dynamic memory allocation */
    const size_t bytes = 12345;
    assert_rnp_success(init_mem_dest(&memdst, NULL, bytes));
    for (size_t i = 0; i < bytes / hexes_len; i++) {
        dst_write(&memdst, hexes, hexes_len);
        assert_int_equal(memdst.writeb, (i + 1) * hexes_len);
        assert_int_equal(memdst.werr, RNP_SUCCESS);
    }

    mem_dest_discard_overflow(&memdst, true);
    dst_write(&memdst, hexes, hexes_len);
    assert_int_equal(memdst.writeb, bytes - bytes % hexes_len + hexes_len);
    assert_int_equal(memdst.werr, RNP_SUCCESS);
    mem_dest_discard_overflow(&memdst, false);
    dst_write(&memdst, hexes, hexes_len);
    assert_int_equal(memdst.writeb, bytes - bytes % hexes_len + hexes_len);
    assert_int_not_equal(memdst.werr, RNP_SUCCESS);
    dst_write(&memdst, hexes, hexes_len);
    assert_int_equal(memdst.writeb, bytes - bytes % hexes_len + hexes_len);
    assert_int_not_equal(memdst.werr, RNP_SUCCESS);
    dst_close(&memdst, true);
}

static void
copy_tmp_path(char *buf, size_t buflen, pgp_dest_t *dst)
{
    typedef struct pgp_dest_file_param_t {
        int         fd;
        int         errcode;
        bool        overwrite;
        std::string path;
    } pgp_dest_file_param_t;

    pgp_dest_file_param_t *param = (pgp_dest_file_param_t *) dst->param;
    strncpy(buf, param->path.c_str(), buflen - 1);
}

TEST_F(rnp_tests, test_stream_file)
{
    const char * filename = "dummyfile.dat";
    const char * dirname = "dummydir";
    const char * file2name = "dummydir/dummyfile.dat";
    const char * filedata = "dummy message to be stored in the file";
    const int    iterations = 10000;
    const int    filedatalen = strlen(filedata);
    char         tmpname[128] = {0};
    uint8_t      tmpbuf[1024] = {0};
    pgp_dest_t   dst = {};
    pgp_source_t src = {};

    /* try to read non-existing file */
    assert_rnp_failure(init_file_src(&src, filename));
    assert_rnp_failure(init_file_src(&src, dirname));
    /* create dir */
    assert_int_equal(RNP_MKDIR(dirname, S_IRWXU), 0);
    /* attempt to read or create file in place of directory */
    assert_rnp_failure(init_file_src(&src, dirname));
    assert_rnp_failure(init_file_dest(&dst, dirname, false));
    /* with overwrite flag it must succeed, then delete it */
    assert_rnp_success(init_file_dest(&dst, dirname, true));
    assert_int_equal(file_size(dirname), 0);
    dst_close(&dst, true);
    /* create dir back */
    assert_int_equal(RNP_MKDIR(dirname, S_IRWXU), 0);

    /* write some data to the file and the discard it */
    assert_rnp_success(init_file_dest(&dst, filename, false));
    dst_write(&dst, filedata, filedatalen);
    assert_int_not_equal(file_size(filename), -1);
    dst_close(&dst, true);
    assert_int_equal(file_size(filename), -1);

    /* write some data to the file and make sure it is written */
    assert_rnp_success(init_file_dest(&dst, filename, false));
    dst_write(&dst, filedata, filedatalen);
    assert_int_not_equal(file_size(filename), -1);
    dst_close(&dst, false);
    assert_int_equal(file_size(filename), filedatalen);

    /* attempt to create file over existing without overwrite flag */
    assert_rnp_failure(init_file_dest(&dst, filename, false));
    assert_int_equal(file_size(filename), filedatalen);

    /* overwrite file - it should be truncated, then write bunch of bytes */
    assert_rnp_success(init_file_dest(&dst, filename, true));
    assert_int_equal(file_size(filename), 0);
    for (int i = 0; i < iterations; i++) {
        dst_write(&dst, filedata, filedatalen);
    }
    /* and some smaller writes */
    for (int i = 0; i < 5 * iterations; i++) {
        dst_write(&dst, "zzz", 3);
    }
    dst_close(&dst, false);
    assert_int_equal(file_size(filename), iterations * (filedatalen + 15));

    /* read file back, checking the contents */
    assert_rnp_success(init_file_src(&src, filename));
    for (int i = 0; i < iterations; i++) {
        size_t read = 0;
        assert_true(src.read(tmpbuf, filedatalen, &read));
        assert_int_equal(read, filedatalen);
        assert_int_equal(memcmp(tmpbuf, filedata, filedatalen), 0);
    }
    for (int i = 0; i < 5 * iterations; i++) {
        size_t read = 0;
        assert_true(src.read(tmpbuf, 3, &read));
        assert_int_equal(read, 3);
        assert_int_equal(memcmp(tmpbuf, "zzz", 3), 0);
    }
    src.close();

    /* overwrite and discard - file should be deleted */
    assert_rnp_success(init_file_dest(&dst, filename, true));
    assert_int_equal(file_size(filename), 0);
    for (int i = 0; i < iterations; i++) {
        dst_write(&dst, "hello", 6);
    }
    dst_close(&dst, true);
    assert_int_equal(file_size(filename), -1);

    /* create and populate file in subfolder */
    assert_rnp_success(init_file_dest(&dst, file2name, true));
    assert_int_equal(file_size(file2name), 0);
    for (int i = 0; i < iterations; i++) {
        dst_write(&dst, filedata, filedatalen);
    }
    dst_close(&dst, false);
    assert_int_equal(file_size(file2name), filedatalen * iterations);
    assert_int_equal(rnp_unlink(file2name), 0);

    /* create and populate file stream, using tmp name before closing */
    assert_rnp_success(init_tmpfile_dest(&dst, filename, false));
    copy_tmp_path(tmpname, sizeof(tmpname), &dst);
    assert_int_equal(file_size(tmpname), 0);
    assert_int_equal(file_size(filename), -1);
    for (int i = 0; i < iterations; i++) {
        dst_write(&dst, filedata, filedatalen);
    }
    dst_close(&dst, false);
    assert_int_equal(file_size(tmpname), -1);
    assert_int_equal(file_size(filename), filedatalen * iterations);

    /* create and then discard file stream, using tmp name before closing */
    assert_rnp_success(init_tmpfile_dest(&dst, filename, true));
    copy_tmp_path(tmpname, sizeof(tmpname), &dst);
    assert_int_equal(file_size(tmpname), 0);
    dst_write(&dst, filedata, filedatalen);
    /* make sure file was not overwritten */
    assert_int_equal(file_size(filename), filedatalen * iterations);
    dst_close(&dst, true);
    assert_int_equal(file_size(tmpname), -1);
    assert_int_equal(file_size(filename), filedatalen * iterations);

    /* create and then close file stream, using tmp name before closing. No overwrite. */
    assert_rnp_success(init_tmpfile_dest(&dst, filename, false));
    copy_tmp_path(tmpname, sizeof(tmpname), &dst);
    assert_int_equal(file_size(tmpname), 0);
    dst_write(&dst, filedata, filedatalen);
    /* make sure file was not overwritten */
    assert_int_equal(file_size(filename), filedatalen * iterations);
    assert_rnp_failure(dst_finish(&dst));
    dst_close(&dst, false);
    assert_int_equal(file_size(tmpname), filedatalen);
    assert_int_equal(file_size(filename), filedatalen * iterations);
    assert_int_equal(rnp_unlink(tmpname), 0);

    /* create and then close file stream, using tmp name before closing. Overwrite existing. */
    assert_rnp_success(init_tmpfile_dest(&dst, filename, true));
    copy_tmp_path(tmpname, sizeof(tmpname), &dst);
    assert_int_equal(file_size(tmpname), 0);
    dst_write(&dst, filedata, filedatalen);
    /* make sure file was not overwritten yet */
    assert_int_equal(file_size(filename), filedatalen * iterations);
    assert_rnp_success(dst_finish(&dst));
    dst_close(&dst, false);
    assert_int_equal(file_size(tmpname), -1);
    assert_int_equal(file_size(filename), filedatalen);

    /* make sure we can overwrite directory */
    assert_rnp_success(init_tmpfile_dest(&dst, dirname, true));
    copy_tmp_path(tmpname, sizeof(tmpname), &dst);
    assert_int_equal(file_size(tmpname), 0);
    dst_write(&dst, filedata, filedatalen);
    /* make sure file was not overwritten yet */
    assert_int_equal(file_size(dirname), -1);
    assert_rnp_success(dst_finish(&dst));
    dst_close(&dst, false);
    assert_int_equal(file_size(tmpname), -1);
    assert_int_equal(file_size(dirname), filedatalen);

    /* cleanup */
    assert_int_equal(rnp_unlink(dirname), 0);
}

TEST_F(rnp_tests, test_stream_signatures)
{
    pgp::pkt::Signature sig;
    pgp_source_t        sigsrc;
    rnp::Key *          key = nullptr;

    /* load keys */
    auto pubring = new rnp::KeyStore("data/test_stream_signatures/pub.asc", global_ctx);
    assert_true(pubring->load());
    /* load signature */
    assert_rnp_success(init_file_src(&sigsrc, "data/test_stream_signatures/source.txt.sig"));
    assert_rnp_success(sig.parse(sigsrc));
    sigsrc.close();
    /* hash signed file */
    pgp_hash_alg_t halg = sig.halg;
    auto           hash_orig = rnp::Hash::create(halg);
    assert_true(stream_hash_file(*hash_orig, "data/test_stream_signatures/source.txt"));
    /* hash forged file */
    auto hash_forged = rnp::Hash::create(halg);
    assert_true(
      stream_hash_file(*hash_forged, "data/test_stream_signatures/source_forged.txt"));
    /* find signing key */
    assert_non_null(key = pubring->get_signer(sig));
    /* validate signature and fields */
    auto hash = hash_orig->clone();
    assert_int_equal(sig.creation(), 1522241943);
    assert_true(signature_validate(sig, *key->material(), *hash, global_ctx).errors().empty());
    /* check forged file */
    hash = hash_forged->clone();
    assert_false(
      signature_validate(sig, *key->material(), *hash, global_ctx).errors().empty());
    /* now let's create signature and sign file */

    /* load secret key */
    auto secring = new rnp::KeyStore("data/test_stream_signatures/sec.asc", global_ctx);
    assert_true(secring->load());
    assert_non_null(key = secring->get_signer(sig));
    assert_true(key->is_secret());
    /* fill signature */
    uint32_t create = time(NULL);
    uint32_t expire = 123456;
    sig = {};
    sig.version = PGP_V4;
    sig.halg = halg;
    sig.palg = key->alg();
    sig.set_type(PGP_SIG_BINARY);
    sig.set_keyfp(key->fp());
    sig.set_keyid(key->keyid());
    sig.set_creation(create);
    sig.set_expiration(expire);
    /* make use of add_notation() to cover it */
    try {
        std::vector<uint8_t> value;
        value.resize(66000);
        sig.add_notation("dummy@example.com", value, false, true);
        assert_true(false);
    } catch (const rnp::rnp_exception &e) {
        assert_int_equal(e.code(), RNP_ERROR_BAD_PARAMETERS);
    }
    sig.add_notation("dummy@example.com", "make codecov happy!", false);
    sig.fill_hashed_data();
    /* try to sign without decrypting of the secret key */
    hash = hash_orig->clone();
    assert_throw(signature_calculate(sig, *key->material(), *hash, global_ctx));
    /* now unlock the key and sign */
    pgp_password_provider_t pswd_prov(rnp_password_provider_string, (void *) "password");
    assert_true(key->unlock(pswd_prov));
    hash = hash_orig->clone();
    signature_calculate(sig, *key->material(), *hash, global_ctx);
    /* now verify signature */
    hash = hash_orig->clone();
    /* validate signature and fields */
    assert_int_equal(sig.creation(), create);
    assert_int_equal(sig.expiration(), expire);
    assert_true(sig.has_subpkt(PGP_SIG_SUBPKT_ISSUER_FPR));
    assert_true(sig.keyfp() == key->fp());
    assert_true(signature_validate(sig, *key->material(), *hash, global_ctx).errors().empty());
    /* cleanup */
    delete pubring;
    delete secring;
}

TEST_F(rnp_tests, test_stream_signatures_revoked_key)
{
    pgp::pkt::Signature sig = {};
    pgp_source_t        sigsrc = {0};

    /* load signature */
    assert_rnp_success(
      init_file_src(&sigsrc, "data/test_stream_signatures/revoked-key-sig.gpg"));
    assert_rnp_success(sig.parse(sigsrc));
    sigsrc.close();
    /* check revocation */
    assert_int_equal(sig.revocation_code(), PGP_REVOCATION_RETIRED);
    assert_string_equal(sig.revocation_reason().c_str(), "For testing!");
}

TEST_F(rnp_tests, test_stream_key_load)
{
    pgp_source_t               keysrc = {0};
    pgp_dest_t                 keydst = {0};
    pgp_key_sequence_t         keyseq;
    pgp::Fingerprint           keyfp;
    pgp_transferable_key_t *   key = NULL;
    pgp_transferable_subkey_t *skey = NULL;

    /* public keyring, read-save-read-save armored-read */
    assert_rnp_success(init_file_src(&keysrc, "data/keyrings/1/pubring.gpg"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_true(keyseq.keys.size() > 1);
    keysrc.close();

    assert_rnp_success(init_file_dest(&keydst, "keyout.gpg", true));
    assert_true(write_transferable_keys(keyseq, &keydst, false));
    dst_close(&keydst, false);

    assert_rnp_success(init_file_src(&keysrc, "keyout.gpg"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    keysrc.close();

    assert_rnp_success(init_file_dest(&keydst, "keyout.asc", true));
    assert_true(write_transferable_keys(keyseq, &keydst, true));
    dst_close(&keydst, false);

    assert_rnp_success(init_file_src(&keysrc, "keyout.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    keysrc.close();

    /* secret keyring */
    assert_rnp_success(init_file_src(&keysrc, "data/keyrings/1/secring.gpg"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_true(keyseq.keys.size() > 1);
    keysrc.close();

    assert_rnp_success(init_file_dest(&keydst, "keyout-sec.gpg", true));
    assert_true(write_transferable_keys(keyseq, &keydst, false));
    dst_close(&keydst, false);

    assert_rnp_success(init_file_src(&keysrc, "keyout-sec.gpg"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    keysrc.close();

    assert_rnp_success(init_file_dest(&keydst, "keyout-sec.asc", true));
    assert_true(write_transferable_keys(keyseq, &keydst, true));
    dst_close(&keydst, false);

    assert_rnp_success(init_file_src(&keysrc, "keyout-sec.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    keysrc.close();

    /* armored v3 public key */
    assert_rnp_success(init_file_src(&keysrc, "data/keyrings/4/rsav3-p.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    keyfp = pgp::Fingerprint(key->key);
    assert_true(cmp_keyid(keyfp.keyid(), "7D0BC10E933404C9"));
    assert_false(cmp_keyid(keyfp.keyid(), "1D0BC10E933404C9"));
    keysrc.close();

    /* armored v3 secret key */
    assert_rnp_success(init_file_src(&keysrc, "data/keyrings/4/rsav3-s.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    keyfp = pgp::Fingerprint(key->key);
    assert_true(cmp_keyid(keyfp.keyid(), "7D0BC10E933404C9"));
    keysrc.close();

    /* rsa/rsa public key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/rsa-rsa-pub.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    keyfp = pgp::Fingerprint(key->key);
    assert_true(cmp_keyfp(keyfp, "6BC04A5A3DDB35766B9A40D82FB9179118898E8B"));
    assert_true(cmp_keyid(keyfp.keyid(), "2FB9179118898E8B"));
    assert_int_equal(key->subkeys.size(), 1);
    assert_false(key->subkeys[0].subkey.pub_data.empty());
    keysrc.close();

    /* rsa/rsa secret key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/rsa-rsa-sec.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    keyfp = pgp::Fingerprint(key->key);
    assert_true(cmp_keyfp(keyfp, "6BC04A5A3DDB35766B9A40D82FB9179118898E8B"));
    assert_true(cmp_keyid(keyfp.keyid(), "2FB9179118898E8B"));
    assert_int_equal(key->subkeys.size(), 1);
    assert_false(key->subkeys[0].subkey.pub_data.empty());
    keysrc.close();

    /* dsa/el-gamal public key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/dsa-eg-pub.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    keyfp = pgp::Fingerprint(key->key);
    assert_true(cmp_keyfp(keyfp, "091C44CE9CFBC3FF7EC7A64DC8A10A7D78273E10"));
    assert_int_equal(key->subkeys.size(), 1);
    assert_non_null(skey = &key->subkeys.front());
    keyfp = pgp::Fingerprint(skey->subkey);
    assert_true(cmp_keyid(keyfp.keyid(), "02A5715C3537717E"));
    keysrc.close();

    /* dsa/el-gamal secret key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/dsa-eg-sec.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    assert_int_equal(key->subkeys.size(), 1);
    assert_false(key->subkeys[0].subkey.pub_data.empty());
    keysrc.close();

    /* curve 25519 ecc public key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-25519-pub.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    keyfp = pgp::Fingerprint(key->key);
    assert_true(cmp_keyfp(keyfp, "21FC68274AAE3B5DE39A4277CC786278981B0728"));
    keysrc.close();

    /* curve 25519 ecc secret key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-25519-sec.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    assert_true(key->subkeys.empty());
    keysrc.close();

    /* eddsa/x25519 ecc public key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-x25519-pub.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    keyfp = pgp::Fingerprint(key->key);
    assert_true(cmp_keyfp(keyfp, "4C9738A6F2BE4E1A796C9B7B941822A0FC1B30A5"));
    assert_int_equal(key->subkeys.size(), 1);
    assert_non_null(skey = &key->subkeys.front());
    keyfp = pgp::Fingerprint(skey->subkey);
    assert_true(cmp_keyid(keyfp.keyid(), "C711187E594376AF"));
    keysrc.close();

    /* eddsa/x25519 ecc secret key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-x25519-sec.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    assert_int_equal(key->subkeys.size(), 1);
    assert_false(key->subkeys[0].subkey.pub_data.empty());
    keysrc.close();

    /* p-256 ecc public key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-p256-pub.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    keyfp = pgp::Fingerprint(key->key);
    assert_true(cmp_keyfp(keyfp, "B54FDEBBB673423A5D0AA54423674F21B2441527"));
    assert_non_null(skey = &key->subkeys.front());
    keyfp = pgp::Fingerprint(skey->subkey);
    assert_true(cmp_keyid(keyfp.keyid(), "37E285E9E9851491"));
    keysrc.close();

    /* p-256 ecc secret key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-p256-sec.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    assert_int_equal(key->subkeys.size(), 1);
    assert_false(key->subkeys[0].subkey.pub_data.empty());
    keysrc.close();

    /* p-384 ecc public key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-p384-pub.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    keyfp = pgp::Fingerprint(key->key);
    assert_true(cmp_keyfp(keyfp, "AB25CBA042DD924C3ACC3ED3242A3AA5EA85F44A"));
    assert_non_null(skey = &key->subkeys.front());
    keyfp = pgp::Fingerprint(skey->subkey);
    assert_true(cmp_keyid(keyfp.keyid(), "E210E3D554A4FAD9"));
    keysrc.close();

    /* p-384 ecc secret key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-p384-sec.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    assert_int_equal(key->subkeys.size(), 1);
    assert_false(key->subkeys[0].subkey.pub_data.empty());
    keysrc.close();

    /* p-521 ecc public key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-p521-pub.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    keyfp = pgp::Fingerprint(key->key);
    assert_true(cmp_keyfp(keyfp, "4FB39FF6FA4857A4BD7EF5B42092CA8324263B6A"));
    assert_non_null(skey = &key->subkeys.front());
    keyfp = pgp::Fingerprint(skey->subkey);
    assert_true(cmp_keyid(keyfp.keyid(), "9853DF2F6D297442"));
    keysrc.close();

    /* p-521 ecc secret key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-p521-sec.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    assert_int_equal(key->subkeys.size(), 1);
    assert_false(key->subkeys[0].subkey.pub_data.empty());
    keysrc.close();

    /* Brainpool P256 ecc public key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-bp256-pub.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    keyfp = pgp::Fingerprint(key->key);
    assert_true(cmp_keyfp(keyfp, "0633C5F72A198F51E650E4ABD0C8A3DAF9E0634A"));
    assert_non_null(skey = &key->subkeys.front());
    keyfp = pgp::Fingerprint(skey->subkey);
    assert_true(cmp_keyid(keyfp.keyid(), "2EDABB94D3055F76"));
    keysrc.close();

    /* Brainpool P256 ecc secret key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-bp256-sec.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    assert_int_equal(key->subkeys.size(), 1);
    assert_false(key->subkeys[0].subkey.pub_data.empty());
    keysrc.close();

    /* Brainpool P384 ecc public key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-bp384-pub.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    keyfp = pgp::Fingerprint(key->key);
    assert_true(cmp_keyfp(keyfp, "5B8A254C823CED98DECD10ED6CF2DCE85599ADA2"));
    assert_non_null(skey = &key->subkeys.front());
    keyfp = pgp::Fingerprint(skey->subkey);
    assert_true(cmp_keyid(keyfp.keyid(), "CFF1BB6F16D28191"));
    keysrc.close();

    /* Brainpool P384 ecc secret key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-bp384-sec.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    assert_int_equal(key->subkeys.size(), 1);
    assert_false(key->subkeys[0].subkey.pub_data.empty());
    keysrc.close();

    /* Brainpool P512 ecc public key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-bp512-pub.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    keyfp = pgp::Fingerprint(key->key);
    assert_true(cmp_keyfp(keyfp, "4C59AB9272AA6A1F60B85BD0AA5C58D14F7B8F48"));
    assert_non_null(skey = &key->subkeys.front());
    keyfp = pgp::Fingerprint(skey->subkey);
    assert_true(cmp_keyid(keyfp.keyid(), "20CDAA1482BA79CE"));
    keysrc.close();

    /* Brainpool P512 ecc secret key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-bp512-sec.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    assert_int_equal(key->subkeys.size(), 1);
    assert_false(key->subkeys[0].subkey.pub_data.empty());
    keysrc.close();

    /* secp256k1 ecc public key, not supported now */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-p256k1-pub.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    keyfp = pgp::Fingerprint(key->key);
    assert_true(cmp_keyfp(keyfp, "81F772B57D4EBFE7000A66233EA5BB6F9692C1A0"));
    assert_non_null(skey = &key->subkeys.front());
    keyfp = pgp::Fingerprint(skey->subkey);
    assert_true(cmp_keyid(keyfp.keyid(), "7635401F90D3E533"));
    keysrc.close();

    /* secp256k1 ecc secret key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-p256k1-sec.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_int_equal(keyseq.keys.size(), 1);
    assert_non_null(key = &keyseq.keys.front());
    assert_int_equal(key->subkeys.size(), 1);
    assert_false(key->subkeys[0].subkey.pub_data.empty());
    keysrc.close();
}

static void
buggy_key_load_single(const void *keydata, size_t keylen)
{
    pgp_source_t       memsrc = {0};
    pgp_key_sequence_t keyseq;
    size_t             partlen;
    uint8_t *          dataptr;

    /* try truncated load */
    for (partlen = 1; partlen < keylen; partlen += 15) {
        assert_rnp_success(init_mem_src(&memsrc, keydata, partlen, false));
        if (!process_pgp_keys(memsrc, keyseq, false)) {
            /* it may succeed if we accidentally hit some packet boundary */
            assert_false(keyseq.keys.empty());
        } else {
            assert_true(keyseq.keys.empty());
        }
        memsrc.close();
    }

    /* try modified load */
    dataptr = (uint8_t *) keydata;
    for (partlen = 1; partlen < keylen; partlen++) {
        dataptr[partlen] ^= 0xff;
        assert_rnp_success(init_mem_src(&memsrc, keydata, keylen, false));
        if (!process_pgp_keys(memsrc, keyseq, false)) {
            /* it may succeed if we accidentally hit some packet boundary */
            assert_false(keyseq.keys.empty());
        } else {
            assert_true(keyseq.keys.empty());
        }
        memsrc.close();
        dataptr[partlen] ^= 0xff;
    }
}

/* check for memory leaks during buggy key loads */
TEST_F(rnp_tests, test_stream_key_load_errors)
{
    pgp_source_t fsrc = {0};
    pgp_source_t armorsrc = {0};
    pgp_source_t memsrc = {0};

    const char *key_files[] = {"data/keyrings/4/rsav3-p.asc",
                               "data/keyrings/4/rsav3-s.asc",
                               "data/keyrings/1/pubring.gpg",
                               "data/keyrings/1/secring.gpg",
                               "data/test_stream_key_load/dsa-eg-pub.asc",
                               "data/test_stream_key_load/dsa-eg-sec.asc",
                               "data/test_stream_key_load/ecc-25519-pub.asc",
                               "data/test_stream_key_load/ecc-25519-sec.asc",
                               "data/test_stream_key_load/ecc-x25519-pub.asc",
                               "data/test_stream_key_load/ecc-x25519-sec.asc",
                               "data/test_stream_key_load/ecc-p256-pub.asc",
                               "data/test_stream_key_load/ecc-p256-sec.asc",
                               "data/test_stream_key_load/ecc-p384-pub.asc",
                               "data/test_stream_key_load/ecc-p384-sec.asc",
                               "data/test_stream_key_load/ecc-p521-pub.asc",
                               "data/test_stream_key_load/ecc-p521-sec.asc",
                               "data/test_stream_key_load/ecc-bp256-pub.asc",
                               "data/test_stream_key_load/ecc-bp256-sec.asc",
                               "data/test_stream_key_load/ecc-bp384-pub.asc",
                               "data/test_stream_key_load/ecc-bp384-sec.asc",
                               "data/test_stream_key_load/ecc-bp512-pub.asc",
                               "data/test_stream_key_load/ecc-bp512-sec.asc",
                               "data/test_stream_key_load/ecc-p256k1-pub.asc",
                               "data/test_stream_key_load/ecc-p256k1-sec.asc"};

    for (size_t i = 0; i < sizeof(key_files) / sizeof(char *); i++) {
        assert_rnp_success(init_file_src(&fsrc, key_files[i]));
        if (fsrc.is_armored()) {
            assert_rnp_success(init_armored_src(&armorsrc, &fsrc));
            assert_rnp_success(read_mem_src(&memsrc, &armorsrc));
            armorsrc.close();
        } else {
            assert_rnp_success(read_mem_src(&memsrc, &fsrc));
        }
        fsrc.close();
        buggy_key_load_single(mem_src_get_memory(&memsrc), memsrc.size);
        memsrc.close();
    }
}

TEST_F(rnp_tests, test_stream_key_decrypt)
{
    pgp_source_t               keysrc = {0};
    pgp_key_sequence_t         keyseq;
    pgp_transferable_key_t *   key = NULL;
    pgp_transferable_subkey_t *subkey = NULL;

    /* load and decrypt secret keyring */
    assert_rnp_success(init_file_src(&keysrc, "data/keyrings/1/secring.gpg"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    for (auto &key : keyseq.keys) {
        assert_rnp_failure(decrypt_secret_key(&key.key, "passw0rd"));
        assert_rnp_success(decrypt_secret_key(&key.key, "password"));

        for (auto &subkey : key.subkeys) {
            assert_rnp_failure(decrypt_secret_key(&subkey.subkey, "passw0rd"));
            assert_rnp_success(decrypt_secret_key(&subkey.subkey, "password"));
        }
    }
    keysrc.close();

    /* armored v3 secret key */
    assert_rnp_success(init_file_src(&keysrc, "data/keyrings/4/rsav3-s.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_non_null(key = &keyseq.keys.front());
    assert_rnp_failure(decrypt_secret_key(&key->key, "passw0rd"));
#if defined(ENABLE_IDEA)
    assert_rnp_success(decrypt_secret_key(&key->key, "password"));
#else
    assert_rnp_failure(decrypt_secret_key(&key->key, "password"));
#endif
    keysrc.close();

    /* rsa/rsa secret key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/rsa-rsa-sec.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_non_null(key = &keyseq.keys.front());
    assert_rnp_success(decrypt_secret_key(&key->key, "password"));
    assert_non_null(subkey = &key->subkeys.front());
    assert_rnp_success(decrypt_secret_key(&subkey->subkey, "password"));
    keysrc.close();

    /* dsa/el-gamal secret key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/dsa-eg-sec.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_non_null(key = &keyseq.keys.front());
    assert_rnp_success(decrypt_secret_key(&key->key, "password"));
    assert_non_null(subkey = &key->subkeys.front());
    assert_rnp_success(decrypt_secret_key(&subkey->subkey, "password"));
    keysrc.close();

    /* curve 25519 eddsa ecc secret key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-25519-sec.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_non_null(key = &keyseq.keys.front());
    assert_rnp_success(decrypt_secret_key(&key->key, "password"));
    keysrc.close();

    /* x25519 ecc secret key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-x25519-sec.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_non_null(key = &keyseq.keys.front());
    assert_rnp_success(decrypt_secret_key(&key->key, "password"));
    assert_non_null(subkey = &key->subkeys.front());
    assert_rnp_success(decrypt_secret_key(&subkey->subkey, "password"));
    keysrc.close();

    /* p-256 ecc secret key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-p256-sec.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_non_null(key = &keyseq.keys.front());
    assert_rnp_success(decrypt_secret_key(&key->key, "password"));
    assert_non_null(subkey = &key->subkeys.front());
    assert_rnp_success(decrypt_secret_key(&subkey->subkey, "password"));
    keysrc.close();

    /* p-384 ecc secret key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-p384-sec.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_non_null(key = &keyseq.keys.front());
    assert_rnp_success(decrypt_secret_key(&key->key, "password"));
    assert_non_null(subkey = &key->subkeys.front());
    assert_rnp_success(decrypt_secret_key(&subkey->subkey, "password"));
    keysrc.close();

    /* p-521 ecc secret key */
    assert_rnp_success(init_file_src(&keysrc, "data/test_stream_key_load/ecc-p521-sec.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    assert_non_null(key = &keyseq.keys.front());
    assert_rnp_success(decrypt_secret_key(&key->key, "password"));
    assert_non_null(subkey = &key->subkeys.front());
    assert_rnp_success(decrypt_secret_key(&subkey->subkey, "password"));
    keysrc.close();
}

TEST_F(rnp_tests, test_stream_key_encrypt)
{
    pgp_source_t       keysrc = {0};
    pgp_dest_t         keydst = {0};
    uint8_t            keybuf[16384];
    size_t             keylen;
    pgp_key_sequence_t keyseq;
    pgp_key_sequence_t keyseq2;

    /* load and decrypt secret keyring, then re-encrypt and reload keys */
    assert_rnp_success(init_file_src(&keysrc, "data/keyrings/1/secring.gpg"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    keysrc.close();
    for (auto &key : keyseq.keys) {
        assert_rnp_success(decrypt_secret_key(&key.key, "password"));

        for (auto &subkey : key.subkeys) {
            assert_rnp_success(decrypt_secret_key(&subkey.subkey, "password"));
        }

        /* change password and encryption algorithm */
        key.key.sec_protection.symm_alg = PGP_SA_CAMELLIA_192;
        assert_rnp_success(encrypt_secret_key(&key.key, "passw0rd", global_ctx.rng));
        for (auto &subkey : key.subkeys) {
            subkey.subkey.sec_protection.symm_alg = PGP_SA_CAMELLIA_256;
            assert_rnp_success(encrypt_secret_key(&subkey.subkey, "passw0rd", global_ctx.rng));
        }
        /* write changed key */
        assert_rnp_success(init_mem_dest(&keydst, keybuf, sizeof(keybuf)));
        assert_true(write_transferable_key(key, keydst));
        keylen = keydst.writeb;
        dst_close(&keydst, false);
        /* load and decrypt changed key */
        assert_rnp_success(init_mem_src(&keysrc, keybuf, keylen, false));
        assert_rnp_success(process_pgp_keys(keysrc, keyseq2, false));
        keysrc.close();
        assert_false(keyseq2.keys.empty());
        auto &key2 = keyseq2.keys.front();
        assert_int_equal(key2.key.sec_protection.symm_alg, PGP_SA_CAMELLIA_192);
        assert_rnp_success(decrypt_secret_key(&key2.key, "passw0rd"));

        for (auto &subkey : key2.subkeys) {
            assert_int_equal(subkey.subkey.sec_protection.symm_alg, PGP_SA_CAMELLIA_256);
            assert_rnp_success(decrypt_secret_key(&subkey.subkey, "passw0rd"));
        }
        /* write key without the password */
        key2.key.sec_protection.s2k.usage = PGP_S2KU_NONE;
        assert_rnp_success(encrypt_secret_key(&key2.key, NULL, global_ctx.rng));
        for (auto &subkey : key2.subkeys) {
            subkey.subkey.sec_protection.s2k.usage = PGP_S2KU_NONE;
            assert_rnp_success(encrypt_secret_key(&subkey.subkey, NULL, global_ctx.rng));
        }
        /* write changed key */
        assert_rnp_success(init_mem_dest(&keydst, keybuf, sizeof(keybuf)));
        assert_true(write_transferable_key(key2, keydst));
        keylen = keydst.writeb;
        dst_close(&keydst, false);
        /* load non-encrypted key */
        assert_rnp_success(init_mem_src(&keysrc, keybuf, keylen, false));
        assert_rnp_success(process_pgp_keys(keysrc, keyseq2, false));
        keysrc.close();
        assert_false(keyseq2.keys.empty());
        auto &key3 = keyseq2.keys.front();
        assert_int_equal(key3.key.sec_protection.s2k.usage, PGP_S2KU_NONE);
        assert_rnp_success(decrypt_secret_key(&key3.key, NULL));

        for (auto &subkey : key3.subkeys) {
            assert_int_equal(subkey.subkey.sec_protection.s2k.usage, PGP_S2KU_NONE);
            assert_rnp_success(decrypt_secret_key(&subkey.subkey, NULL));
        }
    }
}

TEST_F(rnp_tests, test_stream_key_signatures)
{
    pgp_source_t       keysrc = {0};
    pgp_key_sequence_t keyseq;
    rnp::Key *         pkey = nullptr;
    rnp::SignatureInfo sinfo;

    /* v3 public key */
    auto pubring = new rnp::KeyStore("data/keyrings/4/rsav3-p.asc", global_ctx);
    assert_true(pubring->load());
    assert_rnp_success(init_file_src(&keysrc, "data/keyrings/4/rsav3-p.asc"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    keysrc.close();
    assert_int_equal(keyseq.keys.size(), 1);
    auto &key = keyseq.keys.front();
    auto &uid = key.userids.front();
    auto &sig = uid.signatures.front();
    assert_non_null(pkey = pubring->get_signer(sig));
    /* check certification signature */
    auto hash = signature_hash_certification(sig, key.key, uid.uid);
    /* this signature uses MD5 hash after the allowed date */
    auto res = signature_validate(sig, *pkey->material(), *hash, global_ctx);
    assert_int_equal(res.errors().size(), 1);
    assert_int_equal(res.errors().at(0), RNP_ERROR_SIG_WEAK_HASH);
    /* add rule which allows MD5 */
    rnp::SecurityRule allow_md5(
      rnp::FeatureType::Hash, PGP_HASH_MD5, rnp::SecurityLevel::Default);
    allow_md5.override = true;
    global_ctx.profile.add_rule(allow_md5);
    hash = signature_hash_certification(sig, key.key, uid.uid);
    assert_true(
      signature_validate(sig, *pkey->material(), *hash, global_ctx).errors().empty());
    /* modify userid and check signature */
    uid.uid.uid[2] = '?';
    hash = signature_hash_certification(sig, key.key, uid.uid);
    assert_false(
      signature_validate(sig, *pkey->material(), *hash, global_ctx).errors().empty());
    /* remove MD5 rule */
    assert_true(global_ctx.profile.del_rule(allow_md5));
    delete pubring;

    /* keyring */
    pubring = new rnp::KeyStore("data/keyrings/1/pubring.gpg", global_ctx);
    assert_true(pubring->load());
    assert_rnp_success(init_file_src(&keysrc, "data/keyrings/1/pubring.gpg"));
    assert_rnp_success(process_pgp_keys(keysrc, keyseq, false));
    keysrc.close();

    /* check key signatures */
    for (auto &keyref : keyseq.keys) {
        for (auto &uid : keyref.userids) {
            /* userid certifications */
            for (auto &sig : uid.signatures) {
                assert_non_null(pkey = pubring->get_signer(sig));
                /* high level interface */
                sinfo.sig = &sig;
                sinfo.validity.reset();
                pkey->validate_cert(sinfo, keyref.key, uid.uid, global_ctx);
                assert_true(sinfo.validity.valid());
                /* low level check */
                auto hash = signature_hash_certification(sig, keyref.key, uid.uid);
                auto res = signature_validate(sig, *pkey->material(), *hash, global_ctx);
                assert_true(res.errors().empty());
                /* modify userid and check signature */
                uid.uid.uid[2] = '?';
                sinfo.validity.reset();
                pkey->validate_cert(sinfo, keyref.key, uid.uid, global_ctx);
                assert_false(sinfo.validity.valid());
                hash = signature_hash_certification(sig, keyref.key, uid.uid);
                res = signature_validate(sig, *pkey->material(), *hash, global_ctx);
                assert_false(res.errors().empty());
            }
        }

        /* subkey binding signatures */
        for (auto &subkey : keyref.subkeys) {
            auto &sig = subkey.signatures.front();
            assert_non_null(pkey = pubring->get_signer(sig));
            /* high level interface */
            sinfo.sig = &sig;
            pgp::KeyID subid = pgp::Fingerprint(subkey.subkey).keyid();
            char       ssubid[PGP_KEY_ID_SIZE * 2 + 1];
            assert_true(rnp::hex_encode(subid.data(), subid.size(), ssubid, sizeof(ssubid)));
            rnp::Key *psub = rnp_tests_get_key_by_id(pubring, ssubid);
            assert_non_null(psub);
            sinfo.validity.reset();
            pkey->validate_binding(sinfo, *psub, global_ctx);
            assert_true(sinfo.validity.valid());
            /* low level check */
            hash = signature_hash_binding(sig, keyref.key, subkey.subkey);
            sinfo.validity.reset();
            pkey->validate_sig(sinfo, *hash, global_ctx);
            assert_true(sinfo.validity.valid());
        }
    }

    delete pubring;
}

static bool
validate_key_sigs(const char *path, rnp::SecurityContext &global_ctx)
{
    auto pubring = new rnp::KeyStore(path, global_ctx);
    bool valid = pubring->load();
    for (auto &key : pubring->keys) {
        key.validate(*pubring);
        valid = valid && key.valid();
    }
    delete pubring;
    return valid;
}

TEST_F(rnp_tests, test_stream_key_signature_validate)
{
    /* v3 public key */
    auto pubring = new rnp::KeyStore("data/keyrings/4/rsav3-p.asc", global_ctx);
    assert_true(pubring->load());
    assert_int_equal(pubring->key_count(), 1);
    rnp::Key &pkey = pubring->keys.front();
    pkey.validate(*pubring);
    /* MD5 signature is marked as invalid by default */
    assert_false(pkey.valid());
    rnp::SecurityRule allow_md5(
      rnp::FeatureType::Hash, PGP_HASH_MD5, rnp::SecurityLevel::Default);
    allow_md5.override = true;
    /* Allow MD5 */
    global_ctx.profile.add_rule(allow_md5);
    /* we need to manually reset signature validity */
    pkey.get_sig(0).validity.reset();
    pkey.revalidate(*pubring);
    assert_true(pkey.valid());
    /* Remove MD5 and revalidate */
    assert_true(global_ctx.profile.del_rule(allow_md5));
    pkey.get_sig(0).validity.reset();
    pkey.revalidate(*pubring);
    assert_false(pkey.valid());
    delete pubring;

    /* keyring */
    pubring = new rnp::KeyStore("data/keyrings/1/pubring.gpg", global_ctx);
    assert_true(pubring->load());
    assert_true(pubring->key_count() > 0);
    int i = 0;
    for (auto &key : pubring->keys) {
        key.validate(*pubring);
        // subkey #2 is expired
        if (i == 2) {
            assert_false(key.valid());
        } else {
            assert_true(key.valid());
        }
        i++;
    }
    delete pubring;

    /* misc key files */
    auto validate = [this](const std::string &file) {
        auto path = "data/test_stream_key_load/" + file;
        return validate_key_sigs(path.c_str(), this->global_ctx);
    };
    assert_true(validate("dsa-eg-pub.asc"));
    assert_true(validate("dsa-eg-sec.asc"));
    assert_true(validate("ecc-25519-pub.asc"));
    assert_true(validate("ecc-25519-sec.asc"));
    assert_true(validate("ecc-x25519-pub.asc"));
    assert_true(validate("ecc-x25519-sec.asc"));
    assert_true(validate("ecc-p256-pub.asc"));
    assert_true(validate("ecc-p256-sec.asc"));
    assert_true(validate("ecc-p384-pub.asc"));
    assert_true(validate("ecc-p384-sec.asc"));
    assert_true(validate("ecc-p521-pub.asc"));
    assert_true(validate("ecc-p521-sec.asc"));
    assert_true(validate("ecc-bp256-pub.asc") == brainpool_enabled());
    assert_true(validate("ecc-bp256-sec.asc") == brainpool_enabled());
    assert_true(validate("ecc-bp384-pub.asc") == brainpool_enabled());
    assert_true(validate("ecc-bp384-sec.asc") == brainpool_enabled());
    assert_true(validate("ecc-bp512-pub.asc") == brainpool_enabled());
    assert_true(validate("ecc-bp512-sec.asc") == brainpool_enabled());
    assert_true(validate("ecc-p256k1-pub.asc"));
    assert_true(validate("ecc-p256k1-sec.asc"));
}

TEST_F(rnp_tests, test_stream_verify_no_key)
{
    cli_rnp_t rnp;
    rnp_cfg   cfg;

    /* setup rnp structure and params */
    cfg.set_str(CFG_KR_PUB_PATH, "");
    cfg.set_str(CFG_KR_SEC_PATH, "");
    cfg.set_str(CFG_KR_PUB_FORMAT, RNP_KEYSTORE_GPG);
    cfg.set_str(CFG_KR_SEC_FORMAT, RNP_KEYSTORE_GPG);
    assert_true(rnp.init(cfg));

    rnp_cfg &rnpcfg = rnp.cfg();
    /* setup cfg for verification */
    rnpcfg.set_str(CFG_INFILE, "data/test_stream_verification/verify_encrypted_no_key.pgp");
    rnpcfg.set_str(CFG_OUTFILE, "output.dat");
    rnpcfg.set_bool(CFG_OVERWRITE, true);
    /* setup operation context */
    assert_rnp_success(
      rnp_ffi_set_pass_provider(rnp.ffi, ffi_string_password_provider, (void *) "pass1"));
    rnpcfg.set_bool(CFG_NO_OUTPUT, false);
    if (sm2_enabled()) {
        /* operation should success if output is not discarded, i.e. operation = decrypt */
        assert_true(cli_rnp_process_file(&rnp));
        assert_int_equal(file_size("output.dat"), 4);
    } else {
        /* operation should fail */
        assert_false(cli_rnp_process_file(&rnp));
        assert_int_equal(file_size("output.dat"), -1);
    }
    /* try second password */
    assert_rnp_success(
      rnp_ffi_set_pass_provider(rnp.ffi, ffi_string_password_provider, (void *) "pass2"));
    assert_true(cli_rnp_process_file(&rnp));
    assert_int_equal(file_size("output.dat"), 4);
    /* decryption/verification fails without password */
    assert_rnp_success(
      rnp_ffi_set_pass_provider(rnp.ffi, ffi_failing_password_provider, NULL));
    assert_false(cli_rnp_process_file(&rnp));
    assert_int_equal(file_size("output.dat"), -1);
    /* decryption/verification fails with wrong password */
    assert_rnp_success(
      rnp_ffi_set_pass_provider(rnp.ffi, ffi_string_password_provider, (void *) "pass_wrong"));
    assert_false(cli_rnp_process_file(&rnp));
    assert_int_equal(file_size("output.dat"), -1);
    /* verification fails if output is discarded, i.e. operation = verify */
    rnpcfg.set_bool(CFG_NO_OUTPUT, true);
    assert_false(cli_rnp_process_file(&rnp));
    assert_int_equal(file_size("output.dat"), -1);

    /* cleanup */
    rnp.end();
}

static bool
check_dump_file_dst(const char *file, bool mpi, bool grip)
{
    rnp::Source src;
    rnp::Dest   dst;

    if (init_file_src(&src.src(), file)) {
        return false;
    }
    if (init_mem_dest(&dst.dst(), NULL, 0)) {
        return false;
    }
    rnp::DumpContextDst ctx(src.src(), dst.dst());
    ctx.set_dump_mpi(mpi);
    ctx.set_dump_grips(grip);

    if (ctx.dump()) {
        return false;
    }
    return true;
}

static bool
check_dump_file_json(const char *file, bool mpi, bool grip)
{
    rnp::Source src;
    if (init_file_src(&src.src(), file)) {
        return false;
    }

    json_object *        jso = NULL;
    rnp::DumpContextJson ctx(src.src(), &jso);
    ctx.set_dump_mpi(mpi);
    ctx.set_dump_grips(grip);

    if (ctx.dump()) {
        return false;
    }
    if (!json_object_is_type(jso, json_type_array)) {
        return false;
    }
    json_object_put(jso);
    return true;
}

static bool
check_dump_file(const char *file, bool mpi, bool grip)
{
    return check_dump_file_dst(file, mpi, grip) && check_dump_file_json(file, mpi, grip);
}

TEST_F(rnp_tests, test_y2k38)
{
    cli_rnp_t rnp;
    rnp_cfg   cfg;

    /* setup rnp structure and params */
    cfg.set_str(CFG_KR_PUB_PATH, "data/keyrings/6");
    cfg.set_str(CFG_KR_SEC_PATH, "data/keyrings/6");
    cfg.set_str(CFG_KR_PUB_FORMAT, RNP_KEYSTORE_GPG);
    cfg.set_str(CFG_KR_SEC_FORMAT, RNP_KEYSTORE_GPG);
    cfg.set_str(CFG_IO_RESS, "stderr.dat");
    assert_true(rnp.init(cfg));

    rnp_cfg &rnpcfg = rnp.cfg();
    /* verify */
    rnpcfg.set_str(CFG_INFILE, "data/test_messages/future.pgp");
    rnpcfg.set_bool(CFG_OVERWRITE, true);
    assert_false(cli_rnp_process_file(&rnp));

    /* clean up and flush the file */
    rnp.end();

    /* check the file for presence of correct dates */
    auto        output = file_to_str("stderr.dat");
    time_t      crtime = 0xC0000000;
    std::string correctMade = "signature made ";
    if (rnp_y2k38_warning(crtime)) {
        correctMade += ">=";
    }
    correctMade += rnp_ctime(crtime);
    assert_true(
      std::search(output.begin(), output.end(), correctMade.begin(), correctMade.end()) !=
      output.end());
    time_t      validtime = rnp_timeadd(crtime, 0xD0000000);
    std::string correctValid = "Valid until ";
    if (rnp_y2k38_warning(validtime)) {
        correctValid += ">=";
    }
    correctValid += rnp_ctime(validtime);
    assert_true(
      std::search(output.begin(), output.end(), correctValid.begin(), correctValid.end()) !=
      output.end());
    unlink("stderr.dat");
}

TEST_F(rnp_tests, test_stream_dumper_y2k38)
{
    rnp::Source src;
    rnp::Dest   dst;
    assert_rnp_success(init_file_src(&src.src(), "data/keyrings/6/pubring.gpg"));
    assert_rnp_success(init_mem_dest(&dst.dst(), NULL, 0));
    rnp::DumpContextDst ctx(src.src(), dst.dst());
    assert_rnp_success(ctx.dump());
    auto   written = (const uint8_t *) mem_dest_get_memory(&dst.dst());
    auto   last = written + dst.writeb();
    time_t timestamp = 2958774690;
    // regenerate time for the current timezone
    std::string correct = "creation time: 2958774690 (";
    if (rnp_y2k38_warning(timestamp)) {
        correct += ">=";
    }
    correct += rnp_ctime(timestamp).substr(0, 24);
    correct += ')';
    assert_true(std::search(written, last, correct.begin(), correct.end()) != last);
}

TEST_F(rnp_tests, test_stream_dumper)
{
    assert_true(check_dump_file("data/keyrings/1/pubring.gpg", false, false));
    assert_true(check_dump_file("data/keyrings/1/secring.gpg", false, false));
    assert_true(check_dump_file("data/keyrings/4/rsav3-p.asc", false, false));
    assert_true(check_dump_file("data/keyrings/4/rsav3-p.asc", true, true));
    assert_true(check_dump_file("data/keyrings/4/rsav3-s.asc", true, false));
    assert_true(check_dump_file("data/test_repgp/encrypted_text.gpg", true, false));
    assert_true(check_dump_file("data/test_repgp/signed.gpg", true, false));
    assert_true(check_dump_file("data/test_repgp/encrypted_key.gpg", true, false));
    assert_true(check_dump_file("data/test_stream_key_load/dsa-eg-pub.asc", true, true));
    assert_true(check_dump_file("data/test_stream_key_load/dsa-eg-sec.asc", true, true));
    assert_true(check_dump_file("data/test_stream_key_load/ecc-25519-pub.asc", true, true));
    assert_true(check_dump_file("data/test_stream_key_load/ecc-25519-sec.asc", true, true));
    assert_true(check_dump_file("data/test_stream_key_load/ecc-x25519-pub.asc", true, true));
    assert_true(check_dump_file("data/test_stream_key_load/ecc-x25519-sec.asc", true, true));
    assert_true(check_dump_file("data/test_stream_key_load/ecc-p256-pub.asc", true, true));
    assert_true(check_dump_file("data/test_stream_key_load/ecc-p256-sec.asc", true, true));
    assert_true(check_dump_file("data/test_stream_key_load/ecc-p384-pub.asc", true, true));
    assert_true(check_dump_file("data/test_stream_key_load/ecc-p384-sec.asc", true, true));
    assert_true(check_dump_file("data/test_stream_key_load/ecc-p521-pub.asc", true, true));
    assert_true(check_dump_file("data/test_stream_key_load/ecc-p521-sec.asc", true, true));
    assert_true(check_dump_file("data/test_stream_key_load/ecc-bp256-pub.asc", true, true));
    assert_true(check_dump_file("data/test_stream_key_load/ecc-bp256-sec.asc", true, true));
    assert_true(check_dump_file("data/test_stream_key_load/ecc-bp384-pub.asc", true, true));
    assert_true(check_dump_file("data/test_stream_key_load/ecc-bp384-sec.asc", true, true));
    assert_true(check_dump_file("data/test_stream_key_load/ecc-bp512-pub.asc", true, true));
    assert_true(check_dump_file("data/test_stream_key_load/ecc-bp512-sec.asc", true, true));
    assert_true(check_dump_file("data/test_stream_key_load/ecc-p256k1-pub.asc", true, true));
    assert_true(check_dump_file("data/test_stream_key_load/ecc-p256k1-sec.asc", true, true));
    assert_true(check_dump_file("data/test_stream_signatures/source.txt.asc", true, true));
    assert_true(check_dump_file("data/test_stream_signatures/source.txt.asc.asc", true, true));
    assert_true(check_dump_file(
      "data/test_stream_verification/verify_encrypted_no_key.pgp", true, true));

    pgp_source_t src;
    pgp_dest_t   dst;
    assert_rnp_success(init_file_src(&src, "data/test_stream_signatures/source.txt"));
    assert_rnp_success(init_mem_dest(&dst, NULL, 0));
    rnp::DumpContextDst ctx(src, dst);
    assert_rnp_failure(ctx.dump());
    src.close();
    dst_close(&dst, false);

    assert_rnp_success(init_file_src(&src, "data/test_messages/message.txt.enc-no-mdc"));
    assert_rnp_success(init_mem_dest(&dst, NULL, 0));
    rnp::DumpContextDst ctx2(src, dst);
    assert_rnp_success(ctx2.dump());
    src.close();
    dst_close(&dst, false);

    assert_rnp_success(init_file_src(&src, "data/test_messages/message.txt.enc-mdc"));
    assert_rnp_success(init_mem_dest(&dst, NULL, 0));
    rnp::DumpContextDst ctx3(src, dst);
    assert_rnp_success(ctx3.dump());
    src.close();
    dst_close(&dst, false);

    assert_rnp_success(init_file_src(&src, "data/test_messages/message-32k-crlf.txt.gpg"));
    assert_rnp_success(init_mem_dest(&dst, NULL, 0));
    rnp::DumpContextDst ctx4(src, dst);
    assert_rnp_success(ctx4.dump());
    src.close();
    dst_close(&dst, false);
}

TEST_F(rnp_tests, test_stream_z)
{
    pgp_source_t src;
    pgp_dest_t   dst;

    /* packet dumper will decompress source stream, making less code lines here */
    assert_rnp_success(init_file_src(&src, "data/test_stream_z/4gb.bzip2"));
    assert_rnp_success(init_null_dest(&dst));
    rnp::DumpContextDst ctx(src, dst);
    ctx.set_dump_mpi(true);
    ctx.set_dump_packets(true);
    assert_rnp_success(ctx.dump());
    src.close();
    dst_close(&dst, true);

    assert_rnp_success(init_file_src(&src, "data/test_stream_z/4gb.bzip2.cut"));
    assert_rnp_success(init_null_dest(&dst));
    rnp::DumpContextDst ctx2(src, dst);
    ctx2.set_dump_mpi(true);
    ctx2.set_dump_packets(true);
    assert_rnp_success(ctx2.dump());
    src.close();
    dst_close(&dst, true);

    assert_rnp_success(init_file_src(&src, "data/test_stream_z/128mb.zlib"));
    assert_rnp_success(init_null_dest(&dst));
    rnp::DumpContextDst ctx3(src, dst);
    ctx3.set_dump_mpi(true);
    ctx3.set_dump_packets(true);
    assert_rnp_success(ctx3.dump());
    src.close();
    dst_close(&dst, true);

    assert_rnp_success(init_file_src(&src, "data/test_stream_z/128mb.zlib.cut"));
    assert_rnp_success(init_null_dest(&dst));
    rnp::DumpContextDst ctx4(src, dst);
    ctx4.set_dump_mpi(true);
    ctx4.set_dump_packets(true);
    assert_rnp_success(ctx4.dump());
    src.close();
    dst_close(&dst, true);

    assert_rnp_success(init_file_src(&src, "data/test_stream_z/128mb.zip"));
    assert_rnp_success(init_null_dest(&dst));
    rnp::DumpContextDst ctx5(src, dst);
    ctx5.set_dump_mpi(true);
    ctx5.set_dump_packets(true);
    assert_rnp_success(ctx5.dump());
    src.close();
    dst_close(&dst, true);

    assert_rnp_success(init_file_src(&src, "data/test_stream_z/128mb.zip.cut"));
    assert_rnp_success(init_null_dest(&dst));
    rnp::DumpContextDst ctx6(src, dst);
    ctx6.set_dump_mpi(true);
    ctx6.set_dump_packets(true);
    assert_rnp_success(ctx6.dump());
    src.close();
    dst_close(&dst, true);
}

/* This test checks for GitHub issue #814.
 */
TEST_F(rnp_tests, test_stream_814_dearmor_double_free)
{
    pgp_source_t src;
    pgp_dest_t   dst;
    const char * buf = "-----BEGIN PGP BAD HEADER-----";

    assert_rnp_success(init_mem_src(&src, buf, strlen(buf), false));
    assert_rnp_success(init_null_dest(&dst));
    assert_rnp_failure(rnp_dearmor_source(&src, &dst));
    src.close();
    dst_close(&dst, true);
}

TEST_F(rnp_tests, test_stream_825_dearmor_blank_line)
{
    pgp_source_t src = {};

    auto keystore = new rnp::KeyStore("", global_ctx);
    assert_rnp_success(
      init_file_src(&src, "data/test_stream_armor/extra_line_before_trailer.asc"));
    assert_true(keystore->load(src));
    assert_int_equal(keystore->key_count(), 2);
    src.close();
    delete keystore;
}

static bool
try_dearmor(const char *str, int len)
{
    pgp_source_t src = {};
    pgp_dest_t   dst = {};
    bool         res = false;

    if (len < 0) {
        return false;
    }
    if (init_mem_src(&src, str, len, false) != RNP_SUCCESS) {
        goto done;
    }
    if (init_null_dest(&dst) != RNP_SUCCESS) {
        goto done;
    }
    res = rnp_dearmor_source(&src, &dst) == RNP_SUCCESS;
done:
    src.close();
    dst_close(&dst, true);
    return res;
}

TEST_F(rnp_tests, test_stream_dearmor_edge_cases)
{
    const char *HDR = "-----BEGIN PGP PUBLIC KEY BLOCK-----";
    const char *B1 = "mDMEWsN6MBYJKwYBBAHaRw8BAQdAAS+nkv9BdVi0JX7g6d+O201bdKhdowbielOo";
    const char *B2 = "ugCpCfi0CWVjYy0yNTUxOYiUBBMWCAA8AhsDBQsJCAcCAyICAQYVCgkICwIEFgID";
    const char *B3 = "AQIeAwIXgBYhBCH8aCdKrjtd45pCd8x4YniYGwcoBQJcVa/NAAoJEMx4YniYGwco";
    const char *B4 = "lFAA/jMt3RUUb5xt63JW6HFcrYq0RrDAcYMsXAY73iZpPsEcAQDmKbH21LkwoClU";
    const char *B5 = "9RrUJSYZnMla/pQdgOxd7/PjRCpbCg==";
    const char *CRC = "=miZp";
    const char *FTR = "-----END PGP PUBLIC KEY BLOCK-----";
    const char *FTR2 = "-----END PGP WEIRD KEY BLOCK-----";
    char        b64[1024];
    char        msg[1024];
    int         b64len = 0;
    int         len = 0;

    /* fill the body with normal \n line endings */
    b64len = snprintf(b64, sizeof(b64), "%s\n%s\n%s\n%s\n%s", B1, B2, B3, B4, B5);
    assert_true((b64len > 0) && (b64len < (int) sizeof(b64)));

    /* try normal message */
    len = snprintf(msg, sizeof(msg), "%s\n\n%s\n%s\n%s\n", HDR, b64, CRC, FTR);
    assert_true(try_dearmor(msg, len));

    /* no empty line after the headers, now accepted, see #1289 */
    len = snprintf(msg, sizeof(msg), "%s\n%s\n%s\n%s\n", HDR, b64, CRC, FTR);
    assert_true(try_dearmor(msg, len));

    /* \r\n line ending */
    len = snprintf(msg, sizeof(msg), "%s\r\n\r\n%s\r\n%s\r\n%s\r\n", HDR, b64, CRC, FTR);
    assert_true(try_dearmor(msg, len));

    /* mixed line ending */
    len = snprintf(msg, sizeof(msg), "%s\r\n\n%s\r\n%s\n%s\r\n", HDR, b64, CRC, FTR);
    assert_true(try_dearmor(msg, len));

    /* extra line before the footer */
    len = snprintf(msg, sizeof(msg), "%s\n\n%s\n%s\n\n%s\n", HDR, b64, CRC, FTR);
    assert_true(try_dearmor(msg, len));

    /* extra spaces after the header: allowed by RFC */
    len = snprintf(msg, sizeof(msg), "%s  \t  \n\n%s\n%s\n%s\n", HDR, b64, CRC, FTR);
    assert_true(try_dearmor(msg, len));

    /* extra spaces after the footer: allowed by RFC as well */
    len = snprintf(msg, sizeof(msg), "%s\n\n%s\n%s\n%s \n", HDR, b64, CRC, FTR);
    assert_true(try_dearmor(msg, len));
    len = snprintf(msg, sizeof(msg), "%s\n\n%s\n%s\n%s\t\n", HDR, b64, CRC, FTR);
    assert_true(try_dearmor(msg, len));
    len = snprintf(msg, sizeof(msg), "%s\n\n%s\n%s\n%s\t\t     \t\t \n", HDR, b64, CRC, FTR);
    assert_true(try_dearmor(msg, len));

    /* invalid footer */
    len = snprintf(msg, sizeof(msg), "%s\n\n%s\n%s\n%s\n", HDR, b64, CRC, FTR2);
    assert_false(try_dearmor(msg, len));

    /* extra spaces or tabs before the footer - allow it, see issue #2199 */
    len = snprintf(msg, sizeof(msg), "%s\n\n%s\n%s\n  %s\n", HDR, b64, CRC, FTR);
    assert_true(try_dearmor(msg, len));
    len = snprintf(msg, sizeof(msg), "%s\n\n%s\n%s\n\t\t %s\n", HDR, b64, CRC, FTR);
    assert_true(try_dearmor(msg, len));
    /* no empty line between crc and footer - FAIL */
    len = snprintf(msg, sizeof(msg), "%s\n\n%s\n%s%s\n", HDR, b64, CRC, FTR);
    assert_false(try_dearmor(msg, len));
    /* extra chars before the footer - FAIL */
    len = snprintf(msg, sizeof(msg), "%s\n\n%s\n%s\n11111%s\n", HDR, b64, CRC, FTR);
    assert_false(try_dearmor(msg, len));

    /* cut out or extended b64 padding */
    len = snprintf(msg, sizeof(msg), "%s\n\n%.*s\n%s\n%s\n", HDR, b64len - 1, b64, CRC, FTR);
    assert_false(try_dearmor(msg, len));
    len = snprintf(msg, sizeof(msg), "%s\n\n%.*s\n%s\n%s\n", HDR, b64len - 2, b64, CRC, FTR);
    assert_false(try_dearmor(msg, len));
    len = snprintf(msg, sizeof(msg), "%s\n\n%s==\n%s\n%s\n", HDR, b64, CRC, FTR);
    assert_false(try_dearmor(msg, len));

    /* invalid chars in b64 data */
    char old = b64[30];
    b64[30] = '?';
    len = snprintf(msg, sizeof(msg), "%s\n\n%s\n%s\n%s\n", HDR, b64, CRC, FTR);
    assert_false(try_dearmor(msg, len));
    b64[30] = old;

    /* modified/malformed crc (should be accepted now, see #1401) */
    len = snprintf(msg, sizeof(msg), "%s\n\n%s\n=miZq\n%s\n", HDR, b64, FTR);
    assert_true(try_dearmor(msg, len));
    len = snprintf(msg, sizeof(msg), "%s\n\n%s\nmiZp\n%s\n", HDR, b64, FTR);
    assert_false(try_dearmor(msg, len));
    len = snprintf(msg, sizeof(msg), "%s\n\n%s\n==miZp\n%s\n", HDR, b64, FTR);
    assert_false(try_dearmor(msg, len));
    len = snprintf(msg, sizeof(msg), "%s\n\n%s\n=miZpp\n%s\n", HDR, b64, FTR);
    assert_false(try_dearmor(msg, len));
    /* missing crc */
    len = snprintf(msg, sizeof(msg), "%s\n\n%s\n\n%s\n", HDR, b64, FTR);
    assert_true(try_dearmor(msg, len));
}

static void
add_openpgp_layers(
  const char *msg, pgp_dest_t &pgpdst, int compr, int encr, rnp::SecurityContext &global_ctx)
{
    pgp_source_t src = {};
    pgp_dest_t   dst = {};

    assert_rnp_success(init_mem_src(&src, msg, strlen(msg), false));
    assert_rnp_success(init_mem_dest(&dst, NULL, 0));
    assert_rnp_success(rnp_wrap_src(src, dst, "message.txt", time(NULL)));
    src.close();
    assert_rnp_success(init_mem_src(&src, mem_dest_own_memory(&dst), dst.writeb, true));
    dst_close(&dst, false);

    /* add compression layers */
    for (int i = 0; i < compr; i++) {
        pgp_compression_type_t alg = (pgp_compression_type_t)((i % 3) + 1);
        assert_rnp_success(init_mem_dest(&dst, NULL, 0));
        assert_rnp_success(rnp_compress_src(src, dst, alg, 9));
        src.close();
        assert_rnp_success(init_mem_src(&src, mem_dest_own_memory(&dst), dst.writeb, true));
        dst_close(&dst, false);
    }

    /* add encryption layers */
    for (int i = 0; i < encr; i++) {
        assert_rnp_success(init_mem_dest(&dst, NULL, 0));
        assert_rnp_success(rnp_raw_encrypt_src(src, dst, "password", global_ctx));
        src.close();
        assert_rnp_success(init_mem_src(&src, mem_dest_own_memory(&dst), dst.writeb, true));
        dst_close(&dst, false);
    }

    assert_rnp_success(init_mem_dest(&pgpdst, NULL, 0));
    assert_rnp_success(dst_write_src(&src, &pgpdst));
    src.close();
}

TEST_F(rnp_tests, test_stream_deep_packet_nesting)
{
    const char *message = "message";
    pgp_dest_t  dst = {};

    /* add 30 compression layers and 2 encryption - must fail */
    add_openpgp_layers(message, dst, 30, 2, global_ctx);
#ifdef DUMP_TEST_CASE
    /* remove ifdef if you want to write it to stdout */
    pgp_source_t src = {};
    assert_rnp_success(init_mem_src(&src, mem_dest_get_memory(&dst), dst.writeb, false));
    pgp_dest_t outdst = {};
    assert_rnp_success(init_stdout_dest(&outdst));
    assert_rnp_success(rnp_armor_source(&src, &outdst, PGP_ARMORED_MESSAGE));
    dst_close(&outdst, false);
    src.close();
#endif
    /* decrypt it via FFI for less code */
    rnp_ffi_t ffi = NULL;
    rnp_ffi_create(&ffi, "GPG", "GPG");
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));

    rnp_input_t input = NULL;
    assert_rnp_success(rnp_input_from_memory(
      &input, (const uint8_t *) mem_dest_get_memory(&dst), dst.writeb, false));
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_failure(rnp_decrypt(ffi, input, output));
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    dst_close(&dst, false);

    /* add  27 compression & 4 encryption layers - must succeed */
    add_openpgp_layers("message", dst, 27, 4, global_ctx);
#ifdef DUMP_TEST_CASE
    /* remove ifdef if you want to write it to stdout */
    assert_rnp_success(init_mem_src(&src, mem_dest_get_memory(&dst), dst.writeb, false));
    assert_rnp_success(init_stdout_dest(&outdst));
    assert_rnp_success(rnp_armor_source(&src, &outdst, PGP_ARMORED_MESSAGE));
    dst_close(&outdst, false);
    src.close();
#endif
    /* decrypt it via FFI for less code */
    assert_rnp_success(rnp_input_from_memory(
      &input, (const uint8_t *) mem_dest_get_memory(&dst), dst.writeb, false));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_decrypt(ffi, input, output));
    rnp_input_destroy(input);
    /* check output */
    uint8_t *buf = NULL;
    size_t   len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    assert_int_equal(strlen(message), len);
    assert_int_equal(memcmp(buf, message, len), 0);

    rnp_output_destroy(output);
    dst_close(&dst, false);

    rnp_ffi_destroy(ffi);
}

static bool
src_reader_generator(pgp_source_t *, void *buf, size_t len, size_t *read)
{
    *read = len;
    for (; len; buf = ((uint8_t *) buf) + 1, len--) {
        *(uint8_t *) buf = len & 0x7F;
    }
    return true;
}

TEST_F(rnp_tests, test_stream_cache)
{
    pgp_source_t src = {0};
    uint8_t      sample[sizeof(src.cache->buf)];
    size_t       samplesize = sizeof(sample);
    assert_true(src_reader_generator(NULL, sample, samplesize, &samplesize));
    assert_int_equal(sizeof(sample), samplesize);

    init_src_common(&src, 0);
    int8_t *buf = (int8_t *) src.cache->buf;
    src.raw_read = src_reader_generator;
    size_t len = sizeof(src.cache->buf);

    // empty cache, pos=0
    memset(src.cache->buf, 0xFF, len);
    src.cache->pos = 0;
    src.cache->len = 0;
    assert_true(src.peek_eq(NULL, len));
    assert_false(memcmp(buf, sample, samplesize));

    // empty cache, pos is somewhere in the middle
    memset(src.cache->buf, 0xFF, len);
    src.cache->pos = 100;
    src.cache->len = 100;
    assert_true(src.peek_eq(NULL, len));
    assert_false(memcmp(buf, sample, samplesize));

    // empty cache, pos=max
    memset(src.cache->buf, 0xFF, len);
    src.cache->pos = len;
    src.cache->len = len;
    assert_true(src.peek_eq(NULL, len));
    assert_false(memcmp(buf, sample, samplesize));

    // cache has some data in the middle
    src.cache->pos = 128; // sample boundary
    src.cache->len = 300;
    memset(src.cache->buf, 0xFF, src.cache->pos);
    memset(src.cache->buf + src.cache->len, 0xFF, len - src.cache->len);
    assert_true(src.peek_eq(NULL, len));
    assert_false(memcmp(buf, sample, samplesize));

    // cache has some data starting from pos until the end
    src.cache->pos = 128; // sample boundary
    src.cache->len = len;
    memset(src.cache->buf, 0xFF, src.cache->pos);
    assert_true(src.peek_eq(NULL, len));
    assert_false(memcmp(buf, sample, samplesize));

    // cache is almost full
    src.cache->pos = 0;
    src.cache->len = len - 1;
    src.cache->buf[len - 1] = 0xFF;
    assert_true(src.peek_eq(NULL, len));
    assert_false(memcmp(buf, sample, samplesize));

    // cache is full
    src.cache->pos = 0;
    src.cache->len = len;
    memset(src.cache->buf, 0xFF, src.cache->pos);
    memset(src.cache->buf + src.cache->len, 0xFF, len - src.cache->len);
    assert_true(src.peek_eq(NULL, len));
    assert_false(memcmp(buf, sample, samplesize));

    src.close();
}
