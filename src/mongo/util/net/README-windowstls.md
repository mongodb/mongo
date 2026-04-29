# Windows TLS 1.3 (SChannel) Support

This document covers MongoDB's TLS 1.3 support on Windows, which uses the
[SChannel SSP](https://docs.microsoft.com/en-us/windows-server/security/tls/tls-ssl-schannel-ssp-overview)
provided by the OS. It describes the features, design choices, and known limitations introduced by
SERVER-79980.

## Table of Contents

- [Platform Requirements](#platform-requirements)
- [What Changed](#what-changed)
  - [SCH_CREDENTIALS replaces SCHANNEL_CRED](#sch_credentials-replaces-schannel_cred)
  - [Protocol selection: blocklist instead of allowlist](#protocol-selection-blocklist-instead-of-allowlist)
  - [CNG key storage replaces CAPI](#cng-key-storage-replaces-capi)
  - [Client certificate handling](#client-certificate-handling)
- [Post-Handshake Message Handling](#post-handshake-message-handling)
  - [SEC_I_RENEGOTIATE on TLS 1.3 (Schannel-to-Schannel)](#sec_i_renegotiate-on-tls-13-schannel-to-schannel)
  - [0x80090317 from OpenSSL peers (Schannel-as-client)](#0x80090317-from-openssl-peers-schannel-as-client)
  - [Why ISC/ASC must not be called after a post-handshake message](#why-iscasc-must-not-be-called-after-a-post-handshake-message)
  - [TLS record header fallback](#tls-record-header-fallback)
- [Shutdown Handling](#shutdown-handling)
- [Known Limitations](#known-limitations)
  - [NewSessionTickets from OpenSSL peers](#newsessiontickets-from-openssl-peers)

---

## Platform Requirements

TLS 1.3 in SChannel requires **Windows Server 2022 / Windows 11 (build 22000) or later**. Older
versions of Windows (including Windows Server 2019) do not expose TLS 1.3 through the SChannel API,
even if an OS update has added partial support. The helper function `windowsSupportsTLS13()` in
[jstests/libs/os_helpers.js](../../../../jstests/libs/os_helpers.js) detects TLS 1.3 availability by
querying the registry key `HKLM\SYSTEM\...\SCHANNEL\Protocols\TLS 1.3\Client` and falls back to
parsing `systeminfo` output for Windows 11 / Server 2022 host name strings.

When TLS 1.3 is not available the server falls back to TLS 1.2, which remains fully supported.

---

## What Changed

### SCH_CREDENTIALS replaces SCHANNEL_CRED

The legacy `SCHANNEL_CRED` credential structure (version 4) predates TLS 1.3 and does not support
it. TLS 1.3 requires the newer `SCH_CREDENTIALS` structure (version 5, introduced in Windows 10 1903
/ Server 2022), which accepts a `TLS_PARAMETERS` array describing per-protocol constraints.

**Before:**

```cpp
SCHANNEL_CRED cred;
cred.dwVersion = SCHANNEL_CRED_VERSION;   // 4
cred.grbitEnabledProtocols = SP_PROT_TLS1_SERVER | SP_PROT_TLS1_2_SERVER; // allowlist
```

**After:**

```cpp
TLS_PARAMETERS tlsParams = {};
SCH_CREDENTIALS cred;
cred.dwVersion = SCH_CREDENTIALS_VERSION;  // 5
cred.cTlsParameters = 1;
cred.pTlsParameters = &tlsParams;
tlsParams.grbitDisabledProtocols = SP_PROT_TLS1_0 | SP_PROT_TLS1_1;  // blocklist
```

Crucially, `SCH_CREDENTIALS` with an empty `grbitDisabledProtocols` (zero) means _all_ protocols
supported by the OS are enabled, including TLS 1.3. The old allowlist approach would never enable
TLS 1.3 because the old constants did not include a TLS 1.3 bit.

The `--tlsDisabledProtocols` option now supports `TLS1_3` as a value, which sets `SP_PROT_TLS1_3` in
`grbitDisabledProtocols`.

See [ssl_manager_windows.cpp](ssl_manager_windows.cpp) — `initSSLContext()`.

### Protocol selection: blocklist instead of allowlist

The old code built a protocol allowlist via bitwise OR of per-direction constants
(`SP_PROT_TLS1_2_SERVER`, `SP_PROT_TLS1_2_CLIENT`, etc.) and assigned it to `grbitEnabledProtocols`.
The new code uses a single direction-agnostic `grbitDisabledProtocols` bitmask in `TLS_PARAMETERS` —
new protocols added to Windows in future releases are automatically available without code changes,
and the bitmask is the same for both server and client directions.

### CNG key storage replaces CAPI

`SCH_CREDENTIALS` requires private keys to be stored in the **Cryptography API: Next Generation
(CNG)** key store (`NCrypt`). The legacy CAPI (`CRYPT_ACQUIRE_CONTEXT` / `CryptImportKey`) keys used
with `SCHANNEL_CRED` are not accepted by `SCH_CREDENTIALS`.

`readCertPEMFile()` in [ssl_manager_windows.cpp](ssl_manager_windows.cpp) was rewritten to:

1. Parse the PEM file and extract the raw RSA private key bytes.
2. Call `NCryptOpenStorageProvider(MS_KEY_STORAGE_PROVIDER)` to open the CNG software KSP.
3. Call `NCryptImportKey` with a PID-scoped named container (`LEGACY_RSAPRIVATE_BLOB`,
   `NCRYPT_OVERWRITE_KEY_FLAG`).
4. Associate the key with the certificate context via `CERT_KEY_PROV_INFO_PROP_ID`, storing only the
   container name and provider name — **not** a live key handle.

Storing only the name blob (prop ID 2) rather than a live handle (prop IDs 78/99) avoids a slow
KSP-wide key enumeration that CertGetCertificateContextProperty would trigger when a handle is not
yet cached. Schannel opens the named container directly via `NCryptOpenKey` at handshake time.

Container names are scoped to the process ID and a monotonically increasing counter, so multiple TLS
contexts within the same process do not collide. The `UniqueNcryptKeyWithDeletion` RAII wrapper
calls `NCryptDeleteKey` on destruction, cleaning up ephemeral key material when the
`SSLManagerWindows` is torn down.

### Client certificate handling

`InitializeSecurityContextW` accepts an `ISC_REQ_USE_SUPPLIED_CREDS` flag that restricts SChannel to
the credentials in `_cred->paCred`. When no client certificate is configured (`cCreds == 0`),
setting this flag causes a deadlock: SChannel sends an empty `CertificateRequest` response for TLS
1.3 mutual-auth servers, then hangs waiting for a reply that never comes.

The fix ([ssl/detail/schannel.hpp](ssl/detail/schannel.hpp), `getClientFlags()`):

```cpp
DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
    ISC_REQ_EXTENDED_ERROR | ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION;
if (_cred->cCreds > 0) {
    flags |= ISC_REQ_USE_SUPPLIED_CREDS;
}
```

When no certificate is configured, SChannel falls back to its normal store-search behaviour and
sends an empty `Certificate` message, allowing the TLS 1.3 handshake to complete.

---

## Post-Handshake Message Handling

TLS 1.3 introduces _post-handshake messages_ — records exchanged over the established connection
after the handshake completes. The two cases MongoDB encounters are:

- **NewSessionTicket (NST)**: the server distributes session-resumption material to the client.
  OpenSSL sends 2 NSTs by default immediately after the handshake.
- **KeyUpdate**: a peer requests traffic-key rotation.

SChannel handles these internally but surfaces them to the application through `DecryptMessage`
return values. Two different status codes are used depending on which side of the connection
SChannel is on.

### SEC_I_RENEGOTIATE on TLS 1.3 (Schannel-to-Schannel)

When SChannel is the **server** (or when both peers are SChannel), `DecryptMessage` returns
`SEC_I_RENEGOTIATE` (0x00090316) after consuming a post-handshake record.

Pre-TLS 1.3, this code meant the peer requested _renegotiation_ (via `HelloRequest`), which MongoDB
blocks. On TLS 1.3, renegotiation was removed from the specification entirely; `SEC_I_RENEGOTIATE`
is reused to signal post-handshake messages instead.

The code in [ssl/detail/impl/schannel.ipp](ssl/detail/impl/schannel.ipp) (`decryptBuffer`)
distinguishes the two cases by querying `SECPKG_ATTR_CONNECTION_INFO`:

```cpp
const bool isTLS13 = (qi == SEC_E_OK) && ((connInfo.dwProtocol & SP_PROT_TLS1_3) != 0);
if (!isTLS13) {
    *pDecryptState = DecryptState::Renegotiate;
    ec = asio::ssl::error::no_renegotiation;
    return ssl_want::want_nothing;
}
// TLS 1.3: post-handshake message consumed internally — no ISC/ASC call needed.
```

Any trailing bytes indicated by `SECBUFFER_EXTRA` are preserved in `_pExtraEncryptedBuffer` and
injected at the head of the next decryption loop iteration.

### 0x80090317 from OpenSSL peers (Schannel-as-client)

When SChannel acts as a **TLS client** connecting to an **OpenSSL server** (e.g. the PyKMIP server
used for Encrypted Storage Engine tests), `DecryptMessage` returns `0x80090317` instead of
`SEC_I_RENEGOTIATE` when it consumes a TLS 1.3 NewSessionTicket.

`0x80090317` is the _error-severity_ form of `SEC_I_CONTEXT_EXPIRED` (defined in `winerror.h`).
Microsoft does not appear to have published documentation that explicitly describes this behaviour;
it is an observed quirk of SChannel on Windows Server 2022 when the client processes TLS 1.3
post-handshake messages from an OpenSSL peer. It does not indicate a real error; the NST record has
been consumed internally, exactly as with `SEC_I_RENEGOTIATE`.

The code checks the negative-status range to distinguish real failures from informational codes:

```cpp
} else if (ss == static_cast<SECURITY_STATUS>(0x80090317L)) {
    // NST consumed internally — preserve trailing bytes and retry.
    ...
    return ssl_want::want_input_and_retry;
}
```

### Why ISC/ASC must not be called after a post-handshake message

It may seem natural to call `InitializeSecurityContextW` (ISC) or `AcceptSecurityContext` (ASC)
after `DecryptMessage` returns `SEC_I_RENEGOTIATE` or `0x80090317`, on the assumption that a
post-handshake message might require a response (e.g. a `KeyUpdate` acknowledgement). Doing so is
incorrect and must be avoided.

Calling ISC or ASC at this point **silently rotates SChannel's application-layer traffic keys from K
to K+1** without any corresponding wire-level `KeyUpdate` message being sent to the peer. The peer
still encrypts with K, so the next `DecryptMessage` call fails with `SEC_E_DECRYPT_FAILURE`
(0x80090330, "The specified data could not be decrypted").

SChannel handles post-handshake messages entirely internally when `DecryptMessage` returns these
codes. The correct response is simply to retry the read. No ISC/ASC call is required or safe.

### TLS record header fallback

When `DecryptMessage` returns `0x80090317`, SChannel may **not** populate `SECBUFFER_EXTRA` with the
bytes that follow the consumed NST record — even if a subsequent TLS record (e.g. the KMIP
application-data response) arrived in the same `recv()` call.

If `SECBUFFER_EXTRA` is absent, the code falls back to parsing the 5-byte TLS record header manually
to locate the boundary:

```
ContentType[1]  LegacyVersion[2]  PayloadLength[2]
totalRecordBytes = 5 + big_endian(bytes[3..4])
```

The pre-call input pointer and size are used (not the post-call `securityBuffers[0]` values) because
`DecryptMessage` overwrites `securityBuffers[0].cbBuffer` with the consumed NST size, which would
make `recordSize == inputLen` and lose the trailing bytes.

This is the root cause fixed in SERVER-79980: a KMIP response and an NST arriving in the same
`recv()` buffer, with SChannel returning `0x80090317` for the NST and providing no `SECBUFFER_EXTRA`
pointer to the response.

---

## Shutdown Handling

`ApplyControlToken`, `AcceptSecurityContext`, and `InitializeSecurityContextW` may return
`SEC_I_CONTEXT_EXPIRED` (0x00090317, the _success-severity_ form) during TLS shutdown if the context
is already in the expired state (e.g. because a `close_notify` was already processed). This is not
an error.

The shutdown paths in `SSLHandshakeManager::startShutdown()` use the same convention as
`decryptBuffer`: only negative `SECURITY_STATUS` values (where the high bit is set) are treated as
failures. `SEC_I_CONTEXT_EXPIRED` is a non-negative informational code and is accepted as success.

---

## Known Limitations

### NewSessionTickets from OpenSSL peers

When MongoDB acts as a TLS 1.3 client connecting to an OpenSSL-backed server (e.g. the PyKMIP KMIP
server used in Encrypted Storage Engine tests), OpenSSL sends 2 NewSessionTickets by default
immediately after the handshake. Each NST causes SChannel to return `0x80090317` from
`DecryptMessage`. The code handles this correctly, but it means every new connection to a PyKMIP
server incurs two extra read/retry cycles before the first application-data response is surfaced.

**Workaround for test environments**: The `kmip_server.py` wrapper accepts a `--no-session-tickets`
flag that sets `context.num_tickets = 0` on the Python SSL context, suppressing all TLS 1.3 NSTs
from the PyKMIP server. On Windows, `helpers.js` passes this flag automatically when the test runner
detects SChannel is in use (`isWindowsSchannel` in
[helpers.js](../../../../src/mongo/db/modules/enterprise/jstests/encryptdb/libs/helpers.js)).
Non-Windows platforms still receive NSTs, preserving test coverage of the NST handling path.

> **Note**: `ssl.OP_NO_TICKET` in Python's ssl module suppresses TLS 1.2 _session ticket requests
> from the client_ and does **not** suppress TLS 1.3 NewSessionTicket messages sent by the server.
> Use `context.num_tickets = 0` (Python 3.8+) for the latter.
