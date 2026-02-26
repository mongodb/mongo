/*
 * Copyright (c) 2017-2020 [Ribose Inc](https://www.ribose.com).
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

#include "../librepgp/stream-packet.h"
#include "../librepgp/stream-sig.h"
#include "key.hpp"
#include "utils.h"

#include "rnp_tests.h"
#include "support.h"

/* This test loads a .gpg pubring with a single V3 key,
 * and confirms that appropriate key flags are set.
 */
TEST_F(rnp_tests, test_load_v3_keyring_pgp)
{
    pgp_source_t src = {};

    auto key_store = new rnp::KeyStore(global_ctx);

    // load pubring in to the key store
    assert_rnp_success(init_file_src(&src, "data/keyrings/2/pubring.gpg"));
    assert_rnp_success(key_store->load_pgp(src));
    src.close();
    assert_int_equal(1, key_store->key_count());

    // find the key by keyid
    const rnp::Key *key = rnp_tests_get_key_by_id(key_store, "DC70C124A50283F1");
    assert_non_null(key);

    // confirm the key flags are correct
    assert_int_equal(key->flags(),
                     PGP_KF_ENCRYPT | PGP_KF_SIGN | PGP_KF_CERTIFY | PGP_KF_AUTH);

    // confirm that key expiration is correct
    assert_int_equal(key->expiration(), 0);

    // cleanup
    delete key_store;

    // load secret keyring and decrypt the key
    key_store = new rnp::KeyStore(global_ctx);

    assert_rnp_success(init_file_src(&src, "data/keyrings/4/secring.pgp"));
    assert_rnp_success(key_store->load_pgp(src));
    src.close();
    assert_int_equal(1, key_store->key_count());

    key = rnp_tests_get_key_by_id(key_store, "7D0BC10E933404C9");
    assert_non_null(key);

    // confirm the key flags are correct
    assert_int_equal(key->flags(),
                     PGP_KF_ENCRYPT | PGP_KF_SIGN | PGP_KF_CERTIFY | PGP_KF_AUTH);

    // check if the key is secret and is locked
    assert_true(key->is_secret());
    assert_true(key->is_locked());

    // decrypt the key
    pgp_key_pkt_t *seckey = pgp_decrypt_seckey_pgp(key->rawpkt(), key->pkt(), "password");
#if defined(ENABLE_IDEA)
    assert_non_null(seckey);
#else
    assert_null(seckey);
#endif

    // cleanup
    delete seckey;
    delete key_store;
}

/* This test loads a .gpg pubring with multiple V4 keys,
 * finds a particular key of interest, and confirms that
 * the appropriate key flags are set.
 */
TEST_F(rnp_tests, test_load_v4_keyring_pgp)
{
    pgp_source_t src = {};

    auto key_store = new rnp::KeyStore(global_ctx);

    // load it in to the key store
    assert_rnp_success(init_file_src(&src, "data/keyrings/1/pubring.gpg"));
    assert_rnp_success(key_store->load_pgp(src));
    src.close();
    assert_int_equal(7, key_store->key_count());

    // find the key by keyid
    static const std::string keyid = "8a05b89fad5aded1";
    const rnp::Key *         key = rnp_tests_get_key_by_id(key_store, keyid);
    assert_non_null(key);

    // confirm the key flags are correct
    assert_int_equal(key->flags(), PGP_KF_ENCRYPT);

    // cleanup
    delete key_store;
}

/* Just a helper for the below test */
static void
check_pgp_keyring_counts(const char *          path,
                         unsigned              primary_count,
                         const unsigned        subkey_counts[],
                         rnp::SecurityContext &global_ctx)
{
    pgp_source_t src = {};
    auto         key_store = new rnp::KeyStore(global_ctx);

    // load it in to the key store
    assert_rnp_success(init_file_src(&src, path));
    assert_rnp_success(key_store->load_pgp(src));
    src.close();

    // count primary keys first
    unsigned total_primary_count = 0;
    for (auto &key : key_store->keys) {
        if (key.is_primary()) {
            total_primary_count++;
        }
    }
    assert_int_equal(primary_count, total_primary_count);

    // now count subkeys in each primary key
    unsigned total_subkey_count = 0;
    unsigned primary = 0;
    for (auto &key : key_store->keys) {
        if (key.is_primary()) {
            // check the subkey count for this primary key
            assert_int_equal(key.subkey_count(), subkey_counts[primary++]);
        } else if (key.is_subkey()) {
            total_subkey_count++;
        }
    }

    // check the total (not really needed)
    assert_int_equal(key_store->key_count(), total_primary_count + total_subkey_count);

    // cleanup
    delete key_store;
}

/* This test loads a pubring.gpg and secring.gpg and confirms
 * that it contains the expected number of primary keys
 * and the expected number of subkeys for each primary key.
 */
TEST_F(rnp_tests, test_load_keyring_and_count_pgp)
{
    unsigned int primary_count = 2;
    unsigned int subkey_counts[2] = {3, 2};

    // check pubring
    check_pgp_keyring_counts(
      "data/keyrings/1/pubring.gpg", primary_count, subkey_counts, global_ctx);

    // check secring
    check_pgp_keyring_counts(
      "data/keyrings/1/secring.gpg", primary_count, subkey_counts, global_ctx);
}

/* This test loads a V4 keyring and confirms that certain
 * bitfields and time fields are set correctly.
 */
