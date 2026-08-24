# Windows TLS 1.3 (SChannel) Support

On Windows, MongoDB uses the operating system's
[SChannel SSP](https://docs.microsoft.com/en-us/windows-server/security/tls/tls-ssl-schannel-ssp-overview)
for TLS. This document gives a high-level overview of TLS 1.3 support on Windows and the
customer-visible differences from earlier releases.

## Table of Contents

- [Overview](#overview)
- [Platform Requirements](#platform-requirements)
- [Configuration](#configuration)
- [Interoperability](#interoperability)
- [Differences From Previous Releases](#differences-from-previous-releases)

---

## Overview

TLS 1.3 is negotiated automatically on Windows versions that support it. No configuration is
required to enable it, and there is no change to how certificates and keys are supplied — PEM key
files are configured exactly as before. When TLS 1.3 is not available, connections use TLS 1.2,
which remains fully supported.

## Platform Requirements

TLS 1.3 on Windows requires **Windows Server 2022 / Windows 11 (build 22000) or later**. Earlier
versions of Windows (including Windows Server 2019) do not expose TLS 1.3 through SChannel; on those
platforms MongoDB continues to use TLS 1.2.

## Configuration

- TLS 1.3 is enabled by default on supported platforms and is negotiated when both peers support it.
- `--tlsDisabledProtocols` accepts `TLS1_3` to disable TLS 1.3 explicitly, in the same way it
  accepts `TLS1_0`, `TLS1_1`, and `TLS1_2`.
- Certificate and key configuration (PEM files, certificate selectors, mutual TLS) is unchanged.

## Interoperability

Windows TLS 1.3 interoperates with both SChannel and non-SChannel peers, including OpenSSL-based
servers such as the KMIP servers used for Encrypted Storage Engine. TLS 1.3 features that occur
after the handshake — session-resumption tickets and traffic-key updates — are handled transparently
and require no configuration. Mutual (client-certificate) authentication is supported over TLS 1.3.

## Differences From Previous Releases

- **TLS 1.3 is now supported on Windows.** Previous releases negotiated at most TLS 1.2 through
  SChannel; on Windows Server 2022 / Windows 11, connections now negotiate TLS 1.3 when the peer
  supports it.
- **`--tlsDisabledProtocols` now recognizes `TLS1_3`.** Deployments that need to restrict the
  protocol can disable TLS 1.3 the same way as older protocol versions.
- **No change to certificate or key configuration**, and TLS 1.2 behavior is unchanged on platforms
  where TLS 1.3 is unavailable.
