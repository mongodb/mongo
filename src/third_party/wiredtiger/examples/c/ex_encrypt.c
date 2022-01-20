/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * ex_encrypt.c
 * 	demonstrates how to use the encryption API.
 */
#include <test_util.h>

#ifdef _WIN32
/*
 * Explicitly export this function so it is visible when loading extensions.
 */
__declspec(dllexport)
#endif
  int add_my_encryptors(WT_CONNECTION *connection);

static const char *home;

#define SYS_KEYID "system"
#define SYS_PW "system_password"
#define USER1_KEYID "user1"
#define USER2_KEYID "user2"
#define USERBAD_KEYID "userbad"

#define ITEM_MATCHES(config_item, s) \
    (strlen(s) == (config_item).len && strncmp((config_item).str, s, (config_item).len) == 0)

/*! [encryption example callback implementation] */
typedef struct {
    WT_ENCRYPTOR encryptor; /* Must come first */
    int rot_N;              /* rotN value */
    uint32_t num_calls;     /* Count of calls */
    char *keyid;            /* Saved keyid */
    char *password;         /* Saved password */
} MY_CRYPTO;

#define CHKSUM_LEN 4
#define IV_LEN 16

/*
 * make_checksum --
 *     This is where one would call a checksum function on the encrypted buffer. Here we just put a
 *     constant value in it.
 */
static void
make_checksum(uint8_t *dst)
{
    int i;
    /*
     * Assume array is big enough for the checksum.
     */
    for (i = 0; i < CHKSUM_LEN; i++)
        dst[i] = 'C';
}

/*
 * make_iv --
 *     This is where one would generate the initialization vector. Here we just put a constant value
 *     in it.
 */
static void
make_iv(uint8_t *dst)
{
    int i;
    /*
     * Assume array is big enough for the initialization vector.
     */
    for (i = 0; i < IV_LEN; i++)
        dst[i] = 'I';
}

/*
 * Rotate encryption functions.
 */
/*
 * do_rotate --
 *     Perform rot-N on the buffer given.
 */
static void
do_rotate(char *buf, size_t len, int rotn)
{
    uint32_t i;
    /*
     * Now rotate
     */
    for (i = 0; i < len; i++)
        if (isalpha((unsigned char)buf[i])) {
            if (islower((unsigned char)buf[i]))
                buf[i] = ((buf[i] - 'a') + rotn) % 26 + 'a';
            else
                buf[i] = ((buf[i] - 'A') + rotn) % 26 + 'A';
        }
}

/*
 * rotate_decrypt --
 *     A simple rotate decryption.
 */
static int
rotate_decrypt(WT_ENCRYPTOR *encryptor, WT_SESSION *session, uint8_t *src, size_t src_len,
  uint8_t *dst, size_t dst_len, size_t *result_lenp)
{
    MY_CRYPTO *my_crypto = (MY_CRYPTO *)encryptor;
    size_t mylen;
    uint32_t i;

    (void)session; /* Unused */
    ++my_crypto->num_calls;

    if (src == NULL)
        return (0);
    /*
     * Make sure it is big enough.
     */
    mylen = src_len - (CHKSUM_LEN + IV_LEN);
    if (dst_len < mylen) {
        fprintf(stderr, "Rotate: ENOMEM ERROR: dst_len %zu src_len %zu\n", dst_len, src_len);
        return (ENOMEM);
    }

    /*
     * !!! Most implementations would verify any needed
     * checksum and initialize the IV here.
     */
    /*
     * Copy the encrypted data to the destination buffer and then decrypt the destination buffer in
     * place.
     */
    i = CHKSUM_LEN + IV_LEN;
    memcpy(&dst[0], &src[i], mylen);
    /*
     * Call common rotate function on the text portion of the buffer. Send in dst_len as the length
     * of the text.
     */
    /*
     * !!! Most implementations would need the IV too.
     */
    do_rotate((char *)dst, mylen, 26 - my_crypto->rot_N);
    *result_lenp = mylen;
    return (0);
}

/*
 * rotate_encrypt --
 *     A simple rotate encryption.
 */
