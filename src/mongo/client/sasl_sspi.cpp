/*
 * Copyright (C) 2013 10gen, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#ifdef _WIN32

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#define SECURITY_WIN32 1  // Required for SSPI support.

#include "mongo/platform/basic.h"

#include <sasl/sasl.h>
#include <sasl/saslplug.h>
#include <sspi.h>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/client/sasl_sspi_options.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/text.h"

extern "C" int plain_client_plug_init(const sasl_utils_t* utils,
                                      int maxversion,
                                      int* out_version,
                                      sasl_client_plug_t** pluglist,
                                      int* plugcount);

extern "C" int crammd5_client_plug_init(const sasl_utils_t* utils,
                                        int maxversion,
                                        int* out_version,
                                        sasl_client_plug_t** pluglist,
                                        int* plugcount);

namespace mongo {
namespace {
/*
 * SSPI client plugin impl
 */

// The SSPI plugin implements the GSSAPI interface.
char sspiPluginName[] = "GSSAPI";

// This structure is passed through each callback to us by the sasl glue code.
struct SspiConnContext {
    CredHandle cred;
    bool haveCred;
    CtxtHandle ctx;
    bool haveCtxt;
    bool authComplete;
    std::wstring nameToken;
    std::string userPlusRealm;

    SspiConnContext() : haveCred(false), haveCtxt(false), authComplete(false) {}
    ~SspiConnContext() {
        if (haveCtxt) {
            DeleteSecurityContext(&ctx);
        }
        if (haveCred) {
            FreeCredentialsHandle(&cred);
        }
    }
};

// Utility function for fetching error text from Windows API function calls.
void HandleLastError(const sasl_utils_t* utils, DWORD errCode, const char* msg) {
    char* err;
    if (!FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                            FORMAT_MESSAGE_IGNORE_INSERTS,
                        NULL,
                        errCode,
                        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                        (LPSTR)&err,
                        0,
                        NULL)) {
        return;
    }

    std::string buffer(mongoutils::str::stream() << "SSPI: " << msg << ": " << err);
    utils->seterror(utils->conn, 0, "%s", buffer.c_str());
    LocalFree(err);
}