TEST_F(rnp_tests, test_load_check_bitfields_and_times)
{
    const rnp::Key *           key;
    const pgp::pkt::Signature *sig = nullptr;

    // load keyring
    auto key_store = new rnp::KeyStore("data/keyrings/1/pubring.gpg", global_ctx);
    assert_true(key_store->load());

    // find
    key = NULL;
    key = rnp_tests_get_key_by_id(key_store, "7BC6709B15C23A4A");
    assert_non_null(key);
    // check subsig count
    assert_int_equal(key->sig_count(), 3);
    // check subsig properties
    for (size_t i = 0; i < key->sig_count(); i++) {
        sig = &key->get_sig(i).sig;
        static const time_t expected_creation_times[] = {1500569820, 1500569836, 1500569846};
        // check SS_ISSUER_KEY_ID
        assert_true(cmp_keyid(sig->keyid(), "7BC6709B15C23A4A"));
        // check SS_CREATION_TIME
        assert_int_equal(sig->creation(), expected_creation_times[i]);
        // check SS_EXPIRATION_TIME
        assert_int_equal(sig->expiration(), 0);
    }
    // check SS_KEY_EXPIRY
    assert_int_equal(key->expiration(), 0);

    // find
    key = NULL;
    key = rnp_tests_get_key_by_id(key_store, "1ED63EE56FADC34D");
    assert_non_null(key);
    // check subsig count
    assert_int_equal(key->sig_count(), 1);
    sig = &key->get_sig(0).sig;
    // check SS_ISSUER_KEY_ID
    assert_true(cmp_keyid(sig->keyid(), "7BC6709B15C23A4A"));
    // check SS_CREATION_TIME [0]
    assert_int_equal(sig->creation(), 1500569820);
    assert_int_equal(sig->creation(), key->creation());
    // check SS_EXPIRATION_TIME [0]
    assert_int_equal(sig->expiration(), 0);
    // check SS_KEY_EXPIRY
    assert_int_equal(key->expiration(), 0);

    // find
    key = NULL;
    key = rnp_tests_get_key_by_id(key_store, "1D7E8A5393C997A8");
    assert_non_null(key);
    // check subsig count
    assert_int_equal(key->sig_count(), 1);
    sig = &key->get_sig(0).sig;
    // check SS_ISSUER_KEY_ID
    assert_true(cmp_keyid(sig->keyid(), "7BC6709B15C23A4A"));
    // check SS_CREATION_TIME [0]
    assert_int_equal(sig->creation(), 1500569851);
    assert_int_equal(sig->creation(), key->creation());
    // check SS_EXPIRATION_TIME [0]
    assert_int_equal(sig->expiration(), 0);
    // check SS_KEY_EXPIRY
    assert_int_equal(key->expiration(), 123 * 24 * 60 * 60 /* 123 days */);

    // find
    key = NULL;
    key = rnp_tests_get_key_by_id(key_store, "8A05B89FAD5ADED1");
    assert_non_null(key);
    // check subsig count
    assert_int_equal(key->sig_count(), 1);
    sig = &key->get_sig(0).sig;
    // check SS_ISSUER_KEY_ID
    assert_true(cmp_keyid(sig->keyid(), "7BC6709B15C23A4A"));
    // check SS_CREATION_TIME [0]
    assert_int_equal(sig->creation(), 1500569896);
    assert_int_equal(sig->creation(), key->creation());
    // check SS_EXPIRATION_TIME [0]
    assert_int_equal(sig->expiration(), 0);
    // check SS_KEY_EXPIRY
    assert_int_equal(key->expiration(), 0);

    // find
    key = NULL;
    key = rnp_tests_get_key_by_id(key_store, "2FCADF05FFA501BB");
    assert_non_null(key);
    // check subsig count
    assert_int_equal(key->sig_count(), 3);
    // check subsig properties
    for (size_t i = 0; i < key->sig_count(); i++) {
        sig = &key->get_sig(i).sig;
        static const time_t expected_creation_times[] = {1501372449, 1500570153, 1500570147};

        // check SS_ISSUER_KEY_ID
        assert_true(cmp_keyid(sig->keyid(), "2FCADF05FFA501BB"));
        // check SS_CREATION_TIME
        assert_int_equal(sig->creation(), expected_creation_times[i]);
        // check SS_EXPIRATION_TIME
        assert_int_equal(sig->expiration(), 0);
    }
    // check SS_KEY_EXPIRY
    assert_int_equal(key->expiration(), 2076663808);

    // find
    key = NULL;
    key = rnp_tests_get_key_by_id(key_store, "54505A936A4A970E");
    assert_non_null(key);
    // check subsig count
    assert_int_equal(key->sig_count(), 1);
    sig = &key->get_sig(0).sig;
    // check SS_ISSUER_KEY_ID
    assert_true(cmp_keyid(sig->keyid(), "2FCADF05FFA501BB"));
    // check SS_CREATION_TIME [0]
    assert_int_equal(sig->creation(), 1500569946);
    assert_int_equal(sig->creation(), key->creation());
    // check SS_EXPIRATION_TIME [0]
    assert_int_equal(sig->expiration(), 0);
    // check SS_KEY_EXPIRY
    assert_int_equal(key->expiration(), 2076663808);

    // find
    key = NULL;
    key = rnp_tests_get_key_by_id(key_store, "326EF111425D14A5");
    assert_non_null(key);
    // check subsig count
    assert_int_equal(key->sig_count(), 1);
    sig = &key->get_sig(0).sig;
    // check SS_ISSUER_KEY_ID
    assert_true(cmp_keyid(sig->keyid(), "2FCADF05FFA501BB"));
    // check SS_CREATION_TIME [0]
    assert_int_equal(sig->creation(), 1500570165);
    assert_int_equal(sig->creation(), key->creation());
    // check SS_EXPIRATION_TIME [0]
    assert_int_equal(sig->expiration(), 0);
    // check SS_KEY_EXPIRY
    assert_int_equal(key->expiration(), 0);

    // cleanup
    delete key_store;
}

/* This test loads a V3 keyring and confirms that certain
 * bitfields and time fields are set correctly.
 */
TEST_F(rnp_tests, test_load_check_bitfields_and_times_v3)
{
    pgp::KeyID                 keyid = {};
    const rnp::Key *           key;
    const pgp::pkt::Signature *sig = nullptr;

    // load keyring
    auto key_store = new rnp::KeyStore("data/keyrings/2/pubring.gpg", global_ctx);
    assert_true(key_store->load());

    // find
    key = NULL;
    assert_true(rnp::hex_decode("DC70C124A50283F1", keyid.data(), keyid.size()));
    key = rnp_tests_get_key_by_id(key_store, "DC70C124A50283F1");
    assert_non_null(key);
    // check key version
    assert_int_equal(key->version(), PGP_V3);
    // check subsig count
    assert_int_equal(key->sig_count(), 1);
    sig = &key->get_sig(0).sig;
    // check signature version
    assert_int_equal(sig->version, 3);
    // check issuer
    assert_true(rnp::hex_decode("DC70C124A50283F1", keyid.data(), keyid.size()));
    assert_true(keyid == sig->keyid());
    // check creation time
    assert_int_equal(sig->creation(), 1005209227);
    assert_int_equal(sig->creation(), key->creation());
    // check signature expiration time (V3 sigs have none)
    assert_int_equal(sig->expiration(), 0);
    // check key expiration
    assert_int_equal(key->expiration(), 0); // only for V4 keys
    assert_int_equal(key->pkt().v3_days, 0);

    // cleanup
    delete key_store;
}

