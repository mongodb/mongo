/*
 * Copyright (c) 2018-2019 [Ribose Inc](https://www.ribose.com).
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
#include "../librepgp/stream-packet.h"
#include "../librepgp/stream-armor.h"
#include "keygen.hpp"

static bool
all_keys_valid(const rnp::KeyStore *keyring, rnp::Key *except = NULL)
{
    char keyid[PGP_KEY_ID_SIZE * 2 + 3] = {0};

    for (auto &key : keyring->keys) {
        if ((!key.valid() || key.expired()) && (&key != except)) {
            if (!rnp::hex_encode(key.keyid().data(),
                                 key.keyid().size(),
                                 keyid,
                                 sizeof(keyid),
                                 rnp::HexFormat::Lowercase)) {
                throw std::exception();
            }
            RNP_LOG("key %s is not valid", keyid);
            return false;
        }
    }
    return true;
}

TEST_F(rnp_tests, test_key_validate)
{
    rnp::Key *key = nullptr;

    auto pubring = new rnp::KeyStore("data/keyrings/1/pubring.gpg", global_ctx);
    assert_true(pubring->load());
    /* this keyring has one expired subkey */
    assert_non_null(key = rnp_tests_get_key_by_id(pubring, "1d7e8a5393c997a8"));
    assert_false(key->valid());
    assert_true(key->expired());
    assert_true(all_keys_valid(pubring, key));
    delete pubring;

    /* secret key is marked is expired as well */
    auto secring = new rnp::KeyStore("data/keyrings/1/secring.gpg", global_ctx);
    assert_true(secring->load());
    assert_non_null(key = rnp_tests_get_key_by_id(secring, "1d7e8a5393c997a8"));
    assert_false(key->valid());
    assert_true(key->expired());
    assert_true(all_keys_valid(secring, key));
    delete secring;

    pubring = new rnp::KeyStore("data/keyrings/2/pubring.gpg", global_ctx);
    assert_true(pubring->load());
    assert_true(all_keys_valid(pubring));

    /* secret keyring doesn't have signatures - so keys are marked as invalid */
    secring = new rnp::KeyStore("data/keyrings/2/secring.gpg", global_ctx);
    assert_true(secring->load());
    assert_false(all_keys_valid(secring));
    /* but after adding signatures from public it is marked as valid */
    assert_non_null(key = rnp_tests_get_key_by_id(pubring, "dc70c124a50283f1"));
    assert_non_null(secring->import_key(*key, true));
    assert_true(all_keys_valid(secring));
    delete pubring;
    delete secring;

    pubring =
      new rnp::KeyStore("data/keyrings/3/pubring.kbx", global_ctx, rnp::KeyFormat::KBX);
    assert_true(pubring->load());
    assert_true(all_keys_valid(pubring));

    secring =
      new rnp::KeyStore("data/keyrings/3/private-keys-v1.d", global_ctx, rnp::KeyFormat::G10);
    rnp::KeyProvider key_provider(rnp_key_provider_store, pubring);
    assert_true(secring->load(&key_provider));
    assert_true(all_keys_valid(secring));
    delete pubring;
    delete secring;

    pubring = new rnp::KeyStore("data/keyrings/4/pubring.pgp", global_ctx);
    assert_true(pubring->load());
    /* certification has signature with MD5 hash algorithm */
    assert_false(all_keys_valid(pubring));

    pubring->clear();
    /* add rule which allows MD5 */
    rnp::SecurityRule allow_md5(
      rnp::FeatureType::Hash, PGP_HASH_MD5, rnp::SecurityLevel::Default);
    allow_md5.override = true;
    global_ctx.profile.add_rule(allow_md5);
    assert_true(pubring->load());
    assert_true(all_keys_valid(pubring));
    pubring->clear();
    /* remove rule */
    assert_true(global_ctx.profile.del_rule(allow_md5));
    assert_true(pubring->load());
    assert_false(all_keys_valid(pubring));
    delete pubring;

    /* secret keyring doesn't have certifications - so marked as invalid */
    secring = new rnp::KeyStore("data/keyrings/4/secring.pgp", global_ctx);
    assert_true(secring->load());
    assert_false(all_keys_valid(secring));
    delete secring;

    pubring = new rnp::KeyStore("data/keyrings/5/pubring.gpg", global_ctx);
    assert_true(pubring->load());
    assert_true(all_keys_valid(pubring));
    delete pubring;

    secring = new rnp::KeyStore("data/keyrings/5/secring.gpg", global_ctx);
    assert_true(secring->load());
    assert_true(all_keys_valid(secring));
    delete secring;
}

