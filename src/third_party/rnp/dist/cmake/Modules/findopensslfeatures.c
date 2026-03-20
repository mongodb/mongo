#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <openssl/ec.h>
#include <openssl/objects.h>
#include <openssl/evp.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/provider.h>
#endif

int
list_curves()
{
    size_t            len = EC_get_builtin_curves(NULL, 0);
    EC_builtin_curve *curves = OPENSSL_malloc(sizeof(EC_builtin_curve) * len);
    if (!curves) {
        fprintf(stderr, "Allocation failed.\n");
        return 1;
    }
    if (!EC_get_builtin_curves(curves, len)) {
        OPENSSL_free(curves);
        fprintf(stderr, "Failed to get curves.\n");
        return 1;
    }
    for (size_t i = 0; i < len; i++) {
        const char *sname = OBJ_nid2sn(curves[i].nid);
        if (!sname) {
            continue;
        }
        printf("%s\n", sname);
    }
    OPENSSL_free(curves);
    return 0;
}

static void
print_hash(const EVP_MD *md, const char *from, const char *to, void *arg)
{
    if (!md) {
        return;
    }
    if (strstr(from, "rsa") || strstr(from, "RSA")) {
        return;
    }
    printf("%s\n", from);
}

int
list_hashes()
{
    EVP_MD_do_all_sorted(print_hash, NULL);
    return 0;
}

static void
print_cipher(const EVP_CIPHER *cipher, const char *from, const char *to, void *x)
{
    if (!cipher) {
        return;
    }
    printf("%s\n", from);
}

int
list_ciphers()
{
    EVP_CIPHER_do_all_sorted(print_cipher, NULL);
    return 0;
}

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
static void
print_km_name(const char *name, void *param)
{
    /* Do not print OIDs for better clarity */
    if (!name || ((name[0] <= '9') && (name[0] >= '0'))) {
        return;
    }
    printf("%s\n", name);
}

static void
print_km(EVP_KEYMGMT *km, void *param)
{
    EVP_KEYMGMT_names_do_all(km, print_km_name, NULL);
}
#endif

int
list_publickey()
{
#if OPENSSL_VERSION_NUMBER < 0x30000000L
    for (size_t i = 0; i < EVP_PKEY_meth_get_count(); i++) {
        const EVP_PKEY_METHOD *pmeth = EVP_PKEY_meth_get0(i);
        int                    id = 0;
        EVP_PKEY_meth_get0_info(&id, NULL, pmeth);
        printf("%s\n", OBJ_nid2ln(id));
    }
#else
    EVP_KEYMGMT_do_all_provided(NULL, print_km, NULL);
#endif
    return 0;
}

int
list_providers()
{
    printf("default\n");
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    const char *known_names[] = {"legacy", "fips"};
    for (size_t i = 0; i < sizeof(known_names) / sizeof(known_names[0]); i++) {
        OSSL_PROVIDER *prov = OSSL_PROVIDER_load(NULL, known_names[i]);
        if (prov) {
            printf("%s\n", known_names[i]);
            OSSL_PROVIDER_unload(prov);
        }
    }
#else
    /* OpenSSL < 3.0 includes all legacy algorithms in the default provider */
    printf("legacy\n");
#endif
    return 0;
}

int
main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr,
                "Usage: opensslfeatures [curves|hashes|ciphers|publickey|providers]\n");
        return 1;
    }
    if (!strcmp(argv[1], "hashes")) {
        return list_hashes();
    }
    if (!strcmp(argv[1], "ciphers")) {
        return list_ciphers();
    }
    if (!strcmp(argv[1], "curves")) {
        return list_curves();
    }
    if (!strcmp(argv[1], "publickey")) {
        return list_publickey();
    }
    if (!strcmp(argv[1], "providers")) {
        return list_providers();
    }
    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    return 1;
}
