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

#include "rnp_tests.h"
#include "support.h"
#include "time-utils.h"

#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#else
#ifndef WIFEXITED
#define WIFEXITED(stat) (((*((int *) &(stat))) & 0xC0000000) == 0)
#endif

#ifndef WEXITSTATUS
#define WEXITSTATUS(stat) (*((int *) &(stat)))
#endif
#endif

#ifdef _WIN32
#include <windows.h>
#include "str-utils.h"
#endif

int rnp_main(int argc, char **argv);
int rnpkeys_main(int argc, char **argv);

static int
call_rnp(const char *cmd, ...)
{
    int     argc = 0;
    int     res;
    char ** argv = (char **) calloc(32, sizeof(char *));
    va_list args;

    if (!argv) {
        return -1;
    }

    va_start(args, cmd);
    while (cmd) {
        argv[argc++] = (char *) cmd;
        cmd = va_arg(args, char *);
    }
    va_end(args);
    /* reset state of getopt_long used in rnp */
    optind = 1;

    if (!strcmp(argv[0], "rnp")) {
        res = rnp_main(argc, argv);
    } else if (!strcmp(argv[0], "rnpkeys")) {
        res = rnpkeys_main(argc, argv);
    } else {
        res = -1;
    }
    free(argv);

    return res;
}

#define KEYS "data/keyrings"
#define GENKEYS "data/keyrings_genkey_tmp"
#define MKEYS "data/test_stream_key_merge/"
#define FILES "data/test_cli"
#define G10KEYS "data/test_stream_key_load/g10"

TEST_F(rnp_tests, test_cli_rnp_keyfile)
{
    int ret;

    /* sign with keyfile, using default key */
    ret = call_rnp("rnp",
                   "--keyfile",
                   MKEYS "key-sec.asc",
                   "--password",
                   "password",
                   "-s",
                   FILES "/hello.txt",
                   NULL);
    assert_int_equal(ret, 0);
    assert_true(rnp_file_exists(FILES "/hello.txt.pgp"));
    /* verify signed file */
    ret =
      call_rnp("rnp", "--keyfile", MKEYS "key-pub.asc", "-v", FILES "/hello.txt.pgp", NULL);
    assert_int_equal(ret, 0);
    assert_int_equal(rnp_unlink(FILES "/hello.txt.pgp"), 0);

    /* sign with keyfile, using user id */
    ret = call_rnp("rnp",
                   "-f",
                   MKEYS "key-sec.asc",
                   "-u",
                   "key-merge-uid-2",
                   "--password",
                   "password",
                   "--armor",
                   "-s",
                   FILES "/hello.txt",
                   NULL);
    assert_int_equal(ret, 0);
    assert_true(rnp_file_exists(FILES "/hello.txt.asc"));
    /* verify signed file */
    ret = call_rnp("rnp", "-f", MKEYS "key-pub.asc", "-v", FILES "/hello.txt.asc", NULL);
    assert_int_equal(ret, 0);
    /* verify with key without self-signature - should fail */
    ret =
      call_rnp("rnp", "-f", MKEYS "key-pub-just-key.pgp", "-v", FILES "/hello.txt.asc", NULL);
    assert_int_not_equal(ret, 0);
    assert_int_equal(rnp_unlink(FILES "/hello.txt.asc"), 0);

    /* encrypt with keyfile, using default key */
    ret = call_rnp("rnp", "--keyfile", MKEYS "key-pub.asc", "-e", FILES "/hello.txt", NULL);
    assert_int_equal(ret, 0);
    assert_true(rnp_file_exists(FILES "/hello.txt.pgp"));
    /* decrypt it with raw seckey, without userids and sigs */
    ret = call_rnp("rnp",
                   "--keyfile",
                   MKEYS "key-sec-no-uid-no-sigs.pgp",
                   "--password",
                   "password",
                   "-d",
                   FILES "/hello.txt.pgp",
                   "--output",
                   "-",
                   NULL);
    assert_int_equal(ret, 0);
    assert_int_equal(rnp_unlink(FILES "/hello.txt.pgp"), 0);

    /* try to encrypt with keyfile, using the signing subkey */
    ret = call_rnp("rnp",
                   "--keyfile",
                   MKEYS "key-pub.asc",
                   "-r",
                   "16CD16F267CCDD4F",
                   "--armor",
                   "-e",
                   FILES "/hello.txt",
                   NULL);
    assert_int_not_equal(ret, 0);
    assert_false(rnp_file_exists(FILES "/hello.txt.asc"));
    /* now encrypt with keyfile, using the encrypting subkey */
    ret = call_rnp("rnp",
                   "--keyfile",
                   MKEYS "key-pub.asc",
                   "-r",
                   "AF1114A47F5F5B28",
                   "--armor",
                   "-e",
                   FILES "/hello.txt",
                   NULL);
    assert_int_equal(ret, 0);
    assert_true(rnp_file_exists(FILES "/hello.txt.asc"));
    /* fail to decrypt it with pubkey */
    ret = call_rnp("rnp",
                   "--keyfile",
                   MKEYS "key-pub-subkey-1.pgp",
                   "--password",
                   "password",
                   "-d",
                   FILES "/hello.txt.asc",
                   "--output",
                   "-",
                   NULL);
    assert_int_not_equal(ret, 0);
    /* decrypt correctly with seckey + subkeys */
    ret = call_rnp("rnp",
                   "--keyfile",
                   MKEYS "key-sec.pgp",
                   "--password",
                   "password",
                   "-d",
                   FILES "/hello.txt.asc",
                   "--output",
                   "-",
                   NULL);
    assert_int_equal(ret, 0);
    assert_int_equal(rnp_unlink(FILES "/hello.txt.asc"), 0);
}

