// Copyright 2016 MongoDB, Inc.

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/safestack.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
#include <stdio.h>
#include <stdint.h>

#if defined(_WIN32)
#include <wincrypt.h>
#elif defined(__APPLE__)
#include <Security/Security.h>
#endif

#include <fcntl.h>

static int checkX509_STORE_error(char* err, size_t err_len) {
    unsigned long errCode = ERR_peek_last_error();
    if (ERR_GET_LIB(errCode) != ERR_LIB_X509 ||
        ERR_GET_REASON(errCode) != X509_R_CERT_ALREADY_IN_HASH_TABLE) {
        snprintf(err,
                 err_len,
                 "Error adding certificate to X509 store: %s",
                 ERR_reason_error_string(errCode));
        return 0;
    }
    return 1;
}

#if defined(_WIN32)

void formatError(const DWORD error_code, const char * prefix, char * err, size_t err_len) {
        LPTSTR errorText = NULL;
        FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER |
                          FORMAT_MESSAGE_IGNORE_INSERTS,
                      NULL,
                      error_code,
                      0,
                      (LPTSTR)&errorText,  // output
                      0,                   // minimum size for output buffer
                      NULL);
        snprintf(err, err_len, "%s: %s",prefix, errorText);
        LocalFree(errorText);
}

// This imports the certificates in a given Windows certificate store into an
// X509_STORE for
// openssl to use during certificate validation.
static int importCertStoreToX509_STORE(
    LPWSTR storeName, DWORD storeLocation, X509_STORE* verifyStore, char* err, size_t err_len) {
    int status = 1;
    X509* x509Cert = NULL;
    HCERTSTORE systemStore =
        CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, (HCRYPTPROV)NULL, storeLocation, storeName);
    if (systemStore == NULL) {
	formatError(GetLastError(),"error opening system CA store",err,err_len);
        status = 0;
        goto CLEANUP;
    }

    PCCERT_CONTEXT certCtx = NULL;
    while ((certCtx = CertEnumCertificatesInStore(systemStore, certCtx)) != NULL) {
        const uint8_t * certBytes = (const uint8_t *)(certCtx->pbCertEncoded);
        x509Cert = d2i_X509(NULL, &certBytes, certCtx->cbCertEncoded);
        if (x509Cert == NULL) {
	    // 120 from the SSL documentation for ERR_error_string
            static const size_t msglen = 120;
            char msg[msglen];
            ERR_error_string_n(ERR_get_error(), msg, msglen);
            snprintf(
                err, err_len, "Error parsing X509 object from Windows certificate store %s", msg);
            status = 0;
            goto CLEANUP;
        }

        if (1 != X509_STORE_add_cert(verifyStore, x509Cert)) {
            int store_error_status = checkX509_STORE_error(err, err_len);
            if (!store_error_status) {
                status = 0;
                goto CLEANUP;
            }
        }
    }
    DWORD lastError = GetLastError();
    if (lastError != CRYPT_E_NOT_FOUND) {
	formatError(lastError,"Error enumerating certificates",err,err_len);
        status = 0;
        goto CLEANUP;
    }

CLEANUP:
    if (systemStore != NULL) {
        CertCloseStore(systemStore, 0);
    }
    if (x509Cert != NULL) {
        X509_free(x509Cert);
    }
    return status;
}
#elif defined(__APPLE__)

static int importKeychainToX509_STORE(X509_STORE* verifyStore, char* err, size_t err_len) {
    int status = 1;
    CFArrayRef result = NULL;
    OSStatus osStatus;

    // This copies all the certificates trusted by the system (regardless of what
    // keychain they're
    // attached to) into a CFArray.
    if ((osStatus = SecTrustCopyAnchorCertificates(&result)) != 0) {
        CFStringRef statusString = SecCopyErrorMessageString(osStatus, NULL);
        snprintf(err,
                 err_len,
                 "Error enumerating certificates: %s",
                 CFStringGetCStringPtr(statusString, kCFStringEncodingASCII));
        CFRelease(statusString);
        status = 0;
        goto CLEANUP;
    }

    CFDataRef rawData = NULL;
    X509* x509Cert = NULL;

    for (CFIndex i = 0; i < CFArrayGetCount(result); i++) {
        SecCertificateRef cert = (SecCertificateRef)CFArrayGetValueAtIndex(result, i);

        rawData = SecCertificateCopyData(cert);
        if (!rawData) {
            snprintf(err, err_len, "Error enumerating certificates");
            status = 0;
            goto CLEANUP;
        }
        const uint8_t* rawDataPtr = CFDataGetBytePtr(rawData);

        // Parse an openssl X509 object from each returned certificate
        x509Cert = d2i_X509(NULL, &rawDataPtr, CFDataGetLength(rawData));
        if (!x509Cert) {
            snprintf(err,
                     err_len,
                     "Error parsing X509 certificate from system keychain: %s",
                     ERR_reason_error_string(ERR_peek_last_error()));
            status = 0;
            goto CLEANUP;
        }

        // Add the parsed X509 object to the X509_STORE verification store
        if (X509_STORE_add_cert(verifyStore, x509Cert) != 1) {
            int check_error_status = checkX509_STORE_error(err, err_len);
            if (!check_error_status) {
                status = check_error_status;
                goto CLEANUP;
            }
        }
        CFRelease(rawData);
        rawData = NULL;
        X509_free(x509Cert);
        x509Cert = NULL;
    }

CLEANUP:
    if (result != NULL) {
        CFRelease(result);
    }
    if (rawData != NULL) {
        CFRelease(rawData);
    }
    if (x509Cert != NULL) {
        X509_free(x509Cert);
    }
    return status;
}
#endif

int _setupSystemCA(SSL_CTX* context, char* err, size_t err_len) {
#if !defined(_WIN32) && !defined(__APPLE__)
    // On non-Windows/non-Apple platforms, the OpenSSL libraries should have been
    // configured with default locations for CA certificates.
    if (SSL_CTX_set_default_verify_paths(context) != 1) {
        snprintf(err,
                 err_len,
                 "error loading system CA certificates "
                 "(default certificate file: %s default certificate path: %s )",
                 X509_get_default_cert_file(),
                 X509_get_default_cert_dir());
        return 0;
    }
    return 1;
#else

    X509_STORE* verifyStore = SSL_CTX_get_cert_store(context);
    if (!verifyStore) {
        snprintf(err,
                 err_len,
                 "no X509 store found for SSL context while loading "
                 "system certificates");
        return 0;
    }
#if defined(_WIN32)
    int status = importCertStoreToX509_STORE(
        L"root", CERT_SYSTEM_STORE_CURRENT_USER, verifyStore, err, err_len);
    if (!status)
        return status;
    return importCertStoreToX509_STORE(
        L"CA", CERT_SYSTEM_STORE_CURRENT_USER, verifyStore, err, err_len);
#elif defined(__APPLE__)
    return importKeychainToX509_STORE(verifyStore, err, err_len);
#endif
#endif
}