#define MERGE_PATH "data/test_stream_key_merge/"

TEST_F(rnp_tests, test_load_armored_pub_sec)
{
    auto key_store = new rnp::KeyStore(MERGE_PATH "key-both.asc", global_ctx);
    assert_true(key_store->load());

    /* we must have 1 main key and 2 subkeys */
    assert_int_equal(key_store->key_count(), 3);

    rnp::Key *key = NULL;
    assert_non_null(key = rnp_tests_get_key_by_id(key_store, "9747D2A6B3A63124"));
    assert_true(key->valid());
    assert_true(key->is_primary());
    assert_true(key->is_secret());
    assert_int_equal(key->rawpkt_count(), 5);
    assert_int_equal(key->rawpkt().tag(), PGP_PKT_SECRET_KEY);
    assert_int_equal(key->get_uid(0).rawpkt.tag(), PGP_PKT_USER_ID);
    assert_int_equal(key->get_sig(0).raw.tag(), PGP_PKT_SIGNATURE);
    assert_int_equal(key->get_uid(1).rawpkt.tag(), PGP_PKT_USER_ID);
    assert_int_equal(key->get_sig(1).raw.tag(), PGP_PKT_SIGNATURE);

    assert_non_null(key = rnp_tests_get_key_by_id(key_store, "AF1114A47F5F5B28"));
    assert_true(key->valid());
    assert_true(key->is_subkey());
    assert_true(key->is_secret());
    assert_int_equal(key->rawpkt_count(), 2);
    assert_int_equal(key->rawpkt().tag(), PGP_PKT_SECRET_SUBKEY);
    assert_int_equal(key->get_sig(0).raw.tag(), PGP_PKT_SIGNATURE);

    assert_non_null(key = rnp_tests_get_key_by_id(key_store, "16CD16F267CCDD4F"));
    assert_true(key->valid());
    assert_true(key->is_subkey());
    assert_true(key->is_secret());
    assert_int_equal(key->rawpkt_count(), 2);
    assert_int_equal(key->rawpkt().tag(), PGP_PKT_SECRET_SUBKEY);
    assert_int_equal(key->get_sig(0).raw.tag(), PGP_PKT_SIGNATURE);

    /* make sure half of keyid doesn't work */
    assert_null(key = rnp_tests_get_key_by_id(key_store, "0000000016CD16F2"));
    assert_null(key = rnp_tests_get_key_by_id(key_store, "67CCDD4F00000000"));
    assert_null(key = rnp_tests_get_key_by_id(key_store, "0000000067CCDD4F"));

    /* both user ids should be present */
    assert_non_null(rnp_tests_key_search(key_store, "key-merge-uid-1"));
    assert_non_null(rnp_tests_key_search(key_store, "key-merge-uid-2"));

    delete key_store;
}

static bool
load_transferable_key(pgp_transferable_key_t *key, const char *fname)
{
    pgp_source_t src = {};
    bool         res = !init_file_src(&src, fname) && !process_pgp_key(src, *key, false);
    src.close();
    return res;
}

static bool
load_transferable_subkey(pgp_transferable_subkey_t *key, const char *fname)
{
    pgp_source_t src = {};
    bool         res = !init_file_src(&src, fname) && !process_pgp_subkey(src, *key, false);
    src.close();
    return res;
}

static bool
load_keystore(rnp::KeyStore *keystore, const char *fname)
{
    pgp_source_t src = {};
    bool         res = !init_file_src(&src, fname) && !keystore->load_pgp(src);
    src.close();
    return res;
}

static bool
check_subkey_fp(rnp::Key *key, rnp::Key *subkey, size_t index)
{
    if (key->get_subkey_fp(index) != subkey->fp()) {
        return false;
    }
    if (!subkey->has_primary_fp()) {
        return false;
    }
    return key->fp() == subkey->primary_fp();
}