static bool
test_cli_g10_key_sign(const char *userid)
{
    /* create signature */
    int ret = call_rnp("rnp",
                       "--homedir",
                       G10KEYS,
                       "--password",
                       "password",
                       "-u",
                       userid,
                       "-s",
                       FILES "/hello.txt",
                       NULL);
    if (ret) {
        rnp_unlink(FILES "/hello.txt.pgp");
        return false;
    }

    /* verify back */
    ret = call_rnp(
      "rnp", "--homedir", G10KEYS, "-v", FILES "/hello.txt.pgp", "--output", "-", NULL);
    rnp_unlink(FILES "/hello.txt.pgp");
    return !ret;
}

static bool
test_cli_g10_key_encrypt(const char *userid)
{
    /* encrypt */
    int ret =
      call_rnp("rnp", "--homedir", G10KEYS, "-r", userid, "-e", FILES "/hello.txt", NULL);
    if (ret) {
        rnp_unlink(FILES "/hello.txt.pgp");
        return false;
    }

    /* decrypt it back */
    ret = call_rnp("rnp",
                   "--homedir",
                   G10KEYS,
                   "--password",
                   "password",
                   "-d",
                   FILES "/hello.txt.pgp",
                   "--output",
                   "-",
                   NULL);
    rnp_unlink(FILES "/hello.txt.pgp");
    return !ret;
}

