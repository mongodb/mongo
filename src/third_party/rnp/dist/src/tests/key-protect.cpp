/*
 * Copyright (c) 2017-2019 [Ribose Inc](https://www.ribose.com).
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

#include "key.hpp"

#include "rnp_tests.h"
#include "support.h"
#include "keygen.hpp"

bool
rsa_sec_empty(const pgp::KeyMaterial &key)
{
    auto &rsa = dynamic_cast<const pgp::RSAKeyMaterial &>(key);
    return mpi_empty(rsa.d()) && mpi_empty(rsa.p()) && mpi_empty(rsa.q()) &&
           mpi_empty(rsa.u());
}

bool
rsa_sec_filled(const pgp::KeyMaterial &key)
{
    auto &rsa = dynamic_cast<const pgp::RSAKeyMaterial &>(key);
    return !mpi_empty(rsa.d()) && !mpi_empty(rsa.p()) && !mpi_empty(rsa.q()) &&
           !mpi_empty(rsa.u());
}

/* This test loads a .gpg keyring and tests protect/unprotect functionality.
 * There is also some lock/unlock testing in here, since the two are
 * somewhat related.
 */
TEST_F(rnp_tests, test_key_protect_load_pgp)
{
    rnp::Key *         key = nullptr;
    static const char *keyids[] = {"7bc6709b15c23a4a", // primary
                                   "1ed63ee56fadc34d",
                                   "1d7e8a5393c997a8",
                                   "8a05b89fad5aded1",
                                   "2fcadf05ffa501bb", // primary
                                   "54505a936a4a970e",
                                   "326ef111425d14a5"};

    // load our keyring and do some quick checks
    {
        pgp_source_t src = {};
        auto         ks = new rnp::KeyStore(global_ctx);

        assert_rnp_success(init_file_src(&src, "data/keyrings/1/secring.gpg"));
        assert_rnp_success(ks->load_pgp(src));
        src.close();

        for (size_t i = 0; i < ARRAY_SIZE(keyids); i++) {
            rnp::Key *tmp = rnp_tests_get_key_by_id(ks, keyids[i]);
            assert_non_null(tmp);
            // all keys in this keyring are encrypted and thus should be both protected and
            // locked initially
            assert_true(tmp->is_protected());
            assert_true(tmp->is_locked());
        }

        rnp::Key *tmp = rnp_tests_get_key_by_id(ks, keyids[0]);
        assert_non_null(tmp);

        // steal this key from the store
        key = new rnp::Key(*tmp);
        assert_non_null(key);
        delete ks;
    }

    // confirm that this key is indeed RSA
    assert_int_equal(key->alg(), PGP_PKA_RSA);

    // confirm key material is currently all NULL (in other words, the key is locked)
    assert_true(rsa_sec_empty(*key->material()));

    // try to unprotect with a failing password provider
    pgp_password_provider_t pprov(failing_password_callback);
    assert_false(key->unprotect(pprov, global_ctx));

    // try to unprotect with an incorrect password
    pprov = {string_copy_password_callback, (void *) "badpass"};
    assert_false(key->unprotect(pprov, global_ctx));

    // unprotect with the correct password
    pprov = {string_copy_password_callback, (void *) "password"};
    assert_true(key->unprotect(pprov, global_ctx));
    assert_false(key->is_protected());

    // should still be locked
    assert_true(key->is_locked());

    // confirm secret key material is still NULL
    assert_true(rsa_sec_empty(*key->material()));

    // unlock (no password required since the key is not protected)
    pprov = {asserting_password_callback};
    assert_true(key->unlock(pprov));
    assert_false(key->is_locked());

    // secret key material should be available
    assert_true(rsa_sec_filled(*key->material()));

    // save the secret MPIs for some later comparisons
    auto &rsa = dynamic_cast<const pgp::RSAKeyMaterial &>(*key->material());
    auto  d = rsa.d();
    auto  p = rsa.p();
    auto  q = rsa.q();
    auto  u = rsa.u();

    // confirm that packets[0] is no longer encrypted
    {
        auto              ks = new rnp::KeyStore(global_ctx);
        rnp::MemorySource memsrc(key->rawpkt().data());

        assert_rnp_success(ks->load_pgp(memsrc.src()));

        // grab the first key
        rnp::Key *reloaded_key = NULL;
        assert_non_null(reloaded_key = rnp_tests_get_key_by_id(ks, keyids[0]));
        assert_non_null(reloaded_key);

        // should not be locked, nor protected
        assert_false(reloaded_key->is_locked());
        assert_false(reloaded_key->is_protected());
        // secret key material should not be NULL
        auto &rsar = dynamic_cast<const pgp::RSAKeyMaterial &>(*reloaded_key->material());
        assert_false(mpi_empty(rsar.d()));
        assert_false(mpi_empty(rsar.p()));
        assert_false(mpi_empty(rsar.q()));
        assert_false(mpi_empty(rsar.u()));

        // compare MPIs of the reloaded key, with the unlocked key from earlier
        assert_true(rsa.d() == rsar.d());
        assert_true(rsa.p() == rsar.p());
        assert_true(rsa.q() == rsar.q());
        assert_true(rsa.u() == rsar.u());
        // negative test to try to ensure the above is a valid test
        assert_false(rsa.d() == rsar.p());

        // lock it
        assert_true(reloaded_key->lock());
        assert_true(reloaded_key->is_locked());
        // confirm that secret MPIs are NULL again
        assert_true(rsa_sec_empty(*reloaded_key->material()));
        // unlock it (no password, since it's not protected)
        pgp_password_provider_t pprov(asserting_password_callback);
        assert_true(reloaded_key->unlock(pprov));
        assert_false(reloaded_key->is_locked());
        // compare MPIs of the reloaded key, with the unlocked key from earlier
        auto &rsar2 = dynamic_cast<const pgp::RSAKeyMaterial &>(*reloaded_key->material());
        assert_true(rsa.d() == rsar2.d());
        assert_true(rsa.p() == rsar2.p());
        assert_true(rsa.q() == rsar2.q());
        assert_true(rsa.u() == rsar2.u());

        delete ks;
    }

    // lock
    assert_true(key->lock());

    // try to protect (will fail when key is locked)
    pprov = {string_copy_password_callback, (void *) "newpass"};
    assert_false(key->protect({}, pprov, global_ctx));
    assert_false(key->is_protected());

    // unlock
    pprov = {asserting_password_callback};
    assert_true(key->unlock(pprov));
    assert_false(key->is_locked());

    // try to protect with a failing password provider
    pprov = {failing_password_callback};
    assert_false(key->protect({}, pprov, global_ctx));
    assert_false(key->is_protected());

    // (re)protect with a new password
    pprov = {string_copy_password_callback, (void *) "newpass"};
    assert_true(key->protect({}, pprov, global_ctx));
    assert_true(key->is_protected());

    // lock
    assert_true(key->lock());
    assert_true(key->is_locked());

    // try to unlock with old password
    pprov = {string_copy_password_callback, (void *) "password"};
    assert_false(key->unlock(pprov));
    assert_true(key->is_locked());

    // unlock with new password
    pprov = {string_copy_password_callback, (void *) "newpass"};
    assert_true(key->unlock(pprov));
    assert_false(key->is_locked());

    // compare secret MPIs with those from earlier
    auto &rsa2 = dynamic_cast<const pgp::RSAKeyMaterial &>(*key->material());
    assert_true(rsa2.d() == d);
    assert_true(rsa2.p() == p);
    assert_true(rsa2.q() == q);
    assert_true(rsa2.u() == u);

    // cleanup
    delete key;
}