TEST_F(rnp_tests, test_load_merge)
{
    rnp::Key *                key, *skey1, *skey2;
    pgp_transferable_key_t    tkey = {};
    pgp_transferable_subkey_t tskey = {};
    pgp_password_provider_t   provider = {};
    provider.callback = string_copy_password_callback;
    provider.userdata = (void *) "password";

    auto        key_store = new rnp::KeyStore("", global_ctx);
    std::string keyid = "9747D2A6B3A63124";
    std::string sub1id = "AF1114A47F5F5B28";
    std::string sub2id = "16CD16F267CCDD4F";

    /* load just key packet */
    assert_true(load_transferable_key(&tkey, MERGE_PATH "key-pub-just-key.pgp"));
    assert_true(key_store->add_ts_key(tkey));
    assert_int_equal(key_store->key_count(), 1);
    assert_non_null(key = rnp_tests_get_key_by_id(key_store, keyid));
    assert_false(key->valid());
    assert_int_equal(key->rawpkt_count(), 1);
    assert_int_equal(key->rawpkt().tag(), PGP_PKT_PUBLIC_KEY);

    /* load key + user id 1 without sigs */
    assert_true(load_transferable_key(&tkey, MERGE_PATH "key-pub-uid-1-no-sigs.pgp"));
    assert_true(key_store->add_ts_key(tkey));
    assert_int_equal(key_store->key_count(), 1);
    assert_non_null(key = rnp_tests_get_key_by_id(key_store, keyid));
    assert_false(key->valid());
    assert_int_equal(key->uid_count(), 1);
    assert_int_equal(key->rawpkt_count(), 2);
    assert_int_equal(key->rawpkt().tag(), PGP_PKT_PUBLIC_KEY);
    assert_int_equal(key->get_uid(0).rawpkt.tag(), PGP_PKT_USER_ID);
    assert_null(rnp_tests_key_search(key_store, "key-merge-uid-1"));
    assert_true(key == rnp_tests_get_key_by_id(key_store, "9747D2A6B3A63124"));

    /* load key + user id 1 with sigs */
    assert_true(load_transferable_key(&tkey, MERGE_PATH "key-pub-uid-1.pgp"));
    assert_true(key_store->add_ts_key(tkey));
    assert_int_equal(key_store->key_count(), 1);
    assert_non_null(key = rnp_tests_get_key_by_id(key_store, keyid));
    assert_true(key->valid());
    assert_int_equal(key->uid_count(), 1);
    assert_int_equal(key->rawpkt_count(), 3);
    assert_int_equal(key->rawpkt().tag(), PGP_PKT_PUBLIC_KEY);
    assert_int_equal(key->get_uid(0).rawpkt.tag(), PGP_PKT_USER_ID);
    assert_int_equal(key->get_sig(0).raw.tag(), PGP_PKT_SIGNATURE);
    assert_true(key == rnp_tests_key_search(key_store, "key-merge-uid-1"));

    /* load key + user id 2 with sigs */
    assert_true(load_transferable_key(&tkey, MERGE_PATH "key-pub-uid-2.pgp"));
    assert_true(key_store->add_ts_key(tkey));
    /* try to add it twice */
    assert_true(key_store->add_ts_key(tkey));
    assert_int_equal(key_store->key_count(), 1);
    assert_non_null(key = rnp_tests_get_key_by_id(key_store, keyid));
    assert_true(key->valid());
    assert_int_equal(key->uid_count(), 2);
    assert_int_equal(key->rawpkt_count(), 5);
    assert_int_equal(key->rawpkt().tag(), PGP_PKT_PUBLIC_KEY);
    assert_int_equal(key->get_uid(0).rawpkt.tag(), PGP_PKT_USER_ID);
    assert_int_equal(key->get_sig(0).raw.tag(), PGP_PKT_SIGNATURE);
    assert_int_equal(key->get_uid(1).rawpkt.tag(), PGP_PKT_USER_ID);
    assert_int_equal(key->get_sig(1).raw.tag(), PGP_PKT_SIGNATURE);
    assert_true(key == rnp_tests_key_search(key_store, "key-merge-uid-1"));
    assert_true(key == rnp_tests_key_search(key_store, "key-merge-uid-2"));

    /* load key + subkey 1 without sigs */
    assert_true(load_transferable_key(&tkey, MERGE_PATH "key-pub-subkey-1-no-sigs.pgp"));
    assert_true(key_store->add_ts_key(tkey));
    assert_int_equal(key_store->key_count(), 2);
    assert_non_null(key = rnp_tests_get_key_by_id(key_store, keyid));
    assert_non_null(skey1 = rnp_tests_get_key_by_id(key_store, sub1id));
    assert_true(key->valid());
    assert_false(skey1->valid());
    assert_int_equal(key->uid_count(), 2);
    assert_int_equal(key->subkey_count(), 1);
    assert_true(check_subkey_fp(key, skey1, 0));
    assert_int_equal(key->rawpkt_count(), 5);
    assert_int_equal(key->rawpkt().tag(), PGP_PKT_PUBLIC_KEY);
    assert_int_equal(key->get_uid(0).rawpkt.tag(), PGP_PKT_USER_ID);
    assert_int_equal(key->get_sig(0).raw.tag(), PGP_PKT_SIGNATURE);
    assert_int_equal(key->get_uid(1).rawpkt.tag(), PGP_PKT_USER_ID);
    assert_int_equal(key->get_sig(1).raw.tag(), PGP_PKT_SIGNATURE);
    assert_int_equal(skey1->uid_count(), 0);
    assert_int_equal(skey1->rawpkt_count(), 1);
    assert_int_equal(skey1->rawpkt().tag(), PGP_PKT_PUBLIC_SUBKEY);

    /* load just subkey 1 but with signature */
    assert_true(load_transferable_subkey(&tskey, MERGE_PATH "key-pub-no-key-subkey-1.pgp"));
    assert_true(key_store->add_ts_subkey(tskey, key));
    /* try to add it twice */
    assert_true(key_store->add_ts_subkey(tskey, key));
    assert_int_equal(key_store->key_count(), 2);
    assert_non_null(key = rnp_tests_get_key_by_id(key_store, keyid));
    assert_non_null(skey1 = rnp_tests_get_key_by_id(key_store, sub1id));
    assert_true(key->valid());
    assert_true(skey1->valid());
    assert_int_equal(key->uid_count(), 2);
    assert_int_equal(key->subkey_count(), 1);
    assert_true(check_subkey_fp(key, skey1, 0));
    assert_int_equal(key->rawpkt_count(), 5);
    assert_int_equal(key->rawpkt().tag(), PGP_PKT_PUBLIC_KEY);
    assert_int_equal(key->get_uid(0).rawpkt.tag(), PGP_PKT_USER_ID);
    assert_int_equal(key->get_sig(0).raw.tag(), PGP_PKT_SIGNATURE);
    assert_int_equal(key->get_uid(1).rawpkt.tag(), PGP_PKT_USER_ID);
    assert_int_equal(key->get_sig(1).raw.tag(), PGP_PKT_SIGNATURE);
    assert_int_equal(skey1->uid_count(), 0);
    assert_int_equal(skey1->rawpkt_count(), 2);
    assert_int_equal(skey1->rawpkt().tag(), PGP_PKT_PUBLIC_SUBKEY);
    assert_int_equal(skey1->get_sig(0).raw.tag(), PGP_PKT_SIGNATURE);

    /* load key + subkey 2 with signature */
    assert_true(load_transferable_key(&tkey, MERGE_PATH "key-pub-subkey-2.pgp"));
    assert_true(key_store->add_ts_key(tkey));
    /* try to add it twice */
    assert_true(key_store->add_ts_key(tkey));
    assert_int_equal(key_store->key_count(), 3);
    assert_non_null(key = rnp_tests_get_key_by_id(key_store, keyid));
    assert_non_null(skey1 = rnp_tests_get_key_by_id(key_store, sub1id));
    assert_non_null(skey2 = rnp_tests_get_key_by_id(key_store, sub2id));
    assert_true(key->valid());
    assert_true(skey1->valid());
    assert_true(skey2->valid());
    assert_int_equal(key->uid_count(), 2);
    assert_int_equal(key->subkey_count(), 2);
    assert_true(check_subkey_fp(key, skey1, 0));
    assert_true(check_subkey_fp(key, skey2, 1));
    assert_int_equal(key->rawpkt_count(), 5);
    assert_int_equal(key->rawpkt().tag(), PGP_PKT_PUBLIC_KEY);
    assert_int_equal(key->get_uid(0).rawpkt.tag(), PGP_PKT_USER_ID);
    assert_int_equal(key->get_sig(0).raw.tag(), PGP_PKT_SIGNATURE);
    assert_int_equal(key->get_uid(1).rawpkt.tag(), PGP_PKT_USER_ID);
    assert_int_equal(key->get_sig(1).raw.tag(), PGP_PKT_SIGNATURE);
    assert_int_equal(skey1->uid_count(), 0);
    assert_int_equal(skey1->rawpkt_count(), 2);
    assert_int_equal(skey1->rawpkt().tag(), PGP_PKT_PUBLIC_SUBKEY);
    assert_int_equal(skey1->get_sig(0).raw.tag(), PGP_PKT_SIGNATURE);
    assert_int_equal(skey2->uid_count(), 0);
    assert_int_equal(skey2->rawpkt_count(), 2);
    assert_int_equal(skey2->rawpkt().tag(), PGP_PKT_PUBLIC_SUBKEY);
    assert_int_equal(skey2->get_sig(0).raw.tag(), PGP_PKT_SIGNATURE);

    /* load secret key & subkeys */
    assert_true(load_transferable_key(&tkey, MERGE_PATH "key-sec-no-uid-no-sigs.pgp"));
    assert_true(key_store->add_ts_key(tkey));
    /* try to add it twice */
    assert_true(key_store->add_ts_key(tkey));
    assert_int_equal(key_store->key_count(), 3);
    assert_non_null(key = rnp_tests_get_key_by_id(key_store, keyid));
    assert_non_null(skey1 = rnp_tests_get_key_by_id(key_store, sub1id));
    assert_non_null(skey2 = rnp_tests_get_key_by_id(key_store, sub2id));
    assert_true(key->valid());
    assert_true(skey1->valid());
    assert_true(skey2->valid());
    assert_int_equal(key->uid_count(), 2);
    assert_int_equal(key->subkey_count(), 2);
    assert_true(check_subkey_fp(key, skey1, 0));
    assert_true(check_subkey_fp(key, skey2, 1));
    assert_int_equal(key->rawpkt_count(), 5);
    assert_int_equal(key->rawpkt().tag(), PGP_PKT_SECRET_KEY);
    assert_int_equal(key->get_uid(0).rawpkt.tag(), PGP_PKT_USER_ID);
    assert_int_equal(key->get_sig(0).raw.tag(), PGP_PKT_SIGNATURE);
    assert_int_equal(key->get_uid(1).rawpkt.tag(), PGP_PKT_USER_ID);
    assert_int_equal(key->get_sig(1).raw.tag(), PGP_PKT_SIGNATURE);
    assert_int_equal(skey1->uid_count(), 0);
    assert_int_equal(skey1->rawpkt_count(), 2);
    assert_int_equal(skey1->rawpkt().tag(), PGP_PKT_SECRET_SUBKEY);
    assert_int_equal(skey1->get_sig(0).raw.tag(), PGP_PKT_SIGNATURE);
    assert_int_equal(skey2->uid_count(), 0);
    assert_int_equal(skey2->rawpkt_count(), 2);
    assert_int_equal(skey2->rawpkt().tag(), PGP_PKT_SECRET_SUBKEY);
    assert_int_equal(skey2->get_sig(0).raw.tag(), PGP_PKT_SIGNATURE);

    assert_true(key->unlock(provider));
    assert_true(skey1->unlock(provider));
    assert_true(skey2->unlock(provider));

    /* load the whole public + secret key */
    assert_true(load_transferable_key(&tkey, MERGE_PATH "key-pub.asc"));
    assert_true(key_store->add_ts_key(tkey));
    assert_true(load_transferable_key(&tkey, MERGE_PATH "key-sec.asc"));
    assert_true(key_store->add_ts_key(tkey));
    assert_int_equal(key_store->key_count(), 3);
    assert_non_null(key = rnp_tests_get_key_by_id(key_store, keyid));
    assert_non_null(skey1 = rnp_tests_get_key_by_id(key_store, sub1id));
    assert_non_null(skey2 = rnp_tests_get_key_by_id(key_store, sub2id));
    assert_true(key->valid());
    assert_true(skey1->valid());
    assert_true(skey2->valid());
    assert_int_equal(key->uid_count(), 2);
    assert_int_equal(key->subkey_count(), 2);
    assert_true(check_subkey_fp(key, skey1, 0));
    assert_true(check_subkey_fp(key, skey2, 1));
    assert_int_equal(key->rawpkt_count(), 5);
    assert_int_equal(key->rawpkt().tag(), PGP_PKT_SECRET_KEY);
    assert_int_equal(key->get_uid(0).rawpkt.tag(), PGP_PKT_USER_ID);
    assert_int_equal(key->get_sig(0).raw.tag(), PGP_PKT_SIGNATURE);
    assert_int_equal(key->get_uid(1).rawpkt.tag(), PGP_PKT_USER_ID);
    assert_int_equal(key->get_sig(1).raw.tag(), PGP_PKT_SIGNATURE);
    assert_int_equal(skey1->uid_count(), 0);
    assert_int_equal(skey1->rawpkt_count(), 2);
    assert_int_equal(skey1->rawpkt().tag(), PGP_PKT_SECRET_SUBKEY);
    assert_int_equal(skey1->get_sig(0).raw.tag(), PGP_PKT_SIGNATURE);
    assert_int_equal(skey2->uid_count(), 0);
    assert_int_equal(skey2->rawpkt_count(), 2);
    assert_int_equal(skey2->rawpkt().tag(), PGP_PKT_SECRET_SUBKEY);
    assert_int_equal(skey2->get_sig(0).raw.tag(), PGP_PKT_SIGNATURE);
    assert_true(key == rnp_tests_key_search(key_store, "key-merge-uid-1"));
    assert_true(key == rnp_tests_key_search(key_store, "key-merge-uid-2"));

    delete key_store;
}