int sspiClientMechNew(void* glob_context,
                      sasl_client_params_t* cparams,
                      void** conn_context) throw() {
    // Prepare auth identity to pass to AcquireCredentialsHandle
    SEC_WINNT_AUTH_IDENTITY authIdentity;
    authIdentity.Flags = SEC_WINNT_AUTH_IDENTITY_UNICODE;

    // Fetch username@realm.
    sasl_getsimple_t* user_cb;
    void* user_context;
    int ret = cparams->utils->getcallback(
        cparams->utils->conn, SASL_CB_USER, (sasl_callback_ft*)&user_cb, &user_context);
    if (ret != SASL_OK) {
        cparams->utils->seterror(cparams->utils->conn, 0, "getcallback user failed");
        return ret;
    }
    const char* rawUserPlusRealm;
    unsigned rawUserPlusRealmLength = 0;

    ret = user_cb(user_context, SASL_CB_USER, &rawUserPlusRealm, &rawUserPlusRealmLength);
    if (ret != SASL_OK) {
        cparams->utils->seterror(cparams->utils->conn, 0, "user callback failed");
        return ret;
    }
    std::string userPlusRealm(rawUserPlusRealm, rawUserPlusRealmLength);

    // Parse out the username and realm.
    size_t atSign = userPlusRealm.find('@');
    if (atSign == std::string::npos) {
        cparams->utils->seterror(cparams->utils->conn, 0, "no @REALM found in username");
        return SASL_BADPARAM;
    }
    std::string utf8Username(userPlusRealm, 0, atSign);
    std::wstring utf16Username(toWideString(utf8Username.c_str()));

    authIdentity.UserLength = utf16Username.length();
    authIdentity.User =
        reinterpret_cast<unsigned short*>(const_cast<wchar_t*>(utf16Username.c_str()));

    std::string utf8Domain(userPlusRealm, atSign + 1);
    std::wstring utf16Domain(toWideString(utf8Domain.c_str()));
    authIdentity.DomainLength = utf16Domain.length();
    authIdentity.Domain =
        reinterpret_cast<unsigned short*>(const_cast<wchar_t*>(utf16Domain.c_str()));

    // Fetch password, if available.
    authIdentity.PasswordLength = 0;
    authIdentity.Password = NULL;
    std::wstring utf16Password;

    sasl_secret_t* password = NULL;
    sasl_getsecret_t* pass_cb;
    void* pass_context;
    ret = cparams->utils->getcallback(
        cparams->utils->conn, SASL_CB_PASS, (sasl_callback_ft*)&pass_cb, &pass_context);

    if ((ret == SASL_OK) && pass_cb) {
        ret = pass_cb(cparams->utils->conn, pass_context, SASL_CB_PASS, &password);
        if ((ret == SASL_OK) && password) {
            std::string utf8Password(reinterpret_cast<char*>(password->data), password->len);
            utf16Password = toWideString(utf8Password.c_str());
            authIdentity.PasswordLength = utf16Password.length();
            authIdentity.Password =
                reinterpret_cast<unsigned short*>(const_cast<wchar_t*>(utf16Password.c_str()));
        }
    }

    // Actually acquire the handle to the client credentials.
    std::unique_ptr<SspiConnContext> pcctx(new SspiConnContext());
    pcctx->userPlusRealm = userPlusRealm;
    TimeStamp ignored;
    SECURITY_STATUS status = AcquireCredentialsHandleW(NULL,  // principal
                                                       L"kerberos",
                                                       SECPKG_CRED_OUTBOUND,
                                                       NULL,           // LOGON id
                                                       &authIdentity,  // auth data
                                                       NULL,           // get key fn
                                                       NULL,           // get key arg
                                                       &pcctx->cred,
                                                       &ignored);
    if (status != SEC_E_OK) {
        HandleLastError(cparams->utils, status, "AcquireCredentialsHandle");
        return SASL_FAIL;
    }

    pcctx->haveCred = true;

    // Compose target name token. First, verify that a hostname has been provided.
    if (cparams->serverFQDN == NULL || strlen(cparams->serverFQDN) == 0) {
        cparams->utils->seterror(cparams->utils->conn, 0, "SSPI: no serverFQDN");
        return SASL_FAIL;
    }

    // Then obtain all potential FQDNs for the hostname.
    std::string canonName = cparams->serverFQDN;
    auto fqdns = getHostFQDNs(cparams->serverFQDN, saslSSPIGlobalParams.canonicalization);
    if (!fqdns.empty()) {
        // PTR records should point to the canonical name. If there's more than one, warn and
        // arbitrarily use the last entry.
        if (fqdns.size() > 1) {
            std::stringstream ss;
            ss << "Found multiple PTR records while performing reverse DNS: [ ";
            for (const std::string& fqdn : fqdns) {
                ss << fqdn << " ";
            }
            ss << "]";
            warning() << ss.str();
        }
        canonName = std::move(fqdns.back());
        fqdns.pop_back();
    } else if (saslSSPIGlobalParams.canonicalization != HostnameCanonicalizationMode::kNone) {
        warning() << "Was unable to acquire an FQDN";
    }

    pcctx->nameToken = toWideString(cparams->service) + L'/' + toWideString(canonName.c_str());
    if (!saslSSPIGlobalParams.realmOverride.empty()) {
        pcctx->nameToken += L'@' + toWideString(saslSSPIGlobalParams.realmOverride.c_str());
    }

    *conn_context = pcctx.release();

    return SASL_OK;
}