#define DATA_PATH "data/test_forged_keys/"

static void
key_store_add(rnp::KeyStore *keyring, const char *keypath)
{
    pgp_source_t           keysrc = {};
    pgp_transferable_key_t tkey = {};

    assert_rnp_success(init_file_src(&keysrc, keypath));
    assert_rnp_success(process_pgp_key(keysrc, tkey, false));
    assert_true(keyring->add_ts_key(tkey));
    keysrc.close();
}

static bool
key_check(rnp::KeyStore *keyring, const std::string &keyid, bool valid, bool expired = false)
{
    rnp::Key *key = rnp_tests_get_key_by_id(keyring, keyid);
    return key && (key->validated()) && (key->valid() == valid) && (key->expired() == expired);
}

TEST_F(rnp_tests, test_forged_key_validate)
{
    auto pubring = new rnp::KeyStore("", global_ctx);

    /* load valid dsa-eg key */
    key_store_add(pubring, DATA_PATH "dsa-eg-pub.pgp");
    assert_true(key_check(pubring, "C8A10A7D78273E10", true));
    pubring->clear();

    /* load dsa-eg key with forged self-signature and binding. Subkey will not be valid as
     * well. */
    key_store_add(pubring, DATA_PATH "dsa-eg-pub-forged-key.pgp");
    assert_true(key_check(pubring, "C8A10A7D78273E10", false));
    assert_true(key_check(pubring, "02A5715C3537717E", false));
    pubring->clear();

    /* load dsa-eg key with forged key material */
    key_store_add(pubring, DATA_PATH "dsa-eg-pub-forged-material.pgp");
    rnp::Key *key = rnp_tests_get_key_by_id(pubring, "C8A10A7D78273E10");
    assert_null(key);
    /* malformed key material causes keyid change */
    key = rnp_tests_get_key_by_id(pubring, "C258AB3B54097B9B");
    assert_non_null(key);
    assert_false(key->valid());
    assert_false(key->expired());
    pubring->clear();

    /* load dsa-eg keypair with forged subkey binding signature */
    key_store_add(pubring, DATA_PATH "dsa-eg-pub-forged-subkey.pgp");
    assert_true(key_check(pubring, "02A5715C3537717E", false));
    assert_true(key_check(pubring, "C8A10A7D78273E10", true));
    pubring->clear();

    /* load valid eddsa key */
    key_store_add(pubring, DATA_PATH "ecc-25519-pub.pgp");
    assert_true(key_check(pubring, "CC786278981B0728", true));
    pubring->clear();

    /* load eddsa key with forged self-signature */
    key_store_add(pubring, DATA_PATH "ecc-25519-pub-forged-key.pgp");
    assert_true(key_check(pubring, "CC786278981B0728", false));
    pubring->clear();

    /* load eddsa key with forged key material */
    key_store_add(pubring, DATA_PATH "ecc-25519-pub-forged-material.pgp");
    key = rnp_tests_get_key_by_id(pubring, "1BEF78DF765B79A2");
    assert_non_null(key);
    assert_false(key->valid());
    assert_false(key->expired());
    pubring->clear();

    /* load valid ecdsa/ecdh p-256 keypair */
    key_store_add(pubring, DATA_PATH "ecc-p256-pub.pgp");
    assert_true(key_check(pubring, "23674F21B2441527", true));
    assert_true(key_check(pubring, "37E285E9E9851491", true));
    pubring->clear();

    /* load ecdsa/ecdh key with forged self-signature. Both valid since there is valid binding.
     */
    key_store_add(pubring, DATA_PATH "ecc-p256-pub-forged-key.pgp");
    assert_true(key_check(pubring, "23674F21B2441527", true));
    assert_true(key_check(pubring, "37E285E9E9851491", true));
    pubring->clear();

    /* load ecdsa/ecdh key with forged key material. Subkey is not valid as well. */
    key_store_add(pubring, DATA_PATH "ecc-p256-pub-forged-material.pgp");
    key = rnp_tests_get_key_by_id(pubring, "23674F21B2441527");
    assert_null(key);
    key = rnp_tests_get_key_by_id(pubring, "41DEA786D18E5184");
    assert_non_null(key);
    assert_false(key->valid());
    assert_false(key->expired());
    assert_true(key_check(pubring, "37E285E9E9851491", false));
    pubring->clear();

    /* load ecdsa/ecdh keypair with forged subkey binding signature */
    key_store_add(pubring, DATA_PATH "ecc-p256-pub-forged-subkey.pgp");
    assert_true(key_check(pubring, "37E285E9E9851491", false));
    assert_true(key_check(pubring, "23674F21B2441527", true));
    pubring->clear();

    /* load ecdsa/ecdh keypair without certification: valid since have binding */
    key_store_add(pubring, DATA_PATH "ecc-p256-pub-no-certification.pgp");
    assert_true(key_check(pubring, "23674F21B2441527", true));
    assert_true(key_check(pubring, "37E285E9E9851491", true));
    pubring->clear();

    /* load ecdsa/ecdh keypair without certification and invalid binding */
    key_store_add(pubring, DATA_PATH "ecc-p256-pub-no-cert-malf-binding.pgp");
    assert_true(key_check(pubring, "23674F21B2441527", false));
    assert_true(key_check(pubring, "37E285E9E9851491", false));
    pubring->clear();

    /* load ecdsa/ecdh keypair without subkey binding */
    key_store_add(pubring, DATA_PATH "ecc-p256-pub-no-binding.pgp");
    assert_true(key_check(pubring, "23674F21B2441527", true));
    assert_true(key_check(pubring, "37E285E9E9851491", false));
    pubring->clear();

    /* load valid rsa/rsa keypair */
    key_store_add(pubring, DATA_PATH "rsa-rsa-pub.pgp");
    /* it is valid only till year 2024 since SHA1 hash is used for signatures */
    assert_true(key_check(pubring, "2FB9179118898E8B", true));
    assert_true(key_check(pubring, "6E2F73008F8B8D6E", true));
    pubring->clear();
    /* load eddsa key which uses SHA1 signature and is created after the cutoff date */
    global_ctx.set_time(SHA1_KEY_FROM + 10);
    key_store_add(pubring, DATA_PATH "eddsa-2024-pub.pgp");
    assert_false(key_check(pubring, "980E3741F632212C", true));
    assert_false(key_check(pubring, "6DA00BF7F8B59B53", true));
    global_ctx.set_time(0);
    pubring->clear();

    /* load rsa/rsa key with forged self-signature. Valid because of valid binding. */
    key_store_add(pubring, DATA_PATH "rsa-rsa-pub-forged-key.pgp");
    assert_true(key_check(pubring, "2FB9179118898E8B", true));
    assert_true(key_check(pubring, "6E2F73008F8B8D6E", true));
    pubring->clear();

    /* load rsa/rsa key with forged key material. Subkey is not valid as well. */
    key_store_add(pubring, DATA_PATH "rsa-rsa-pub-forged-material.pgp");
    key = rnp_tests_get_key_by_id(pubring, "2FB9179118898E8B");
    assert_null(key);
    key = rnp_tests_get_key_by_id(pubring, "791B14952D8F906C");
    assert_non_null(key);
    assert_false(key->valid());
    assert_false(key->expired());
    assert_true(key_check(pubring, "6E2F73008F8B8D6E", false));
    pubring->clear();

    /* load rsa/rsa keypair with forged subkey binding signature */
    key_store_add(pubring, DATA_PATH "rsa-rsa-pub-forged-subkey.pgp");
    assert_true(key_check(pubring, "2FB9179118898E8B", true));
    assert_true(key_check(pubring, "6E2F73008F8B8D6E", false));
    pubring->clear();

    /* load rsa/rsa keypair with future creation date */
    key_store_add(pubring, DATA_PATH "rsa-rsa-pub-future-key.pgp");
    assert_true(key_check(pubring, "3D032D00EE1EC3F5", false));
    assert_true(key_check(pubring, "021085B640CE8DCE", false));
    pubring->clear();

    /* load eddsa/rsa keypair with certification with future creation date - valid because of
     * binding. */
    key_store_add(pubring, DATA_PATH "ecc-25519-pub-future-cert.pgp");
    assert_true(key_check(pubring, "D3B746FA852C2BE8", true));
    assert_true(key_check(pubring, "EB8C21ACDC15CA14", true));
    pubring->clear();

    /* load eddsa/rsa keypair with certification with future creation date - invalid because of
     * invalid binding. */
    key_store_add(pubring, DATA_PATH "ecc-25519-pub-future-cert-malf-bind.pgp");
    assert_true(key_check(pubring, "D3B746FA852C2BE8", false));
    assert_true(key_check(pubring, "EB8C21ACDC15CA14", false));
    pubring->clear();

    /* load ecdsa/rsa keypair with expired subkey */
    key_store_add(pubring, DATA_PATH "ecc-p256-pub-expired-subkey.pgp");
    assert_true(key_check(pubring, "23674F21B2441527", true));
    assert_true(key_check(pubring, "37E285E9E9851491", false, true));
    pubring->clear();

    /* load ecdsa/ecdh keypair with expired key */
    key_store_add(pubring, DATA_PATH "ecc-p256-pub-expired-key.pgp");
    assert_true(key_check(pubring, "23674F21B2441527", false, true));
    assert_true(key_check(pubring, "37E285E9E9851491", false));
    pubring->clear();

    delete pubring;
}