static int
rotate_encrypt(WT_ENCRYPTOR *encryptor, WT_SESSION *session, uint8_t *src, size_t src_len,
  uint8_t *dst, size_t dst_len, size_t *result_lenp)
{
    MY_CRYPTO *my_crypto = (MY_CRYPTO *)encryptor;
    uint32_t i;

    (void)session; /* Unused */
    ++my_crypto->num_calls;

    if (src == NULL)
        return (0);
    if (dst_len < src_len + CHKSUM_LEN + IV_LEN)
        return (ENOMEM);

    i = CHKSUM_LEN + IV_LEN;
    /*
     * Skip over space reserved for checksum and initialization vector. Copy text into destination
     * buffer then encrypt in place.
     */
    memcpy(&dst[i], &src[0], src_len);
    /*
     * Call common rotate function on the text portion of the destination buffer. Send in src_len as
     * the length of the text.
     */
    do_rotate((char *)dst + i, src_len, my_crypto->rot_N);
    /*
     * Checksum the encrypted buffer and add the IV.
     */
    i = 0;
    make_checksum(&dst[i]);
    i += CHKSUM_LEN;
    make_iv(&dst[i]);
    *result_lenp = dst_len;
    return (0);
}

/*
 * rotate_sizing --
 *     A sizing example that returns the header size needed.
 */
static int
rotate_sizing(WT_ENCRYPTOR *encryptor, WT_SESSION *session, size_t *expansion_constantp)
{
    MY_CRYPTO *my_crypto = (MY_CRYPTO *)encryptor;

    (void)session; /* Unused parameters */

    ++my_crypto->num_calls; /* Call count */

    *expansion_constantp = CHKSUM_LEN + IV_LEN;
    return (0);
}

/*
 * rotate_customize --
 *     The customize function creates a customized encryptor
 */
static int
rotate_customize(WT_ENCRYPTOR *encryptor, WT_SESSION *session, WT_CONFIG_ARG *encrypt_config,
  WT_ENCRYPTOR **customp)
{
    MY_CRYPTO *my_crypto;
    WT_CONFIG_ITEM keyid, secret;
    WT_EXTENSION_API *extapi;
    int ret;
    const MY_CRYPTO *orig_crypto;

    extapi = session->connection->get_extension_api(session->connection);

    orig_crypto = (const MY_CRYPTO *)encryptor;
    if ((my_crypto = calloc(1, sizeof(MY_CRYPTO))) == NULL) {
        ret = errno;
        goto err;
    }
    *my_crypto = *orig_crypto;
    my_crypto->keyid = my_crypto->password = NULL;

    /*
     * Stash the keyid and the (optional) secret key from the configuration string.
     */
    error_check(extapi->config_get(extapi, session, encrypt_config, "keyid", &keyid));
    if (keyid.len != 0) {
        if ((my_crypto->keyid = malloc(keyid.len + 1)) == NULL) {
            ret = errno;
            goto err;
        }
        strncpy(my_crypto->keyid, keyid.str, keyid.len + 1);
        my_crypto->keyid[keyid.len] = '\0';
    }

    ret = extapi->config_get(extapi, session, encrypt_config, "secretkey", &secret);
    if (ret == 0 && secret.len != 0) {
        if ((my_crypto->password = malloc(secret.len + 1)) == NULL) {
            ret = errno;
            goto err;
        }
        strncpy(my_crypto->password, secret.str, secret.len + 1);
        my_crypto->password[secret.len] = '\0';
    }
    /*
     * Presumably we'd have some sophisticated key management here that maps the id onto a secret
     * key.
     */
    if (ITEM_MATCHES(keyid, "system")) {
        if (my_crypto->password == NULL || strcmp(my_crypto->password, SYS_PW) != 0) {
            ret = EPERM;
            goto err;
        }
        my_crypto->rot_N = 13;
    } else if (ITEM_MATCHES(keyid, USER1_KEYID))
        my_crypto->rot_N = 4;
    else if (ITEM_MATCHES(keyid, USER2_KEYID))
        my_crypto->rot_N = 19;
    else {
        ret = EINVAL;
        goto err;
    }

    ++my_crypto->num_calls; /* Call count */

    *customp = (WT_ENCRYPTOR *)my_crypto;
    return (0);

err:
    free(my_crypto->keyid);
    free(my_crypto->password);
    free(my_crypto);
    return (ret);
}

/*
 * rotate_terminate --
 *     WiredTiger rotate encryption termination.
 */
static int
rotate_terminate(WT_ENCRYPTOR *encryptor, WT_SESSION *session)
{
    MY_CRYPTO *my_crypto = (MY_CRYPTO *)encryptor;

    (void)session; /* Unused parameters */

    ++my_crypto->num_calls; /* Call count */

    /* Free the allocated memory. */
    free(my_crypto->password);
    my_crypto->password = NULL;

    free(my_crypto->keyid);
    my_crypto->keyid = NULL;

    free(encryptor);

    return (0);
}

/*
 * add_my_encryptors --
 *     A simple example of adding encryption callbacks.
 */