int sspiValidateServerSecurityLayerOffering(SspiConnContext* pcctx,
                                            sasl_client_params_t* cparams,
                                            const char* serverin,
                                            unsigned serverinlen) {
    std::unique_ptr<char[]> message(new char[serverinlen]);
    memcpy(message.get(), serverin, serverinlen);

    SecBuffer wrapBufs[2];
    SecBufferDesc wrapBufDesc;
    wrapBufDesc.cBuffers = 2;
    wrapBufDesc.pBuffers = wrapBufs;
    wrapBufDesc.ulVersion = SECBUFFER_VERSION;

    wrapBufs[0].cbBuffer = serverinlen;
    wrapBufs[0].BufferType = SECBUFFER_STREAM;
    wrapBufs[0].pvBuffer = message.get();

    wrapBufs[1].cbBuffer = 0;
    wrapBufs[1].BufferType = SECBUFFER_DATA;
    wrapBufs[1].pvBuffer = NULL;

    SECURITY_STATUS status = DecryptMessage(&pcctx->ctx, &wrapBufDesc, 0, NULL);
    if (status != SEC_E_OK) {
        HandleLastError(cparams->utils, status, "DecryptMessage");
        return SASL_FAIL;
    }

    // Validate the server's plaintext message.
    // Length (as per RFC 4752)
    if (wrapBufs[1].cbBuffer < 4) {
        cparams->utils->seterror(cparams->utils->conn, 0, "SSPI: server message is too short");
        return SASL_FAIL;
    }
    // First bit of first byte set, indicating that the client may elect to use no
    // security layer. As a client we are uninterested in any of the other features the
    // server offers and thus we ignore the other bits.
    if (!(static_cast<char*>(wrapBufs[1].pvBuffer)[0] & 1)) {
        cparams->utils->seterror(
            cparams->utils->conn, 0, "SSPI: server does not support the required security layer");
        return SASL_BADAUTH;
    }
    return SASL_OK;
}


int sspiSendClientAuthzId(SspiConnContext* pcctx,
                          sasl_client_params_t* cparams,
                          const char* serverin,
                          unsigned serverinlen,
                          const char** clientout,
                          unsigned* clientoutlen,
                          sasl_out_params_t* oparams) {
    // Ensure server response is decryptable.
    int decryptStatus =
        sspiValidateServerSecurityLayerOffering(pcctx, cparams, serverin, serverinlen);
    if (decryptStatus != SASL_OK) {
        return decryptStatus;
    }

    // Fill in AUTHID and AUTHZID fields in oparams.
    int ret = cparams->canon_user(cparams->utils->conn,
                                  pcctx->userPlusRealm.c_str(),
                                  0,
                                  SASL_CU_AUTHID | SASL_CU_AUTHZID,
                                  oparams);

    // Reply to server with security capability and authz name.
    SecPkgContext_Sizes sizes;
    SECURITY_STATUS status = QueryContextAttributes(&pcctx->ctx, SECPKG_ATTR_SIZES, &sizes);
    if (status != SEC_E_OK) {
        HandleLastError(cparams->utils, status, "QueryContextAttributes(sizes)");
        return SASL_FAIL;
    }

    // See RFC4752.
    int plaintextMessageSize = 4 + pcctx->userPlusRealm.size();
    std::unique_ptr<char[]> message(
        new char[sizes.cbSecurityTrailer + plaintextMessageSize + sizes.cbBlockSize]);
    char* plaintextMessage = message.get() + sizes.cbSecurityTrailer;
    plaintextMessage[0] = 1;  // LAYER_NONE
    plaintextMessage[1] = 0;
    plaintextMessage[2] = 0;
    plaintextMessage[3] = 0;
    memcpy(&plaintextMessage[4], pcctx->userPlusRealm.c_str(), pcctx->userPlusRealm.size());

    SecBuffer wrapBufs[3];
    SecBufferDesc wrapBufDesc;
    wrapBufDesc.cBuffers = 3;
    wrapBufDesc.pBuffers = wrapBufs;
    wrapBufDesc.ulVersion = SECBUFFER_VERSION;

    wrapBufs[0].cbBuffer = sizes.cbSecurityTrailer;
    wrapBufs[0].BufferType = SECBUFFER_TOKEN;
    wrapBufs[0].pvBuffer = message.get();

    wrapBufs[1].cbBuffer = plaintextMessageSize;
    wrapBufs[1].BufferType = SECBUFFER_DATA;
    wrapBufs[1].pvBuffer = message.get() + sizes.cbSecurityTrailer;

    wrapBufs[2].cbBuffer = sizes.cbBlockSize;
    wrapBufs[2].BufferType = SECBUFFER_PADDING;
    wrapBufs[2].pvBuffer = message.get() + sizes.cbSecurityTrailer + plaintextMessageSize;

    status = EncryptMessage(&pcctx->ctx, SECQOP_WRAP_NO_ENCRYPT, &wrapBufDesc, 0);

    if (status != SEC_E_OK) {
        HandleLastError(cparams->utils, status, "EncryptMessage");
        return SASL_FAIL;
    }

    // Create the message to send to server.
    *clientoutlen = wrapBufs[0].cbBuffer + wrapBufs[1].cbBuffer + wrapBufs[2].cbBuffer;
    char* newoutbuf = static_cast<char*>(cparams->utils->malloc(*clientoutlen));
    memcpy(newoutbuf, wrapBufs[0].pvBuffer, wrapBufs[0].cbBuffer);
    memcpy(newoutbuf + wrapBufs[0].cbBuffer, wrapBufs[1].pvBuffer, wrapBufs[1].cbBuffer);
    memcpy(newoutbuf + wrapBufs[0].cbBuffer + wrapBufs[1].cbBuffer,
           wrapBufs[2].pvBuffer,
           wrapBufs[2].cbBuffer);
    *clientout = newoutbuf;

    return SASL_OK;
}