TEST_F(rnp_tests, test_key_protect_sec_data)
{
    rnp::KeygenParams keygen(PGP_PKA_RSA, global_ctx);
    auto &            rsa = dynamic_cast<pgp::RSAKeyParams &>(keygen.key_params());
    rsa.set_bits(1024);

    rnp::CertParams cert;
    cert.userid = "test";

    rnp::BindingParams binding;

    /* generate raw unprotected keypair */
    rnp::Key                skey, pkey, ssub, psub;
    pgp_password_provider_t prov = {};
    assert_true(keygen.generate(cert, skey, pkey));
    assert_true(keygen.generate(binding, skey, pkey, ssub, psub, prov));
    assert_false(skey.pkt().sec_data.empty());
    assert_false(ssub.pkt().sec_data.empty());
    assert_true(pkey.pkt().sec_data.empty());
    assert_true(psub.pkt().sec_data.empty());
    /* copy part of the cleartext secret key and save pointers for later checks */
    assert_true(skey.pkt().sec_data.size() >= 32);
    assert_true(ssub.pkt().sec_data.size() >= 32);
    uint8_t raw_skey[32];
    uint8_t raw_ssub[32];
    memcpy(raw_skey, skey.pkt().sec_data.data(), 32);
    memcpy(raw_ssub, ssub.pkt().sec_data.data(), 32);
    pgp_key_pkt_t *skeypkt;
    pgp_key_pkt_t *ssubpkt;
    /* protect key and subkey */
    pgp_password_provider_t     pprov(string_copy_password_callback, (void *) "password");
    rnp_key_protection_params_t prot = {};
    assert_true(skey.protect(prot, pprov, global_ctx));
    assert_true(ssub.protect(prot, pprov, global_ctx));
    assert_int_not_equal(memcmp(raw_skey, skey.pkt().sec_data.data(), 32), 0);
    assert_int_not_equal(memcmp(raw_ssub, ssub.pkt().sec_data.data(), 32), 0);
    /* make sure rawpkt is also protected */
    skeypkt = new pgp_key_pkt_t();
    rnp::MemorySource skeysrc(skey.rawpkt().data());
    assert_rnp_success(skeypkt->parse(skeysrc.src()));
    assert_int_not_equal(memcmp(raw_skey, skeypkt->sec_data.data(), 32), 0);
    assert_int_equal(skeypkt->sec_protection.s2k.specifier, PGP_S2KS_ITERATED_AND_SALTED);
    delete skeypkt;
    ssubpkt = new pgp_key_pkt_t();
    rnp::MemorySource ssubsrc(ssub.rawpkt().data());
    assert_rnp_success(ssubpkt->parse(ssubsrc.src()));
    assert_int_not_equal(memcmp(raw_ssub, ssubpkt->sec_data.data(), 32), 0);
    assert_int_equal(ssubpkt->sec_protection.s2k.specifier, PGP_S2KS_ITERATED_AND_SALTED);
    delete ssubpkt;

    /* unlock and make sure sec_data is not decrypted */
    assert_true(skey.unlock(pprov));
    assert_true(ssub.unlock(pprov));
    assert_int_not_equal(memcmp(raw_skey, skey.pkt().sec_data.data(), 32), 0);
    assert_int_not_equal(memcmp(raw_ssub, ssub.pkt().sec_data.data(), 32), 0);
    /* unprotect key */
    assert_true(skey.unprotect(pprov, global_ctx));
    assert_true(ssub.unprotect(pprov, global_ctx));
    assert_int_equal(memcmp(raw_skey, skey.pkt().sec_data.data(), 32), 0);
    assert_int_equal(memcmp(raw_ssub, ssub.pkt().sec_data.data(), 32), 0);
    /* protect it back  with another password */
    pgp_password_provider_t pprov2(string_copy_password_callback, (void *) "password2");
    assert_true(skey.protect(prot, pprov2, global_ctx));
    assert_true(ssub.protect(prot, pprov2, global_ctx));
    assert_int_not_equal(memcmp(raw_skey, skey.pkt().sec_data.data(), 32), 0);
    assert_int_not_equal(memcmp(raw_ssub, ssub.pkt().sec_data.data(), 32), 0);
    assert_false(skey.unlock(pprov));
    assert_false(ssub.unlock(pprov));
    assert_true(skey.unlock(pprov2));
    assert_true(ssub.unlock(pprov2));
    assert_true(skey.lock());
    assert_true(ssub.lock());
    /* make sure rawpkt is also protected */
    skeypkt = new pgp_key_pkt_t();
    rnp::MemorySource skey2src(skey.rawpkt().data());
    assert_rnp_success(skeypkt->parse(skey2src.src()));
    assert_int_not_equal(memcmp(raw_skey, skeypkt->sec_data.data(), 32), 0);
    assert_int_equal(skeypkt->sec_protection.s2k.specifier, PGP_S2KS_ITERATED_AND_SALTED);
    delete skeypkt;
    ssubpkt = new pgp_key_pkt_t();
    rnp::MemorySource ssub2src(ssub.rawpkt().data());
    assert_rnp_success(ssubpkt->parse(ssub2src.src()));
    assert_int_not_equal(memcmp(raw_ssub, ssubpkt->sec_data.data(), 32), 0);
    assert_int_equal(ssubpkt->sec_protection.s2k.specifier, PGP_S2KS_ITERATED_AND_SALTED);
    delete ssubpkt;
}