TEST_F(rnp_tests, test_load_public_from_secret)
{
    auto secstore = new rnp::KeyStore(MERGE_PATH "key-sec.asc", global_ctx);
    assert_true(secstore->load());
    auto pubstore = new rnp::KeyStore("pubring.gpg", global_ctx);

    std::string keyid = "9747D2A6B3A63124";
    std::string sub1id = "AF1114A47F5F5B28";
    std::string sub2id = "16CD16F267CCDD4F";

    rnp::Key *key = nullptr, *skey1 = nullptr, *skey2 = nullptr;
    assert_non_null(key = rnp_tests_get_key_by_id(secstore, keyid));
    assert_non_null(skey1 = rnp_tests_get_key_by_id(secstore, sub1id));
    assert_non_null(skey2 = rnp_tests_get_key_by_id(secstore, sub2id));

    /* copy the secret key */
    rnp::Key keycp = rnp::Key(*key, false);
    assert_true(keycp.is_secret());
    assert_int_equal(keycp.subkey_count(), 2);
    assert_true(keycp.get_subkey_fp(0) == skey1->fp());
    assert_true(keycp.get_subkey_fp(1) == skey2->fp());
    assert_true(keycp.grip() == key->grip());
    assert_int_equal(keycp.rawpkt().tag(), PGP_PKT_SECRET_KEY);

    /* copy the public part */
    keycp = rnp::Key(*key, true);
    assert_false(keycp.is_secret());
    assert_int_equal(keycp.subkey_count(), 2);
    assert_true(check_subkey_fp(&keycp, skey1, 0));
    assert_true(check_subkey_fp(&keycp, skey2, 1));
    assert_true(keycp.grip() == key->grip());
    assert_int_equal(keycp.rawpkt().tag(), PGP_PKT_PUBLIC_KEY);
    assert_true(keycp.pkt().sec_data.empty());
    assert_false(keycp.pkt().material->secret());
    pubstore->add_key(keycp);
    /* subkey 1 */
    keycp = rnp::Key(*skey1, true);
    assert_false(keycp.is_secret());
    assert_int_equal(keycp.subkey_count(), 0);
    assert_true(check_subkey_fp(key, &keycp, 0));
    assert_true(keycp.grip() == skey1->grip());
    assert_true(cmp_keyid(keycp.keyid(), sub1id));
    assert_int_equal(keycp.rawpkt().tag(), PGP_PKT_PUBLIC_SUBKEY);
    assert_true(keycp.pkt().sec_data.empty());
    assert_false(keycp.pkt().material->secret());
    pubstore->add_key(keycp);
    /* subkey 2 */
    keycp = rnp::Key(*skey2, true);
    assert_false(keycp.is_secret());
    assert_int_equal(keycp.subkey_count(), 0);
    assert_true(check_subkey_fp(key, &keycp, 1));
    assert_true(keycp.grip() == skey2->grip());
    assert_true(cmp_keyid(keycp.keyid(), sub2id));
    assert_int_equal(keycp.rawpkt().tag(), PGP_PKT_PUBLIC_SUBKEY);
    assert_true(keycp.pkt().sec_data.empty());
    assert_false(keycp.pkt().material->secret());
    pubstore->add_key(keycp);
    /* save pubring */
    assert_true(pubstore->write());
    delete pubstore;
    /* reload */
    pubstore = new rnp::KeyStore("pubring.gpg", global_ctx);
    assert_true(pubstore->load());
    assert_non_null(key = rnp_tests_get_key_by_id(pubstore, keyid));
    assert_non_null(skey1 = rnp_tests_get_key_by_id(pubstore, sub1id));
    assert_non_null(skey2 = rnp_tests_get_key_by_id(pubstore, sub2id));

    delete pubstore;
    delete secstore;
}