int sspiClientMechStep(void* conn_context,
                       sasl_client_params_t* cparams,
                       const char* serverin,
                       unsigned serverinlen,
                       sasl_interact_t** prompt_need,
                       const char** clientout,
                       unsigned* clientoutlen,
                       sasl_out_params_t* oparams) throw() {
    SspiConnContext* pcctx = static_cast<SspiConnContext*>(conn_context);
    *clientout = NULL;
    *clientoutlen = 0;

    if (pcctx->authComplete) {
        return sspiSendClientAuthzId(
            pcctx, cparams, serverin, serverinlen, clientout, clientoutlen, oparams);
    }

    SecBufferDesc inbuf;
    SecBuffer inBufs[1];
    SecBufferDesc outbuf;
    SecBuffer outBufs[1];

    if (pcctx->haveCtxt) {
        // If we already have a context, we now have data to send.
        // Put this data in an inbuf.
        inbuf.ulVersion = SECBUFFER_VERSION;
        inbuf.cBuffers = 1;
        inbuf.pBuffers = inBufs;
        inBufs[0].pvBuffer = const_cast<char*>(serverin);
        inBufs[0].cbBuffer = serverinlen;
        inBufs[0].BufferType = SECBUFFER_TOKEN;
    }

    outbuf.ulVersion = SECBUFFER_VERSION;
    outbuf.cBuffers = 1;
    outbuf.pBuffers = outBufs;
    outBufs[0].pvBuffer = NULL;
    outBufs[0].cbBuffer = 0;
    outBufs[0].BufferType = SECBUFFER_TOKEN;

    ULONG contextAttr = 0;
    SECURITY_STATUS status =
        InitializeSecurityContextW(&pcctx->cred,
                                   pcctx->haveCtxt ? &pcctx->ctx : NULL,
                                   const_cast<wchar_t*>(pcctx->nameToken.c_str()),
                                   ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_MUTUAL_AUTH,
                                   0,
                                   SECURITY_NETWORK_DREP,
                                   (pcctx->haveCtxt ? &inbuf : NULL),
                                   0,
                                   &pcctx->ctx,
                                   &outbuf,
                                   &contextAttr,
                                   NULL);

    if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED) {
        HandleLastError(cparams->utils, status, "InitializeSecurityContext");
        return SASL_FAIL;
    }

    ON_BLOCK_EXIT(FreeContextBuffer, outbuf.pBuffers[0].pvBuffer);
    pcctx->haveCtxt = true;

    if (status == SEC_E_OK) {
        // Send back nothing and wait for the server to reply with the security capabilities
        *clientout = NULL;
        *clientoutlen = 0;
        pcctx->authComplete = true;
        return SASL_CONTINUE;
    }

    char* newoutbuf = static_cast<char*>(cparams->utils->malloc(outBufs[0].cbBuffer));
    *clientoutlen = outBufs[0].cbBuffer;
    memcpy(newoutbuf, outBufs[0].pvBuffer, *clientoutlen);
    *clientout = newoutbuf;
    return SASL_CONTINUE;
}