int
add_my_encryptors(WT_CONNECTION *connection)
{
    MY_CRYPTO *m;
    WT_ENCRYPTOR *wt;

    /*
     * Initialize our top level encryptor.
     */
    if ((m = calloc(1, sizeof(MY_CRYPTO))) == NULL)
        return (errno);
    wt = (WT_ENCRYPTOR *)&m->encryptor;
    wt->encrypt = rotate_encrypt;
    wt->decrypt = rotate_decrypt;
    wt->sizing = rotate_sizing;
    wt->customize = rotate_customize;
    wt->terminate = rotate_terminate;
    m->num_calls = 0;
    error_check(connection->add_encryptor(connection, "rotn", (WT_ENCRYPTOR *)m, NULL));

    return (0);
}

/*
 * simple_walk_log --
 *     A simple walk of the write-ahead log. We wrote text messages into the log. Print them. This
 *     verifies we're decrypting properly.
 */
static void
simple_walk_log(WT_SESSION *session)
{
    WT_CURSOR *cursor;
    WT_ITEM logrec_key, logrec_value;
    uint64_t txnid;
    uint32_t fileid, log_file, log_offset, opcount, optype, rectype;
    int found, ret;

    error_check(session->open_cursor(session, "log:", NULL, NULL, &cursor));

    found = 0;
    while ((ret = cursor->next(cursor)) == 0) {
        error_check(cursor->get_key(cursor, &log_file, &log_offset, &opcount));
        error_check(cursor->get_value(
          cursor, &txnid, &rectype, &optype, &fileid, &logrec_key, &logrec_value));

        if (rectype == WT_LOGREC_MESSAGE) {
            found = 1;
            printf("Application Log Record: %s\n", (char *)logrec_value.data);
        }
    }
    scan_end_check(ret == WT_NOTFOUND);

    error_check(cursor->close(cursor));
    if (found == 0) {
        fprintf(stderr, "Did not find log messages.\n");
        exit(EXIT_FAILURE);
    }
}

#define MAX_KEYS 20

#define EXTENSION_NAME "local=(entry=add_my_encryptors)"

#define WT_OPEN_CONFIG_COMMON                             \
    "create,cache_size=100MB,extensions=[" EXTENSION_NAME \
    "],"                                                  \
    "log=(enabled=true,remove=false),"

#define WT_OPEN_CONFIG_GOOD \
    WT_OPEN_CONFIG_COMMON   \
    "encryption=(name=rotn,keyid=" SYS_KEYID ",secretkey=" SYS_PW ")"

#define COMP_A "AAAAAAAAAAAAAAAAAA"
#define COMP_B "BBBBBBBBBBBBBBBBBB"
#define COMP_C "CCCCCCCCCCCCCCCCCC"