#define KEYSIG_PATH "data/test_key_validity/"

TEST_F(rnp_tests, test_key_validity)
{
    /* Case1:
     * Keys: Alice [pub]
     * Alice is signed by Basil, but without the Basil's key.
     * Result: Alice [valid]
     */
    auto pubring = new rnp::KeyStore(KEYSIG_PATH "case1/pubring.gpg", global_ctx);
    assert_true(pubring->load());
    rnp::Key *key = nullptr;
    assert_non_null(key = rnp_tests_key_search(pubring, "Alice <alice@rnp>"));
    assert_true(key->valid());
    assert_false(key->expired());
    delete pubring;

    /* Case2:
     * Keys: Alice [pub], Basil [pub]
     * Alice is signed by Basil, Basil is signed by Alice, but Alice's self-signature is
     * corrupted.
     * Result: Alice [invalid], Basil [valid]
     */
    pubring = new rnp::KeyStore(KEYSIG_PATH "case2/pubring.gpg", global_ctx);
    assert_true(pubring->load());
    assert_non_null(key = rnp_tests_get_key_by_id(pubring, "0451409669FFDE3C"));
    assert_false(key->valid());
    assert_false(key->expired());
    assert_non_null(key = rnp_tests_key_search(pubring, "Basil <basil@rnp>"));
    assert_true(key->valid());
    assert_false(key->expired());
    delete pubring;

    /* Case3:
     * Keys: Alice [pub], Basil [pub]
     * Alice is signed by Basil, but doesn't have self-signature
     * Result: Alice [invalid]
     */
    pubring = new rnp::KeyStore(KEYSIG_PATH "case3/pubring.gpg", global_ctx);
    assert_true(pubring->load());
    assert_non_null(key = rnp_tests_get_key_by_id(pubring, "0451409669FFDE3C"));
    assert_false(key->valid());
    assert_false(key->expired());
    assert_non_null(key = rnp_tests_key_search(pubring, "Basil <basil@rnp>"));
    assert_true(key->valid());
    assert_false(key->expired());
    delete pubring;

    /* Case4:
     * Keys Alice [pub, sub]
     * Alice subkey has invalid binding signature
     * Result: Alice [valid], Alice sub [invalid]
     */
    pubring = new rnp::KeyStore(KEYSIG_PATH "case4/pubring.gpg", global_ctx);
    assert_true(pubring->load());
    assert_non_null(key = rnp_tests_key_search(pubring, "Alice <alice@rnp>"));
    assert_true(key->valid());
    assert_false(key->expired());
    assert_int_equal(key->subkey_count(), 1);
    rnp::Key *subkey = nullptr;
    assert_non_null(subkey = pubring->get_subkey(*key, 0));
    assert_false(subkey->valid());
    assert_false(subkey->expired());
    delete pubring;

    /* Case5:
     * Keys Alice [pub, sub], Basil [pub]
     * Alice subkey has valid binding signature, but from the key Basil
     * Result: Alice [valid], Alice sub [invalid]
     *
     * Note: to re-generate keyring file, use generate.cpp from case5 folder.
     *       To build it, feed -DBUILD_TESTING_GENERATORS=On to the cmake.
     */
    pubring = new rnp::KeyStore(KEYSIG_PATH "case5/pubring.gpg", global_ctx);
    assert_true(pubring->load());
    assert_non_null(key = rnp_tests_key_search(pubring, "Alice <alice@rnp>"));
    assert_true(key->valid());
    assert_false(key->expired());
    assert_int_equal(key->subkey_count(), 1);
    assert_non_null(subkey = pubring->get_subkey(*key, 0));
    assert_false(subkey->valid());
    assert_false(subkey->expired());
    delete pubring;

    /* Case6:
     * Keys Alice [pub, sub]
     * Key Alice has revocation signature by Alice, and subkey doesn't
     * Result: Alice [invalid], Alice sub [invalid]
     */
    pubring = new rnp::KeyStore(KEYSIG_PATH "case6/pubring.gpg", global_ctx);
    assert_true(pubring->load());
    assert_non_null(key = rnp_tests_key_search(pubring, "Alice <alice@rnp>"));
    assert_false(key->valid());
    assert_false(key->expired());
    assert_true(key->revoked());
    assert_int_equal(key->subkey_count(), 1);
    assert_non_null(subkey = pubring->get_subkey(*key, 0));
    assert_false(subkey->valid());
    assert_false(subkey->expired());
    assert_false(subkey->revoked());
    delete pubring;

    /* Case7:
     * Keys Alice [pub, sub]
     * Alice subkey has revocation signature by Alice
     * Result: Alice [valid], Alice sub [invalid]
     */
    pubring = new rnp::KeyStore(KEYSIG_PATH "case7/pubring.gpg", global_ctx);
    assert_true(pubring->load());
    assert_non_null(key = rnp_tests_key_search(pubring, "Alice <alice@rnp>"));
    assert_true(key->valid());
    assert_false(key->expired());
    assert_false(key->revoked());
    assert_int_equal(key->subkey_count(), 1);
    assert_non_null(subkey = pubring->get_subkey(*key, 0));
    assert_false(subkey->valid());
    assert_false(subkey->expired());
    assert_true(subkey->revoked());
    delete pubring;

    /* Case8:
     * Keys Alice [pub, sub]
     * Userid is stripped from the key, but it still has valid subkey binding
     * Result: Alice [valid], Alice sub[valid]
     */
    pubring = new rnp::KeyStore(KEYSIG_PATH "case8/pubring.gpg", global_ctx);
    assert_true(pubring->load());
    assert_non_null(key = rnp_tests_get_key_by_id(pubring, "0451409669FFDE3C"));
    assert_true(key->valid());
    assert_int_equal(key->subkey_count(), 1);
    assert_non_null(subkey = pubring->get_subkey(*key, 0));
    assert_true(subkey->valid());
    delete pubring;

    /* Case9:
     * Keys Alice [pub, sub]
     * Alice key has two self-signatures, one which expires key and second without key
     * expiration.
     * Result: Alice [valid], Alice sub[valid]
     */
    pubring = new rnp::KeyStore(KEYSIG_PATH "case9/pubring.gpg", global_ctx);
    assert_non_null(pubring);
    assert_true(pubring->load());
    assert_non_null(key = rnp_tests_get_key_by_id(pubring, "0451409669FFDE3C"));
    assert_true(key->valid());
    assert_false(key->expired());
    assert_int_equal(key->subkey_count(), 1);
    assert_non_null(subkey = pubring->get_subkey(*key, 0));
    assert_true(subkey->valid());
    assert_false(subkey->expired());
    delete pubring;

    /* Case10:
     * Keys Alice [pub, sub]
     * Alice key has expiring direct-key signature and non-expiring self-certification.
     * Result: Alice [invalid], Alice sub[invalid]
     */
    pubring = new rnp::KeyStore(KEYSIG_PATH "case10/pubring.gpg", global_ctx);
    assert_non_null(pubring);
    assert_true(pubring->load());
    assert_non_null(key = rnp_tests_get_key_by_id(pubring, "0451409669FFDE3C"));
    assert_false(key->valid());
    assert_true(key->expired());
    assert_int_equal(key->subkey_count(), 1);
    assert_non_null(subkey = pubring->get_subkey(*key, 0));
    assert_false(subkey->valid());
    assert_false(subkey->expired());
    delete pubring;

    /* Case11:
     * Keys Alice [pub, sub]
     * Alice key has expiring direct-key signature, non-expiring self-certification and
     * expiring primary userid certification. Result: Alice [invalid], Alice sub[invalid]
     */
    pubring = new rnp::KeyStore(KEYSIG_PATH "case11/pubring.gpg", global_ctx);
    assert_non_null(pubring);
    assert_true(pubring->load());
    assert_non_null(key = rnp_tests_get_key_by_id(pubring, "0451409669FFDE3C"));
    assert_false(key->valid());
    assert_true(key->expired());
    assert_int_equal(key->expiration(), 100);
    assert_int_equal(key->subkey_count(), 1);
    assert_non_null(subkey = pubring->get_subkey(*key, 0));
    assert_false(subkey->valid());
    assert_false(subkey->expired());
    delete pubring;

    /* Case12:
     * Keys Alice [pub, sub]
     * Alice key has non-expiring direct-key signature, non-expiring self-certification and
     * expiring primary userid certification. Result: Alice [invalid], Alice sub[invalid]
     */
    pubring = new rnp::KeyStore(KEYSIG_PATH "case12/pubring.gpg", global_ctx);
    assert_non_null(pubring);
    assert_true(pubring->load());
    assert_non_null(key = rnp_tests_get_key_by_id(pubring, "0451409669FFDE3C"));
    assert_false(key->valid());
    assert_true(key->expired());
    assert_int_equal(key->expiration(), 2000);
    assert_int_equal(key->subkey_count(), 1);
    assert_non_null(subkey = pubring->get_subkey(*key, 0));
    assert_false(subkey->valid());
    assert_false(subkey->expired());
    delete pubring;

    /* Case13:
     * Keys Alice [pub, sub]
     * Alice key has expiring direct-key signature, non-expiring self-certification and
     * non-expiring primary userid certification. Result: Alice [invalid], Alice sub[invalid]
     */
    pubring = new rnp::KeyStore(KEYSIG_PATH "case13/pubring.gpg", global_ctx);
    assert_non_null(pubring);
    assert_true(pubring->load());
    assert_non_null(key = rnp_tests_get_key_by_id(pubring, "0451409669FFDE3C"));
    assert_false(key->valid());
    assert_true(key->expired());
    assert_int_equal(key->expiration(), 6);
    assert_int_equal(key->subkey_count(), 1);
    assert_non_null(subkey = pubring->get_subkey(*key, 0));
    assert_false(subkey->valid());
    assert_false(subkey->expired());
    delete pubring;

    /* Case14:
     * Keys Alice [pub, sub]
     * Alice key has expiring direct-key signature, non-expiring self-certification and
     * non-expiring primary userid certification (with 0 key expiration subpacket). Result:
     * Alice [invalid], Alice sub[invalid]
     */
    pubring = new rnp::KeyStore(KEYSIG_PATH "case14/pubring.gpg", global_ctx);
    assert_non_null(pubring);
    assert_true(pubring->load());
    assert_non_null(key = rnp_tests_get_key_by_id(pubring, "0451409669FFDE3C"));
    assert_false(key->valid());
    assert_true(key->expired());
    assert_int_equal(key->expiration(), 6);
    assert_int_equal(key->subkey_count(), 1);
    assert_non_null(subkey = pubring->get_subkey(*key, 0));
    assert_false(subkey->valid());
    assert_false(subkey->expired());
    delete pubring;

    /* Case15:
     * Keys [pub, sub]
     * Signing subkey has expired primary-key signature embedded into the subkey binding.
     * Result: primary [valid], sub[invalid]
     */
    pubring = new rnp::KeyStore(KEYSIG_PATH "case15/pubring.gpg", global_ctx);
    assert_non_null(pubring);
    assert_true(pubring->load());
    assert_non_null(key = rnp_tests_get_key_by_id(pubring, "E863072D3E9042EE"));
    assert_true(key->valid());
    assert_false(key->expired());
    assert_int_equal(key->expiration(), 0);
    assert_int_equal(key->subkey_count(), 1);
    assert_non_null(subkey = pubring->get_subkey(*key, 0));
    assert_false(subkey->valid());
    assert_false(subkey->expired());
    delete pubring;
}