TEST_F(rnp_tests, test_cli_g10_operations)
{
    int ret;

    /* sign with default g10 key */
    ret = call_rnp(
      "rnp", "--homedir", G10KEYS, "--password", "password", "-s", FILES "/hello.txt", NULL);
    assert_int_equal(ret, 0);

    /* verify back */
    ret = call_rnp("rnp", "--homedir", G10KEYS, "-v", FILES "/hello.txt.pgp", NULL);
    assert_int_equal(ret, 0);
    assert_int_equal(rnp_unlink(FILES "/hello.txt.pgp"), 0);

    /* encrypt with default g10 key */
    ret = call_rnp("rnp", "--homedir", G10KEYS, "-e", FILES "/hello.txt", NULL);
    assert_int_equal(ret, 0);

    /* decrypt it back */
    ret = call_rnp("rnp",
                   "--homedir",
                   G10KEYS,
                   "--password",
                   "password",
                   "-d",
                   FILES "/hello.txt.pgp",
                   "--output",
                   "-",
                   NULL);
    assert_int_equal(ret, 0);
    assert_int_equal(rnp_unlink(FILES "/hello.txt.pgp"), 0);

    /* check dsa/eg key */
    assert_true(test_cli_g10_key_sign("c8a10a7d78273e10"));    // signing key
    assert_true(test_cli_g10_key_encrypt("c8a10a7d78273e10")); // will find subkey
    assert_false(test_cli_g10_key_sign("02a5715c3537717e"));   // fail - encrypting subkey
    assert_true(test_cli_g10_key_encrypt("02a5715c3537717e")); // success

    /* check rsa/rsa key, key is SC while subkey is E. Must succeed till year 2024 */
    assert_true(test_cli_g10_key_sign("2fb9179118898e8b"));
    assert_true(test_cli_g10_key_encrypt("2fb9179118898e8b"));
    assert_false(test_cli_g10_key_sign("6e2f73008f8b8d6e"));
    assert_true(test_cli_g10_key_encrypt("6e2f73008f8b8d6e"));

#ifdef CRYPTO_BACKEND_BOTAN
    /*  GnuPG extended key format requires AEAD support that is available for BOTAN backend
       only https://github.com/rnpgp/rnp/issues/1642 (???)
    */
    /* check new rsa/rsa key, key is SC while subkey is E. */
    assert_true(test_cli_g10_key_sign("bd860a52d1899c0f"));
    assert_true(test_cli_g10_key_encrypt("bd860a52d1899c0f"));
    assert_false(test_cli_g10_key_sign("8e08d46a37414996"));
    assert_true(test_cli_g10_key_encrypt("8e08d46a37414996"));
#endif

    /* check ed25519 key */
    assert_true(test_cli_g10_key_sign("cc786278981b0728"));
    assert_false(test_cli_g10_key_encrypt("cc786278981b0728"));

    /* check ed25519/x25519 key */
    assert_true(test_cli_g10_key_sign("941822a0fc1b30a5"));
    assert_true(test_cli_g10_key_encrypt("941822a0fc1b30a5"));
    assert_false(test_cli_g10_key_sign("c711187e594376af"));
    assert_true(test_cli_g10_key_encrypt("c711187e594376af"));

    /* check p256 key */
    assert_true(test_cli_g10_key_sign("23674f21b2441527"));
    assert_true(test_cli_g10_key_encrypt("23674f21b2441527"));
    assert_false(test_cli_g10_key_sign("37e285e9e9851491"));
    assert_true(test_cli_g10_key_encrypt("37e285e9e9851491"));

    /* check p384 key */
    assert_true(test_cli_g10_key_sign("242a3aa5ea85f44a"));
    assert_true(test_cli_g10_key_encrypt("242a3aa5ea85f44a"));
    assert_false(test_cli_g10_key_sign("e210e3d554a4fad9"));
    assert_true(test_cli_g10_key_encrypt("e210e3d554a4fad9"));

    /* check p521 key */
    assert_true(test_cli_g10_key_sign("2092ca8324263b6a"));
    assert_true(test_cli_g10_key_encrypt("2092ca8324263b6a"));
    assert_false(test_cli_g10_key_sign("9853df2f6d297442"));
    assert_true(test_cli_g10_key_encrypt("9853df2f6d297442"));

    /* check bp256 key */
    assert_true(test_cli_g10_key_sign("d0c8a3daf9e0634a") == brainpool_enabled());
    assert_true(test_cli_g10_key_encrypt("d0c8a3daf9e0634a") == brainpool_enabled());
    assert_false(test_cli_g10_key_sign("2edabb94d3055f76"));
    assert_true(test_cli_g10_key_encrypt("2edabb94d3055f76") == brainpool_enabled());

    /* check bp384 key */
    assert_true(test_cli_g10_key_sign("6cf2dce85599ada2") == brainpool_enabled());
    assert_true(test_cli_g10_key_encrypt("6cf2dce85599ada2") == brainpool_enabled());
    assert_false(test_cli_g10_key_sign("cff1bb6f16d28191"));
    assert_true(test_cli_g10_key_encrypt("cff1bb6f16d28191") == brainpool_enabled());

    /* check bp512 key */
    assert_true(test_cli_g10_key_sign("aa5c58d14f7b8f48") == brainpool_enabled());
    assert_true(test_cli_g10_key_encrypt("aa5c58d14f7b8f48") == brainpool_enabled());
    assert_false(test_cli_g10_key_sign("20cdaa1482ba79ce"));
    assert_true(test_cli_g10_key_encrypt("20cdaa1482ba79ce") == brainpool_enabled());

    /* check secp256k1 key */
    assert_true(test_cli_g10_key_sign("3ea5bb6f9692c1a0"));
    assert_true(test_cli_g10_key_encrypt("3ea5bb6f9692c1a0"));
    assert_false(test_cli_g10_key_sign("7635401f90d3e533"));
    assert_true(test_cli_g10_key_encrypt("7635401f90d3e533"));
}