TEST_F(rnp_tests, test_key_import)
{
    cli_rnp_t                  rnp = {};
    pgp_transferable_key_t     tkey = {};
    pgp_transferable_subkey_t *tskey = NULL;
    pgp_transferable_userid_t *tuid = NULL;

    assert_int_equal(RNP_MKDIR(".rnp", S_IRWXU), 0);
    assert_true(setup_cli_rnp_common(&rnp, RNP_KEYSTORE_GPG, ".rnp", NULL));

    /* import just the public key */
    rnp_cfg &cfg = rnp.cfg();
    cfg.set_str(CFG_KEYFILE, MERGE_PATH "key-pub-just-key.pgp");
    assert_true(cli_rnp_add_key(&rnp));
    assert_true(cli_rnp_save_keyrings(&rnp));
    size_t keycount = 0;
    assert_rnp_success(rnp_get_public_key_count(rnp.ffi, &keycount));
    assert_int_equal(keycount, 1);
    assert_rnp_success(rnp_get_secret_key_count(rnp.ffi, &keycount));
    assert_int_equal(keycount, 0);

    assert_true(load_transferable_key(&tkey, ".rnp/pubring.gpg"));
    assert_true(tkey.subkeys.empty());
    assert_true(tkey.signatures.empty());
    assert_true(tkey.userids.empty());
    assert_int_equal(tkey.key.tag, PGP_PKT_PUBLIC_KEY);

    /* import public key + 1 userid */
    cfg.set_str(CFG_KEYFILE, MERGE_PATH "key-pub-uid-1-no-sigs.pgp");
    assert_true(cli_rnp_add_key(&rnp));
    assert_true(cli_rnp_save_keyrings(&rnp));
    assert_rnp_success(rnp_get_public_key_count(rnp.ffi, &keycount));
    assert_int_equal(keycount, 1);
    assert_rnp_success(rnp_get_secret_key_count(rnp.ffi, &keycount));
    assert_int_equal(keycount, 0);

    assert_true(load_transferable_key(&tkey, ".rnp/pubring.gpg"));
    assert_true(tkey.subkeys.empty());
    assert_true(tkey.signatures.empty());
    assert_int_equal(tkey.userids.size(), 1);
    assert_non_null(tuid = &tkey.userids.front());
    assert_true(tuid->signatures.empty());
    assert_false(memcmp(tuid->uid.uid.data(), "key-merge-uid-1", 15));
    assert_int_equal(tkey.key.tag, PGP_PKT_PUBLIC_KEY);
    assert_int_equal(tuid->uid.tag, PGP_PKT_USER_ID);

    /* import public key + 1 userid + signature */
    cfg.set_str(CFG_KEYFILE, MERGE_PATH "key-pub-uid-1.pgp");
    assert_true(cli_rnp_add_key(&rnp));
    assert_true(cli_rnp_save_keyrings(&rnp));
    assert_rnp_success(rnp_get_public_key_count(rnp.ffi, &keycount));
    assert_int_equal(keycount, 1);
    assert_rnp_success(rnp_get_secret_key_count(rnp.ffi, &keycount));
    assert_int_equal(keycount, 0);

    assert_true(load_transferable_key(&tkey, ".rnp/pubring.gpg"));
    assert_true(tkey.subkeys.empty());
    assert_true(tkey.signatures.empty());
    assert_int_equal(tkey.userids.size(), 1);
    assert_int_equal(tkey.key.tag, PGP_PKT_PUBLIC_KEY);
    assert_non_null(tuid = &tkey.userids.front());
    assert_int_equal(tuid->signatures.size(), 1);
    assert_false(memcmp(tuid->uid.uid.data(), "key-merge-uid-1", 15));
    assert_int_equal(tuid->uid.tag, PGP_PKT_USER_ID);

    /* import public key + 1 subkey */
    cfg.set_str(CFG_KEYFILE, MERGE_PATH "key-pub-subkey-1.pgp");
    assert_true(cli_rnp_add_key(&rnp));
    assert_true(cli_rnp_save_keyrings(&rnp));
    assert_rnp_success(rnp_get_public_key_count(rnp.ffi, &keycount));
    assert_int_equal(keycount, 2);
    assert_rnp_success(rnp_get_secret_key_count(rnp.ffi, &keycount));
    assert_int_equal(keycount, 0);

    assert_true(load_transferable_key(&tkey, ".rnp/pubring.gpg"));
    assert_int_equal(tkey.subkeys.size(), 1);
    assert_true(tkey.signatures.empty());
    assert_int_equal(tkey.userids.size(), 1);
    assert_int_equal(tkey.key.tag, PGP_PKT_PUBLIC_KEY);
    assert_non_null(tuid = &tkey.userids.front());
    assert_int_equal(tuid->signatures.size(), 1);
    assert_false(memcmp(tuid->uid.uid.data(), "key-merge-uid-1", 15));
    assert_int_equal(tuid->uid.tag, PGP_PKT_USER_ID);
    assert_non_null(tskey = &tkey.subkeys.front());
    assert_int_equal(tskey->signatures.size(), 1);
    assert_int_equal(tskey->subkey.tag, PGP_PKT_PUBLIC_SUBKEY);

    /* import secret key with 1 uid and 1 subkey */
    cfg.set_str(CFG_KEYFILE, MERGE_PATH "key-sec-uid-1-subkey-1.pgp");
    assert_true(cli_rnp_add_key(&rnp));
    assert_true(cli_rnp_save_keyrings(&rnp));
    assert_rnp_success(rnp_get_public_key_count(rnp.ffi, &keycount));
    assert_int_equal(keycount, 2);
    assert_rnp_success(rnp_get_secret_key_count(rnp.ffi, &keycount));
    assert_int_equal(keycount, 2);

    assert_true(load_transferable_key(&tkey, ".rnp/pubring.gpg"));
    assert_int_equal(tkey.subkeys.size(), 1);
    assert_true(tkey.signatures.empty());
    assert_int_equal(tkey.userids.size(), 1);
    assert_int_equal(tkey.key.tag, PGP_PKT_PUBLIC_KEY);
    assert_non_null(tuid = &tkey.userids.front());
    assert_int_equal(tuid->signatures.size(), 1);
    assert_false(memcmp(tuid->uid.uid.data(), "key-merge-uid-1", 15));
    assert_int_equal(tuid->uid.tag, PGP_PKT_USER_ID);
    assert_non_null(tskey = &tkey.subkeys.front());
    assert_int_equal(tskey->signatures.size(), 1);
    assert_int_equal(tskey->subkey.tag, PGP_PKT_PUBLIC_SUBKEY);

    assert_true(load_transferable_key(&tkey, ".rnp/secring.gpg"));
    assert_int_equal(tkey.subkeys.size(), 1);
    assert_true(tkey.signatures.empty());
    assert_int_equal(tkey.userids.size(), 1);
    assert_int_equal(tkey.key.tag, PGP_PKT_SECRET_KEY);
    assert_rnp_success(decrypt_secret_key(&tkey.key, "password"));
    assert_non_null(tuid = &tkey.userids.front());
    assert_int_equal(tuid->signatures.size(), 1);
    assert_false(memcmp(tuid->uid.uid.data(), "key-merge-uid-1", 15));
    assert_int_equal(tuid->uid.tag, PGP_PKT_USER_ID);
    assert_non_null(tskey = &tkey.subkeys.front());
    assert_int_equal(tskey->signatures.size(), 1);
    assert_int_equal(tskey->subkey.tag, PGP_PKT_SECRET_SUBKEY);
    assert_rnp_success(decrypt_secret_key(&tskey->subkey, "password"));

    /* import secret key with 2 uids and 2 subkeys */
    cfg.set_str(CFG_KEYFILE, MERGE_PATH "key-sec.pgp");
    assert_true(cli_rnp_add_key(&rnp));
    assert_true(cli_rnp_save_keyrings(&rnp));
    assert_rnp_success(rnp_get_public_key_count(rnp.ffi, &keycount));
    assert_int_equal(keycount, 3);
    assert_rnp_success(rnp_get_secret_key_count(rnp.ffi, &keycount));
    assert_int_equal(keycount, 3);

    assert_true(load_transferable_key(&tkey, ".rnp/pubring.gpg"));
    assert_int_equal(tkey.subkeys.size(), 2);
    assert_true(tkey.signatures.empty());
    assert_int_equal(tkey.userids.size(), 2);
    assert_int_equal(tkey.key.tag, PGP_PKT_PUBLIC_KEY);
    assert_non_null(tuid = &tkey.userids.front());
    assert_int_equal(tuid->signatures.size(), 1);
    assert_false(memcmp(tuid->uid.uid.data(), "key-merge-uid-1", 15));
    assert_int_equal(tuid->uid.tag, PGP_PKT_USER_ID);
    assert_non_null(tuid = &tkey.userids[1]);
    assert_int_equal(tuid->signatures.size(), 1);
    assert_false(memcmp(tuid->uid.uid.data(), "key-merge-uid-2", 15));
    assert_int_equal(tuid->uid.tag, PGP_PKT_USER_ID);
    assert_non_null(tskey = &tkey.subkeys.front());
    assert_int_equal(tskey->signatures.size(), 1);
    assert_int_equal(tskey->subkey.tag, PGP_PKT_PUBLIC_SUBKEY);
    assert_non_null(tskey = &tkey.subkeys[1]);
    assert_int_equal(tskey->signatures.size(), 1);
    assert_int_equal(tskey->subkey.tag, PGP_PKT_PUBLIC_SUBKEY);

    assert_true(load_transferable_key(&tkey, ".rnp/secring.gpg"));
    assert_int_equal(tkey.subkeys.size(), 2);
    assert_true(tkey.signatures.empty());
    assert_int_equal(tkey.userids.size(), 2);
    assert_int_equal(tkey.key.tag, PGP_PKT_SECRET_KEY);
    assert_rnp_success(decrypt_secret_key(&tkey.key, "password"));
    assert_non_null(tuid = &tkey.userids.front());
    assert_int_equal(tuid->signatures.size(), 1);
    assert_false(memcmp(tuid->uid.uid.data(), "key-merge-uid-1", 15));
    assert_int_equal(tuid->uid.tag, PGP_PKT_USER_ID);
    assert_non_null(tuid = &tkey.userids[1]);
    assert_int_equal(tuid->signatures.size(), 1);
    assert_false(memcmp(tuid->uid.uid.data(), "key-merge-uid-2", 15));
    assert_int_equal(tuid->uid.tag, PGP_PKT_USER_ID);
    assert_non_null(tskey = &tkey.subkeys.front());
    assert_int_equal(tskey->signatures.size(), 1);
    assert_int_equal(tskey->subkey.tag, PGP_PKT_SECRET_SUBKEY);
    assert_rnp_success(decrypt_secret_key(&tskey->subkey, "password"));
    assert_non_null(tskey = &tkey.subkeys[1]);
    assert_int_equal(tskey->signatures.size(), 1);
    assert_int_equal(tskey->subkey.tag, PGP_PKT_SECRET_SUBKEY);
    assert_rnp_success(decrypt_secret_key(&tskey->subkey, "password"));

    rnp.end();
}