TEST_F(rnp_tests, test_key_expiry_direct_sig)
{
    /* this test was mainly used to generate test data for cases 10-12 in test_key_validity */
    auto secring = new rnp::KeyStore(KEYSIG_PATH "alice-sub-sec.pgp", global_ctx);
    assert_true(secring->load());
    rnp::Key *key = nullptr;
    assert_non_null(key = rnp_tests_key_search(secring, "Alice <alice@rnp>"));
    assert_true(key->valid());
    assert_false(key->expired());
    /* create direct-key signature */
    pgp::pkt::Signature sig;

    sig.version = PGP_V4;
    sig.halg = PGP_HASH_SHA256;
    sig.palg = key->alg();
    sig.set_type(PGP_SIG_DIRECT);
    sig.set_creation(key->creation());
    sig.set_key_expiration(1000);
    sig.set_keyfp(key->fp());
    sig.set_keyid(key->keyid());

    pgp_password_provider_t pprov(string_copy_password_callback, (void *) "password");
    key->unlock(pprov);
    key->sign_direct(key->pkt(), sig, global_ctx);
    key->add_sig(sig, rnp::UserID::None);
    key->revalidate(*secring);

    /* key becomsed invalid even since it is secret */
    assert_int_equal(key->expiration(), 1000);
    assert_false(key->valid());
    assert_true(key->expired());

    auto pubring = new rnp::KeyStore(KEYSIG_PATH "alice-sub-pub.pgp", global_ctx);
    assert_true(pubring->load());
    rnp::Key *pubkey = nullptr;
    assert_non_null(pubkey = rnp_tests_key_search(pubring, "Alice <alice@rnp>"));
    assert_int_equal(pubkey->expiration(), 0);
    assert_true(pubkey->valid());
    assert_false(pubkey->expired());
    rnp::Key *subpub = nullptr;
    assert_non_null(subpub = rnp_tests_get_key_by_id(pubring, "dd23ceb7febeff17"));
    assert_int_equal(subpub->expiration(), 0);
    assert_true(subpub->valid());
    assert_false(subpub->expired());

    pubkey->add_sig(sig, rnp::UserID::None);
    pubkey->revalidate(*pubring);
    assert_int_equal(pubkey->expiration(), 1000);
    assert_false(pubkey->valid());
    assert_true(pubkey->expired());
    assert_int_equal(subpub->expiration(), 0);
    assert_false(subpub->valid());
    assert_false(subpub->expired());

    /* add primary userid with smaller expiration date */
    rnp::CertParams selfsig1 = {};
    const char *    boris = "Boris <boris@rnp>";
    selfsig1.userid = boris;
    selfsig1.key_expiration = 100;
    selfsig1.primary = true;
    key->add_uid_cert(selfsig1, PGP_HASH_SHA256, global_ctx);
    key->revalidate(*secring);
    /* key becomes invalid even it is secret */
    assert_int_equal(key->expiration(), 100);
    assert_false(key->valid());
    assert_true(key->expired());

    delete secring;
    delete pubring;

    secring = new rnp::KeyStore(KEYSIG_PATH "alice-sub-sec.pgp", global_ctx);
    assert_true(secring->load());
    assert_non_null(key = rnp_tests_key_search(secring, "Alice <alice@rnp>"));
    /* create direct-key signature */
    sig = {};
    sig.version = PGP_V4;
    sig.halg = PGP_HASH_SHA256;
    sig.palg = key->alg();
    sig.set_type(PGP_SIG_DIRECT);
    sig.set_creation(key->creation());
    sig.set_key_expiration(6);
    sig.set_keyfp(key->fp());
    sig.set_keyid(key->keyid());

    key->unlock(pprov);
    key->sign_direct(key->pkt(), sig, global_ctx);
    key->add_sig(sig, rnp::UserID::None);
    key->revalidate(*secring);
    assert_int_equal(key->expiration(), 6);
    /* add primary userid with 0 expiration */
    selfsig1 = {};
    selfsig1.userid = boris;
    selfsig1.key_expiration = 0;
    selfsig1.primary = true;
    key->add_uid_cert(selfsig1, PGP_HASH_SHA256, global_ctx);
    key->revalidate(*secring);
    assert_int_equal(key->expiration(), 6);

    pubring = new rnp::KeyStore(KEYSIG_PATH "alice-sub-pub.pgp", global_ctx);
    assert_true(pubring->load());
    assert_non_null(pubkey = rnp_tests_key_search(pubring, "Alice <alice@rnp>"));
    assert_non_null(subpub = rnp_tests_get_key_by_id(pubring, "dd23ceb7febeff17"));
    assert_int_equal(subpub->expiration(), 0);
    assert_true(subpub->valid());
    assert_false(subpub->expired());

    pubkey->add_sig(sig, rnp::UserID::None);
    pubkey->revalidate(*pubring);
    assert_int_equal(pubkey->expiration(), 6);
    assert_false(pubkey->valid());
    assert_true(pubkey->expired());

    pgp_transferable_userid_t truid = {};
    truid.uid = key->get_uid(1).pkt;
    truid.signatures.push_back(key->get_sig(key->get_uid(1).get_sig(0)).sig);
    pubkey->add_uid(truid);
    pubkey->revalidate(*pubring);

    assert_int_equal(pubkey->expiration(), 6);
    assert_false(pubkey->valid());
    assert_true(pubkey->expired());

    /* code below may be used to print out generated key to save it somewhere */
    /*
    pgp_dest_t out = {};
    pgp_dest_t armored = {};
    assert_rnp_success(init_stdout_dest(&out));
    assert_rnp_success(init_armored_dst(&armored, &out, PGP_ARMORED_PUBLIC_KEY));
    pubkey->write_xfer(armored, pubring);
    dst_close(&armored, false);
    dst_close(&out, false);
    */

    delete secring;
    delete pubring;
}