TEST_F(rnp_tests, test_cli_rnpkeys_unicode)
{
#ifdef _WIN32
    std::string  uid_acp = "\x80@a.com";
    std::wstring uid2_wide =
      L"\x03C9\x0410@b.com"; // some Greek and Cyrillic for CreateProcessW test
    std::string homedir_s = std::string(m_dir) + "/unicode";
    rnp_mkdir(homedir_s.c_str());
    std::string path_s = rnp::path::append(original_dir(), "../rnpkeys/rnpkeys.exe");
    std::string cmdline_s = path_s + " --numbits 2048 --homedir " + homedir_s +
                            " --password password --userid " + uid_acp + " --generate-key";
    UINT         acp = GetACP();
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof pi);
    BOOL res = CreateProcessA(NULL, // (LPSTR) path_s.c_str(), // Module name
                              (LPSTR) cmdline_s.c_str(), // Command line
                              NULL,                      // Process handle not inheritable
                              NULL,                      // Thread handle not inheritable
                              FALSE,                     // Handle inheritance
                              0,                         // Creation flags
                              NULL,                      // Use parent's environment block
                              NULL,                      // Use parent's starting directory
                              &si,                       // Pointer to STARTUPINFO structure
                              &pi); // Pointer to PROCESS_INFORMATION structure
    assert_true(res);
    assert_true(WaitForSingleObject(pi.hProcess, INFINITE) == WAIT_OBJECT_0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    std::wstring homedir_ws = wstr_from_utf8(homedir_s);
    std::wstring path_ws = wstr_from_utf8(path_s);
    std::wstring cmdline_ws = path_ws + L" --numbits 2048 --homedir " + homedir_ws +
                              L" --password password --userid " + uid2_wide +
                              L" --generate-key";
    STARTUPINFOW siw;
    ZeroMemory(&siw, sizeof siw);
    ZeroMemory(&pi, sizeof pi);
    res = CreateProcessW(NULL,
                         (LPWSTR) cmdline_ws.c_str(), // Command line
                         NULL,                        // Process handle not inheritable
                         NULL,                        // Thread handle not inheritable
                         FALSE,                       // Handle inheritance
                         0,                           // Creation flags
                         NULL,                        // Use parent's environment block
                         NULL,                        // Use parent's starting directory
                         &siw,                        // Pointer to STARTUPINFO structure
                         &pi); // Pointer to PROCESS_INFORMATION structure
    assert_true(res);
    assert_true(WaitForSingleObject(pi.hProcess, INFINITE) == WAIT_OBJECT_0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    // Load the keyring and check what was actually written
    rnp_ffi_t ffi;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    rnp_input_t input = NULL;
    assert_rnp_success(rnp_input_from_path(&input, "unicode/pubring.gpg"));
    assert_rnp_success(rnp_load_keys(ffi, "GPG", input, RNP_LOAD_SAVE_PUBLIC_KEYS));
    rnp_input_destroy(input);

    // convert from ACP to wide char via Windows native mechanism
    int convertResult = MultiByteToWideChar(acp, 0, uid_acp.c_str(), uid_acp.size(), NULL, 0);
    assert_true(convertResult > 0);
    std::wstring uid_wide;
    uid_wide.resize(convertResult);
    convertResult = MultiByteToWideChar(
      acp, 0, uid_acp.c_str(), uid_acp.size(), &uid_wide[0], (int) uid_wide.size());
    assert_true(convertResult > 0);

    // we expect to find UID in UTF-8
    std::string      uid_utf8 = wstr_to_utf8(uid_wide);
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", uid_utf8.c_str(), &key));
    assert_non_null(key);

    size_t uids = 0;
    assert_rnp_success(rnp_key_get_uid_count(key, &uids));
    assert_int_equal(uids, 1);

    rnp_uid_handle_t uid = NULL;
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_non_null(uid);

    size_t size = 0;
    char * data = NULL;
    assert_rnp_success(rnp_uid_get_data(uid, (void **) &data, &size));
    std::string uid_read(data, data + size);
    assert_int_equal(0, uid_read.compare(uid_utf8));
    rnp_buffer_destroy(data);
    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(key);

    uid_utf8 = wstr_to_utf8(uid2_wide);
    key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", uid_utf8.c_str(), &key));
    assert_non_null(key);

    uids = 0;
    assert_rnp_success(rnp_key_get_uid_count(key, &uids));
    assert_int_equal(uids, 1);

    uid = NULL;
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_non_null(uid);

    size = 0;
    data = NULL;
    assert_rnp_success(rnp_uid_get_data(uid, (void **) &data, &size));
    std::string uid2_read(data, data + size);
    assert_int_equal(0, uid2_read.compare(uid_utf8));
    rnp_buffer_destroy(data);
    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(key);
    rnp_ffi_destroy(ffi);
#endif
}

TEST_F(rnp_tests, test_cli_rnp)
{
    int ret;
    assert_int_equal(0, call_rnp("rnp", "--version", NULL));

    /* sign with default key */
    ret = call_rnp("rnp",
                   "--homedir",
                   KEYS "/1",
                   "--password",
                   "password",
                   "--sign",
                   FILES "/hello.txt",
                   NULL);
    assert_int_equal(ret, 0);

    /* encrypt with default key */
    ret = call_rnp(
      "rnp", "--homedir", KEYS "/1", "--encrypt", FILES "/hello.txt", "--overwrite", NULL);
    assert_int_equal(ret, 0);

    /* sign and verify back with g10 key */
    ret = call_rnp("rnp",
                   "--homedir",
                   KEYS "/3",
                   "-u",
                   "4BE147BB22DF1E60",
                   "--password",
                   "password",
                   "--sign",
                   FILES "/hello.txt",
                   "--overwrite",
                   NULL);
    assert_int_equal(ret, 0);
    ret = call_rnp("rnp", "--homedir", KEYS "/3", "--verify", FILES "/hello.txt.pgp", NULL);
    assert_int_equal(ret, 0);

    /* encrypt and decrypt back with g10 key */
    ret = call_rnp("rnp",
                   "--homedir",
                   KEYS "/3",
                   "-r",
                   "4BE147BB22DF1E60",
                   "--encrypt",
                   FILES "/hello.txt",
                   "--overwrite",
                   NULL);
    assert_int_equal(ret, 0);
    ret = call_rnp("rnp",
                   "--homedir",
                   KEYS "/3",
                   "--password",
                   "password",
                   "--decrypt",
                   FILES "/hello.txt.pgp",
                   "--output",
                   "-",
                   NULL);
    assert_int_equal(ret, 0);
}

TEST_F(rnp_tests, test_cli_examples)
{
    auto examples_path = rnp::path::append(original_dir(), "../examples");
    /* key generation example */
    auto example_path = rnp::path::append(examples_path, "generate");
    assert_false(example_path.empty());
    assert_int_equal(system(example_path.c_str()), 0);

    /* encryption sample */
    example_path = rnp::path::append(examples_path, "encrypt");
    assert_false(example_path.empty());
    assert_int_equal(system(example_path.c_str()), 0);

    /* decryption sample */
    example_path = rnp::path::append(examples_path, "decrypt");
    assert_false(example_path.empty());
    assert_int_equal(system(example_path.c_str()), 0);

    /* signing sample */
    example_path = rnp::path::append(examples_path, "sign");
    assert_false(example_path.empty());
    assert_int_equal(system(example_path.c_str()), 0);

    /* verification sample */
    example_path = rnp::path::append(examples_path, "verify");
    assert_false(example_path.empty());
    assert_int_equal(system(example_path.c_str()), 0);
}

TEST_F(rnp_tests, test_cli_rnpkeys)
{
    int ret;
    assert_int_equal(0, call_rnp("rnpkeys", "--version", NULL));

    /* test keys listing */
    ret = call_rnp("rnpkeys", "--homedir", KEYS "/1", "--list-keys", NULL);
    assert_int_equal(ret, 0);

    ret = call_rnp("rnpkeys", "--homedir", KEYS "/1", "--list-keys", "--with-sigs", NULL);
    assert_int_equal(ret, 0);

    ret = call_rnp("rnpkeys", "--homedir", KEYS "/2", "--list-keys", NULL);
    assert_int_equal(ret, 0);

    ret = call_rnp("rnpkeys", "--homedir", KEYS "/2", "--list-keys", "--with-sigs", NULL);
    assert_int_equal(ret, 0);

    ret = call_rnp("rnpkeys", "--homedir", KEYS "/3", "--list-keys", NULL);
    assert_int_equal(ret, 0);

    ret = call_rnp("rnpkeys", "--homedir", KEYS "/3", "--list-keys", "--with-sigs", NULL);
    assert_int_equal(ret, 0);

    ret = call_rnp("rnpkeys", "--homedir", KEYS "/5", "--list-keys", NULL);
    assert_int_equal(ret, 0);

    ret = call_rnp("rnpkeys", "--homedir", KEYS "/5", "--list-keys", "--with-sigs", NULL);
    assert_int_equal(ret, 0);

    ret = call_rnp("rnpkeys", "--homedir", G10KEYS, "--list-keys", NULL);
    assert_int_equal(ret, 0);

    ret = call_rnp("rnpkeys", "--homedir", G10KEYS, "--list-keys", "--with-sigs", NULL);
    assert_int_equal(ret, 0);

    /* test single key listing command */
    ret = call_rnp("rnpkeys", "--homedir", KEYS "/1", "--list-keys", "2fcadf05ffa501bb", NULL);
    assert_int_equal(ret, 0);

    ret = call_rnp("rnpkeys", "--homedir", KEYS "/1", "--list-keys", "00000000", NULL);
    assert_int_not_equal(ret, 0);

    ret = call_rnp("rnpkeys", "--homedir", KEYS "/1", "--list-keys", "zzzzzzzz", NULL);
    assert_int_not_equal(ret, 0);
}

// check both primary key and subkey for the given userid
static int
key_expiration_check(rnp::KeyStore *keystore, const char *userid, uint32_t expectedExpiration)
{
    int res = -1; // not found
    for (auto &key : keystore->keys) {
        rnp::Key *pk;
        if (key.is_primary()) {
            pk = &key;
        } else {
            if (!key.has_primary_fp()) {
                return 0;
            }
            pk = keystore->get_key(key.primary_fp());
            if (!pk) {
                return 0;
            }
        }
        if (pk->uid_count() != 1) {
            return 0;
        }
        auto uid = pk->get_uid(0).str;
        if (uid != userid) {
            continue;
        }
        auto expiration = key.expiration();
        if (uid == "expiration_absolute@rnp" || uid == "expiration_beyond2038_absolute@rnp") {
            auto diff = expectedExpiration < expiration ? expiration - expectedExpiration :
                                                          expectedExpiration - expiration;
            // allow 10 minutes diff
            if (diff < 600) {
                res = 1;
            } else {
                return 0;
            }
        } else {
            if (expectedExpiration == expiration) {
                res = 1;
            } else {
                RNP_LOG(
                  "key_expiration_check error: userid=%s expectedExpiration=%u expiration=%u",
                  userid,
                  expectedExpiration,
                  expiration);
                return 0;
            }
        }
    }
    return res;
}

static int
key_generate(const char *homedir, const char *userid, const char *expiration)
{
    int ret = call_rnp("rnpkeys",
                       "--password",
                       "1234",
                       "--homedir",
                       homedir,
                       "--generate-key",
                       "--expiration",
                       expiration,
                       "--userid",
                       userid,
                       "--s2k-iterations",
                       "65536",
                       "--numbits",
                       "1024",
                       NULL);
    return ret;
}

TEST_F(rnp_tests, test_cli_rnpkeys_genkey)
{
    assert_false(RNP_MKDIR(GENKEYS, S_IRWXU));
    time_t   basetime = time(NULL);
    time_t   rawtime = basetime + 604800;
    time_t   y2k38time = INT32_MAX;
    uint32_t expected_diff_beyond2038_absolute;
    if (rnp_y2k38_warning(y2k38time)) {
        // we're on the system that doesn't support dates beyond y2k38
        auto diff_to_y2k38 = y2k38time - basetime;
        expected_diff_beyond2038_absolute = diff_to_y2k38;
    } else {
        struct tm tm2100;
        rnp_localtime(time(NULL), tm2100);
        tm2100.tm_hour = 0;
        tm2100.tm_min = 0;
        tm2100.tm_sec = 0;
        tm2100.tm_mday = 1;
        tm2100.tm_mon = 0;
        tm2100.tm_year = 200;
        /* line below is required to correctly handle DST changes */
        tm2100.tm_isdst = -1;
        expected_diff_beyond2038_absolute = mktime(&tm2100) - basetime;
    }
    struct tm timeinfo;
    rnp_localtime(rawtime, timeinfo);
    // clear hours, minutes and seconds
    timeinfo.tm_hour = 0;
    timeinfo.tm_min = 0;
    timeinfo.tm_sec = 0;
    rawtime = mktime(&timeinfo);
    auto exp =
      fmt("%d-%02d-%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);

    // these should fail and not go to the keystore
    assert_int_not_equal(key_generate(GENKEYS, "expiration_negative@rnp", "-1"), 0);
    assert_int_not_equal(key_generate(GENKEYS, "expiration_unrecognized_1@rnp", "1z"), 0);
    assert_int_not_equal(key_generate(GENKEYS, "expiration_unrecognized_2@rnp", "now"), 0);
    assert_int_not_equal(key_generate(GENKEYS, "expiration_unrecognized_3@rnp", "00000-01-01"),
                         0);
    assert_int_not_equal(
      key_generate(GENKEYS, "expiration_integer_overflow@rnp", "1234567890123456789"), 0);
    assert_int_not_equal(key_generate(GENKEYS, "expiration_32bit_overflow@rnp", "4294967296"),
                         0);
    assert_int_not_equal(key_generate(GENKEYS, "expiration_overflow_day@rnp", "2037-02-29"),
                         0);
    assert_int_not_equal(key_generate(GENKEYS, "expiration_overflow_month@rnp", "2037-13-01"),
                         0);
    if (!rnp_y2k38_warning(y2k38time)) {
        assert_int_not_equal(
          key_generate(GENKEYS, "expiration_overflow_year@rnp", "2337-01-01"), 0);
    }
    assert_int_not_equal(key_generate(GENKEYS, "expiration_underflow_day@rnp", "2037-02-00"),
                         0);
    assert_int_not_equal(key_generate(GENKEYS, "expiration_underflow_month@rnp", "2037-00-01"),
                         0);
    assert_int_not_equal(key_generate(GENKEYS, "expiration_underflow_year@rnp", "1800-01-01"),
                         0);
    assert_int_not_equal(key_generate(GENKEYS, "expiration_overflow@rnp", "200y"), 0);
    assert_int_not_equal(key_generate(GENKEYS, "expiration_past@rnp", "2021-01-01"), 0);

    // these should pass and go to the keystore -- 17 primary keys and 17 subkeys
    assert_int_equal(key_generate(GENKEYS, "expiration_beyond2038_relative@rnp", "20y"), 0);
    assert_int_equal(key_generate(GENKEYS, "expiration_beyond2038_absolute@rnp", "2100-01-01"),
                     0);
    assert_int_equal(key_generate(GENKEYS, "expiration_absolute@rnp", exp.c_str()), 0);
    assert_int_equal(key_generate(GENKEYS, "expiration_max_32bit@rnp", "4294967295"), 0);
    assert_int_equal(key_generate(GENKEYS, "expiration_max_32bit_h@rnp", "1193046h"), 0);
    assert_int_equal(key_generate(GENKEYS, "expiration_1sec@rnp", "1"), 0);
    assert_int_equal(key_generate(GENKEYS, "expiration_1hour@rnp", "1h"), 0);
    assert_int_equal(key_generate(GENKEYS, "expiration_1day@rnp", "1d"), 0);
    assert_int_equal(key_generate(GENKEYS, "expiration_1week@rnp", "1w"), 0);
    assert_int_equal(key_generate(GENKEYS, "expiration_1month@rnp", "1m"), 0);
    assert_int_equal(key_generate(GENKEYS, "expiration_1year@rnp", "1y"), 0);
    assert_int_equal(key_generate(GENKEYS, "expiration_2sec@rnp", "2"), 0);
    assert_int_equal(key_generate(GENKEYS, "expiration_2hours@rnp", "2h"), 0);
    assert_int_equal(key_generate(GENKEYS, "expiration_2days@rnp", "2d"), 0);
    assert_int_equal(key_generate(GENKEYS, "expiration_2weeks@rnp", "2w"), 0);
    assert_int_equal(key_generate(GENKEYS, "expiration_2months@rnp", "2m"), 0);
    assert_int_equal(key_generate(GENKEYS, "expiration_2years@rnp", "2y"), 0);

    auto         keystore = new rnp::KeyStore("", global_ctx);
    pgp_source_t src = {};
    assert_rnp_success(init_file_src(&src, GENKEYS "/pubring.gpg"));
    assert_true(keystore->load(src));
    assert_int_equal(keystore->key_count(), 34);
    src.close();
    assert_int_equal(key_expiration_check(keystore, "expiration_max_32bit@rnp", 4294967295),
                     1);
    assert_int_equal(key_expiration_check(keystore, "expiration_max_32bit_h@rnp", 4294965600),
                     1);
    assert_int_equal(key_expiration_check(keystore, "expiration_1sec@rnp", 1), 1);
    assert_int_equal(key_expiration_check(keystore, "expiration_1hour@rnp", 3600), 1);
    assert_int_equal(key_expiration_check(keystore, "expiration_1day@rnp", 86400), 1);
    assert_int_equal(key_expiration_check(keystore, "expiration_1week@rnp", 604800), 1);
    assert_int_equal(key_expiration_check(keystore, "expiration_1month@rnp", 2678400), 1);
    assert_int_equal(key_expiration_check(keystore, "expiration_1year@rnp", 31536000), 1);
    assert_int_equal(key_expiration_check(keystore, "expiration_2sec@rnp", 2), 1);
    assert_int_equal(key_expiration_check(keystore, "expiration_2hours@rnp", 7200), 1);
    assert_int_equal(key_expiration_check(keystore, "expiration_2days@rnp", 172800), 1);
    assert_int_equal(key_expiration_check(keystore, "expiration_2weeks@rnp", 1209600), 1);
    assert_int_equal(key_expiration_check(keystore, "expiration_2months@rnp", 5356800), 1);
    assert_int_equal(key_expiration_check(keystore, "expiration_2years@rnp", 63072000), 1);
    assert_int_equal(
      key_expiration_check(keystore, "expiration_absolute@rnp", rawtime - basetime), 1);
    assert_int_equal(key_expiration_check(keystore,
                                          "expiration_beyond2038_absolute@rnp",
                                          expected_diff_beyond2038_absolute),
                     1);
    assert_int_equal(
      key_expiration_check(keystore, "expiration_beyond2038_relative@rnp", 630720000), 1);

    delete keystore;
    delete_recursively(GENKEYS);
}

TEST_F(rnp_tests, test_cli_dump)
{
    auto dump_path = rnp::path::append(original_dir(), "../examples/dump");
    char cmd[512] = {0};
    int  chnum;
    int  status;
    /* call dump's help */
    chnum = snprintf(cmd, sizeof(cmd), "%s -h", dump_path.c_str());
    assert_true(chnum < (int) sizeof(cmd));
    status = system(cmd);
    assert_true(WIFEXITED(status));
    assert_int_equal(WEXITSTATUS(status), 1);
    /* run dump on some data */
    chnum = snprintf(cmd, sizeof(cmd), "%s \"%s\"", dump_path.c_str(), KEYS "/1/pubring.gpg");
    assert_true(chnum < (int) sizeof(cmd));
    status = system(cmd);
    assert_true(WIFEXITED(status));
    assert_int_equal(WEXITSTATUS(status), 0);
    /* run dump on some data with json output */
    chnum =
      snprintf(cmd, sizeof(cmd), "%s -j \"%s\"", dump_path.c_str(), KEYS "/1/pubring.gpg");
    assert_true(chnum < (int) sizeof(cmd));
    status = system(cmd);
    assert_true(WIFEXITED(status));
    assert_int_equal(WEXITSTATUS(status), 0);
    /* run dump on directory - must fail but not crash */
    chnum = snprintf(cmd, sizeof(cmd), "%s \"%s\"", dump_path.c_str(), KEYS "/1/");
    assert_true(chnum < (int) sizeof(cmd));
    status = system(cmd);
    assert_true(WIFEXITED(status));
    assert_int_not_equal(WEXITSTATUS(status), 0);
}

TEST_F(rnp_tests, test_cli_logname)
{
    // getenv function is not required to be thread-safe.
    // Another call to getenv, as well as a call to the POSIX functions setenv(), unsetenv(),
    // and putenv() may invalidate the pointer returned by a previous call or modify the string
    // obtained from a previous call.
    char *      logname = getenv("LOGNAME");
    std::string saved_logname(logname ? logname : "");

    char *      user = getenv("USER");
    std::string testname(user ? user : "user");
    testname.append("-test-user");

    setenv("LOGNAME", testname.c_str(), 1);
    assert_string_equal(getenv_logname(), testname.c_str());
    if (user) {
        unsetenv("LOGNAME");
        assert_string_equal(getenv_logname(), user);
    }

    if (logname) {
        setenv("LOGNAME", saved_logname.c_str(), 1);
    } else {
        unsetenv("LOGNAME");
    }
}