TEST_F(rnp_tests, test_load_subkey)
{
    auto        key_store = new rnp::KeyStore("", global_ctx);
    std::string keyid = "9747D2A6B3A63124";
    std::string sub1id = "AF1114A47F5F5B28";
    std::string sub2id = "16CD16F267CCDD4F";

    /* load first subkey with signature */
    rnp::Key *key = nullptr, *skey1 = nullptr, *skey2 = nullptr;
    assert_true(load_keystore(key_store, MERGE_PATH "key-pub-just-subkey-1.pgp"));
    assert_int_equal(key_store->key_count(), 1);
    assert_non_null(skey1 = rnp_tests_get_key_by_id(key_store, sub1id));
    assert_false(skey1->valid());
    assert_int_equal(skey1->rawpkt_count(), 2);
    assert_int_equal(skey1->rawpkt().tag(), PGP_PKT_PUBLIC_SUBKEY);
    assert_int_equal(skey1->get_sig(0).raw.tag(), PGP_PKT_SIGNATURE);
    assert_false(skey1->has_primary_fp());

    /* load second subkey, without signature */
    assert_true(load_keystore(key_store, MERGE_PATH "key-pub-just-subkey-2-no-sigs.pgp"));
    assert_int_equal(key_store->key_count(), 2);
    assert_non_null(skey2 = rnp_tests_get_key_by_id(key_store, sub2id));
    assert_false(skey2->valid());
    assert_int_equal(skey2->rawpkt_count(), 1);
    assert_int_equal(skey2->rawpkt().tag(), PGP_PKT_PUBLIC_SUBKEY);
    assert_false(skey2->has_primary_fp());
    assert_false(skey1 == skey2);

    /* load primary key without subkey signatures */
    assert_true(load_keystore(key_store, MERGE_PATH "key-pub-uid-1.pgp"));
    assert_int_equal(key_store->key_count(), 3);
    assert_non_null(key = rnp_tests_get_key_by_id(key_store, keyid));
    assert_true(key->valid());
    assert_int_equal(key->rawpkt_count(), 3);
    assert_int_equal(key->rawpkt().tag(), PGP_PKT_PUBLIC_KEY);
    assert_int_equal(key->get_uid(0).rawpkt.tag(), PGP_PKT_USER_ID);
    assert_int_equal(key->get_sig(0).raw.tag(), PGP_PKT_SIGNATURE);
    assert_true(skey1 == rnp_tests_get_key_by_id(key_store, sub1id));
    assert_true(skey2 == rnp_tests_get_key_by_id(key_store, sub2id));
    assert_true(skey1->has_primary_fp());
    assert_true(check_subkey_fp(key, skey1, 0));
    assert_int_equal(key->subkey_count(), 1);
    assert_true(skey1->valid());
    assert_false(skey2->valid());

    /* load second subkey with signature */
    assert_true(load_keystore(key_store, MERGE_PATH "key-pub-just-subkey-2.pgp"));
    assert_int_equal(key_store->key_count(), 3);
    assert_true(key == rnp_tests_get_key_by_id(key_store, keyid));
    assert_true(skey1 == rnp_tests_get_key_by_id(key_store, sub1id));
    assert_true(skey2 == rnp_tests_get_key_by_id(key_store, sub2id));
    assert_true(skey2->has_primary_fp());
    assert_true(check_subkey_fp(key, skey2, 1));
    assert_int_equal(key->subkey_count(), 2);
    assert_true(skey2->valid());

    delete key_store;
}