int
main(int argc, char *argv[])
{
    WT_CONNECTION *conn;
    WT_CURSOR *c1, *c2, *nc, *sysc;
    WT_SESSION *session;
    int i, ret;
    char keybuf[32], valbuf[32];
    char *key1, *key2, *key3, *key4, *val1, *val2, *val3, *val4;

    home = example_setup(argc, argv);

    error_check(wiredtiger_open(home, NULL, WT_OPEN_CONFIG_GOOD, &conn));
    error_check(conn->open_session(conn, NULL, NULL, &session));

    /*
     * Write a log record that is larger than the base 128 bytes and also should compress well.
     */
    error_check(session->log_printf(session,
      COMP_A COMP_B COMP_C COMP_A COMP_B COMP_C COMP_A COMP_B COMP_C COMP_A COMP_B COMP_C
      "The quick brown fox jumps over the lazy dog "));
    simple_walk_log(session);

    /*
     * Create and open some tables. Also use column store and compression for some tables. Create
     * some tables with their own keyid, a table using the default system encryptor and a table that
     * is not encrypted, using the reserved name of none.
     */
    error_check(session->create(session, "table:crypto1",
      "encryption=(name=rotn,keyid=" USER1_KEYID
      "),columns=(key0,value0),key_format=S,value_format=S"));
    error_check(session->create(session, "index:crypto1:byvalue",
      "encryption=(name=rotn,keyid=" USER1_KEYID "),columns=(value0,key0)"));
    error_check(session->create(session, "table:crypto2",
      "encryption=(name=rotn,keyid=" USER2_KEYID "),key_format=S,value_format=S"));
    error_check(session->create(
      session, "table:nocrypto", "encryption=(name=none),key_format=S,value_format=S"));
    error_check(session->create(session, "table:syscrypto", "key_format=S,value_format=S"));

    /*
     * Send in an unknown keyid. WiredTiger will try to add in the new keyid, but the customize
     * function above will return an error since it is unrecognized.
     */
    ret = session->create(session, "table:cryptobad",
      "encryption=(name=rotn,keyid=" USERBAD_KEYID "),key_format=S,value_format=S");
    if (ret == 0) {
        fprintf(stderr, "Did not detect bad/unknown keyid error\n");
        exit(EXIT_FAILURE);
    }

    error_check(session->open_cursor(session, "table:crypto1", NULL, NULL, &c1));
    error_check(session->open_cursor(session, "table:crypto2", NULL, NULL, &c2));
    error_check(session->open_cursor(session, "table:nocrypto", NULL, NULL, &nc));
    error_check(session->open_cursor(session, "table:syscrypto", NULL, NULL, &sysc));

    /*
     * Insert a set of keys and values. Insert the same data into all tables so that we can verify
     * they're all the same after we decrypt on read.
     */
    for (i = 0; i < MAX_KEYS; i++) {
        (void)snprintf(keybuf, sizeof(keybuf), "key%d", i);
        c1->set_key(c1, keybuf);
        c2->set_key(c2, keybuf);
        nc->set_key(nc, keybuf);
        sysc->set_key(sysc, keybuf);

        (void)snprintf(valbuf, sizeof(valbuf), "value%d", i);
        c1->set_value(c1, valbuf);
        c2->set_value(c2, valbuf);
        nc->set_value(nc, valbuf);
        sysc->set_value(sysc, valbuf);

        error_check(c1->insert(c1));
        error_check(c2->insert(c2));
        error_check(nc->insert(nc));
        error_check(sysc->insert(sysc));
        if (i % 5 == 0)
            error_check(session->log_printf(session, "Wrote %d records", i));
    }
    error_check(session->log_printf(session, "Done. Wrote %d total records", i));

    while (c1->next(c1) == 0) {
        error_check(c1->get_key(c1, &key1));
        error_check(c1->get_value(c1, &val1));

        printf("Read key %s; value %s\n", key1, val1);
    }
    simple_walk_log(session);
    printf("CLOSE\n");
    error_check(conn->close(conn, NULL));

    /*
     * We want to close and reopen so that we recreate the cache by reading the data from disk,
     * forcing decryption.
     */
    printf("REOPEN and VERIFY encrypted data\n");

    error_check(wiredtiger_open(home, NULL, WT_OPEN_CONFIG_GOOD, &conn));

    error_check(conn->open_session(conn, NULL, NULL, &session));
    /*
     * Verify we can read the encrypted log after restart.
     */
    simple_walk_log(session);
    error_check(session->open_cursor(session, "table:crypto1", NULL, NULL, &c1));
    error_check(session->open_cursor(session, "table:crypto2", NULL, NULL, &c2));
    error_check(session->open_cursor(session, "table:nocrypto", NULL, NULL, &nc));
    error_check(session->open_cursor(session, "table:syscrypto", NULL, NULL, &sysc));

    /*
     * Read the same data from each cursor. All should be identical.
     */
    while (c1->next(c1) == 0) {
        error_check(c2->next(c2));
        error_check(nc->next(nc));
        error_check(sysc->next(sysc));
        error_check(c1->get_key(c1, &key1));
        error_check(c1->get_value(c1, &val1));
        error_check(c2->get_key(c2, &key2));
        error_check(c2->get_value(c2, &val2));
        error_check(nc->get_key(nc, &key3));
        error_check(nc->get_value(nc, &val3));
        error_check(sysc->get_key(sysc, &key4));
        error_check(sysc->get_value(sysc, &val4));

        /* If key 1 matches all the other keys then all the other keys match each other. */
        if (strcmp(key1, key2) != 0)
            fprintf(stderr, "Key1 %s and Key2 %s do not match\n", key1, key2);
        if (strcmp(key1, key3) != 0)
            fprintf(stderr, "Key1 %s and Key3 %s do not match\n", key1, key3);
        if (strcmp(key1, key4) != 0)
            fprintf(stderr, "Key1 %s and Key4 %s do not match\n", key1, key4);
        if (strcmp(val1, val2) != 0)
            fprintf(stderr, "Val1 %s and Val2 %s do not match\n", val1, val2);
        if (strcmp(val1, val3) != 0)
            fprintf(stderr, "Val1 %s and Val3 %s do not match\n", val1, val3);
        if (strcmp(val1, val4) != 0)
            fprintf(stderr, "Val1 %s and Val4 %s do not match\n", val1, val4);

        printf("Verified key %s; value %s\n", key1, val1);
    }

    error_check(conn->close(conn, NULL));

    return (EXIT_SUCCESS);
}