void sspiClientMechDispose(void* conn_context, const sasl_utils_t* utils) {
    SspiConnContext* pcctx = static_cast<SspiConnContext*>(conn_context);
    delete pcctx;
}

void sspiClientMechFree(void* glob_context, const sasl_utils_t* utils) {}

sasl_client_plug_t sspiClientPlugin[] = {
    {sspiPluginName, /* mechanism name */
     112, /* TODO: (taken from gssapi) best mech additional security layer strength factor */
     SASL_SEC_NOPLAINTEXT /* eam: copied from gssapi */
         |
         SASL_SEC_NOACTIVE | SASL_SEC_NOANONYMOUS | SASL_SEC_MUTUAL_AUTH |
         SASL_SEC_PASS_CREDENTIALS, /* security_flags */
     SASL_FEAT_NEEDSERVERFQDN | SASL_FEAT_WANT_CLIENT_FIRST | SASL_FEAT_ALLOWS_PROXY,
     NULL, /* required prompt ids, NULL = user/pass only */
     NULL, /* global state for mechanism */
     sspiClientMechNew,
     sspiClientMechStep,
     sspiClientMechDispose,
     sspiClientMechFree,
     NULL,
     NULL,
     NULL}};

int sspiClientPluginInit(const sasl_utils_t* utils,
                         int max_version,
                         int* out_version,
                         sasl_client_plug_t** pluglist,
                         int* plugcount) {
    if (max_version < SASL_CLIENT_PLUG_VERSION) {
        utils->seterror(utils->conn, 0, "Wrong SSPI version");
        return SASL_BADVERS;
    }

    *out_version = SASL_CLIENT_PLUG_VERSION;
    *pluglist = sspiClientPlugin;
    *plugcount = 1;

    return SASL_OK;
}

/**
 * Registers the plugin at process initialization time.
 * Must be run after the AllocatorsAndMutexes are registered, but before the ClientContext is
 * created.
 */
MONGO_INITIALIZER_WITH_PREREQUISITES(SaslSspiClientPlugin,
                                     ("CyrusSaslAllocatorsAndMutexes", "CyrusSaslClientContext"))
(InitializerContext*) {
    int ret = sasl_client_add_plugin(sspiPluginName, sspiClientPluginInit);
    if (SASL_OK != ret) {
        return Status(ErrorCodes::UnknownError,
                      mongoutils::str::stream() << "could not add SASL Client SSPI plugin "
                                                << sspiPluginName
                                                << ": "
                                                << sasl_errstring(ret, NULL, NULL));
    }

    return Status::OK();
}
MONGO_INITIALIZER_WITH_PREREQUISITES(SaslCramClientPlugin,
                                     ("CyrusSaslAllocatorsAndMutexes", "CyrusSaslClientContext"))
(InitializerContext*) {
    int ret = sasl_client_add_plugin("CRAMMD5", crammd5_client_plug_init);
    if (SASL_OK != ret) {
        return Status(ErrorCodes::UnknownError,
                      mongoutils::str::stream() << "Could not add SASL Client CRAM-MD5 plugin "
                                                << sspiPluginName
                                                << ": "
                                                << sasl_errstring(ret, NULL, NULL));
    }

    return Status::OK();
}

MONGO_INITIALIZER_WITH_PREREQUISITES(SaslPlainClientPlugin,
                                     ("CyrusSaslAllocatorsAndMutexes", "CyrusSaslClientContext"))
(InitializerContext*) {
    int ret = sasl_client_add_plugin("PLAIN", plain_client_plug_init);
    if (SASL_OK != ret) {
        return Status(ErrorCodes::UnknownError,
                      mongoutils::str::stream() << "Could not add SASL Client PLAIN plugin "
                                                << sspiPluginName
                                                << ": "
                                                << sasl_errstring(ret, NULL, NULL));
    }

    return Status::OK();
}

}  // namespace
}  // namespace mongo

#endif  // ifdef _WIN32
