#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/Types.h>
#include <aws/crt/io/ChannelHandler.h>
#include <aws/io/tls_channel_handler.h>

#include <functional>
#include <memory>

struct aws_tls_ctx_options;

namespace Aws
{
    namespace Crt
    {
        namespace Io
        {
            class Pkcs11Lib;
            class TlsContextPkcs11Options;

            enum class TlsMode
            {
                CLIENT,
                SERVER,
            };

            /**
             * Top-level tls configuration options.  These options are used to create a context from which
             * per-connection TLS contexts can be created.
             */
            class AWS_CRT_CPP_API TlsContextOptions
            {
                friend class TlsContext;

              public:
                TlsContextOptions() noexcept;
                virtual ~TlsContextOptions();
                TlsContextOptions(const TlsContextOptions &) noexcept = delete;
                TlsContextOptions &operator=(const TlsContextOptions &) noexcept = delete;
                TlsContextOptions(TlsContextOptions &&) noexcept;
                TlsContextOptions &operator=(TlsContextOptions &&) noexcept;

                /**
                 * @return true if the instance is in a valid state, false otherwise.
                 */
                explicit operator bool() const noexcept { return m_isInit; }

                /**
                 * @return the value of the last aws error encountered by operations on this instance.
                 */
                int LastError() const noexcept;

                /**
                 * Initializes TlsContextOptions with secure by default options, with
                 * no client certificates.
                 */
                static TlsContextOptions InitDefaultClient(Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * Initializes TlsContextOptions for mutual TLS (mTLS), with
                 * client certificate and private key. These are paths to a file on disk. These files
                 * must be in the PEM format.
                 *
                 * NOTE: This is unsupported on iOS.
                 *
                 * @param cert_path: Path to certificate file.
                 * @param pkey_path: Path to private key file.
                 * @param allocator Memory allocator to use.
                 */
                static TlsContextOptions InitClientWithMtls(
                    const char *cert_path,
                    const char *pkey_path,
                    Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * Initializes TlsContextOptions for mutual TLS (mTLS), with
                 * client certificate and private key. These are in memory buffers. These buffers
                 * must be in the PEM format.
                 *
                 * NOTE: This is unsupported on iOS.
                 *
                 * @param cert: Certificate contents in memory.
                 * @param pkey: Private key contents in memory.
                 * @param allocator Memory allocator to use.
                 */
                static TlsContextOptions InitClientWithMtls(
                    const ByteCursor &cert,
                    const ByteCursor &pkey,
                    Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * Initializes TlsContextOptions for mutual TLS (mTLS),
                 * using a PKCS#11 library for private key operations.
                 *
                 * NOTE: This only works on Unix devices.
                 *
                 * @param pkcs11Options PKCS#11 options
                 * @param allocator Memory allocator to use.
                 */
                static TlsContextOptions InitClientWithMtlsPkcs11(
                    const TlsContextPkcs11Options &pkcs11Options,
                    Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * Initializes TlsContextOptions for mutual TLS (mTLS), with
                 * client certificate and private key in the PKCS#12 format.
                 *
                 * NOTE: This only works on Apple devices.
                 *
                 * @param pkcs12_path: Path to PKCS #12 file. The file is loaded from disk and stored internally. It
                 * must remain in memory for the lifetime of the returned object.
                 * @param pkcs12_pwd: Password to PKCS #12 file. It must remain in memory for the lifetime of the
                 * returned object.
                 * @param allocator Memory allocator to use.
                 */
                static TlsContextOptions InitClientWithMtlsPkcs12(
                    const char *pkcs12_path,
                    const char *pkcs12_pwd,
                    Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * @deprecated Custom keychain management is deprecated.
                 *
                 * By default the certificates and private keys are stored in the default keychain
                 * of the account of the process. If you instead wish to provide your own keychain
                 * for storing them, this makes the TlsContext to use that instead.
                 * NOTE: The password of your keychain must be empty.
                 *
                 * NOTE: This only works on MacOS.
                 */
                bool SetKeychainPath(ByteCursor &keychain_path) noexcept;

                /**
                 * Initializes TlsContextOptions for mutual TLS (mTLS),
                 * using a client certificate in a Windows certificate store.
                 *
                 * NOTE: This only works on Windows.
                 *
                 * @param windowsCertStorePath Path to certificate in a Windows certificate store.
                 *    The path must use backslashes and end with the certificate's thumbprint.
                 *    Example: `CurrentUser\MY\A11F8A9B5DF5B98BA3508FBCA575D09570E0D2C6`
                 * @param allocator The memory allocator to use.
                 */
                static TlsContextOptions InitClientWithMtlsSystemPath(
                    const char *windowsCertStorePath,
                    Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * @return true if alpn is supported by the underlying security provider, false
                 * otherwise.
                 */
                static bool IsAlpnSupported() noexcept;

                /**
                 * Sets the list of alpn protocols.
                 * @param alpnList: List of protocol names, delimited by ';'. This string must remain in memory for the
                 * lifetime of this object.
                 */
                bool SetAlpnList(const char *alpnList) noexcept;

                /**
                 * In client mode, this turns off x.509 validation. Don't do this unless you're testing.
                 * It's much better, to just override the default trust store and pass the self-signed
                 * certificate as the caFile argument.
                 *
                 * In server mode, this defaults to false. If you want to support mutual TLS from the server,
                 * you'll want to set this to true.
                 */
                void SetVerifyPeer(bool verifyPeer) noexcept;

                /**
                 * Sets the minimum TLS version allowed.
                 * @param minimumTlsVersion: The minimum TLS version.
                 */
                void SetMinimumTlsVersion(aws_tls_versions minimumTlsVersion);

                /**
                 * Sets the preferred TLS Cipher List
                 * @param cipher_pref: The preferred TLS cipher list.
                 */
                void SetTlsCipherPreference(aws_tls_cipher_pref cipher_pref);

                /**
                 * Overrides the default system trust store.
                 * @param caPath: Path to directory containing trusted certificates, which will overrides the
                 * default trust store. Only useful on Unix style systems where all anchors are stored in a directory
                 * (like /etc/ssl/certs). This string must remain in memory for the lifetime of this object.
                 * @param caFile: Path to file containing PEM armored chain of trusted CA certificates. This
                 * string must remain in memory for the lifetime of this object.
                 */
                bool OverrideDefaultTrustStore(const char *caPath, const char *caFile) noexcept;

                /**
                 * Overrides the default system trust store.
                 * @param ca: PEM armored chain of trusted CA certificates.
                 */
                bool OverrideDefaultTrustStore(const ByteCursor &ca) noexcept;

                /// @private
                const aws_tls_ctx_options *GetUnderlyingHandle() const noexcept { return &m_options; }

              private:
                aws_tls_ctx_options m_options;
                bool m_isInit;
            };

            /**
             * Options for TLS, when using a PKCS#11 library for private key operations.
             *
             * @see TlsContextOptions::InitClientWithMtlsPkcs11()
             */
            class AWS_CRT_CPP_API TlsContextPkcs11Options final
            {
              public:
                /**
                 * @param pkcs11Lib use this PKCS#11 library
                 * @param allocator Memory allocator to use.
                 */
                TlsContextPkcs11Options(
                    const std::shared_ptr<Pkcs11Lib> &pkcs11Lib,
                    Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * Use this PIN to log the user into the PKCS#11 token.
                 * Leave unspecified to log into a token with a "protected authentication path".
                 *
                 * @param pin PIN
                 */
                void SetUserPin(const String &pin) noexcept;

                /**
                 * Specify the slot ID containing a PKCS#11 token.
                 * If not specified, the token will be chosen based on other criteria (such as token label).
                 *
                 * @param id slot ID
                 */
                void SetSlotId(const uint64_t id) noexcept;

                /**
                 * Specify the label of the PKCS#11 token to use.
                 * If not specified, the token will be chosen based on other criteria (such as slot ID).
                 *
                 * @param label label of token
                 */
                void SetTokenLabel(const String &label) noexcept;

                /**
                 * Specify the label of the private key object on the PKCS#11 token.
                 * If not specified, the key will be chosen based on other criteria
                 * (such as being the only available private key on the token).
                 *
                 * @param label label of private key object
                 */
                void SetPrivateKeyObjectLabel(const String &label) noexcept;

                /**
                 * Use this X.509 certificate (file on disk).
                 * The certificate may be specified by other means instead (ex: SetCertificateFileContents())
                 *
                 * @param path path to PEM-formatted certificate file on disk.
                 */
                void SetCertificateFilePath(const String &path) noexcept;

                /**
                 * Use this X.509 certificate (contents in memory).
                 * The certificate may be specified by other means instead (ex: SetCertificateFilePath())
                 *
                 * @param contents contents of PEM-formatted certificate file.
                 */
                void SetCertificateFileContents(const String &contents) noexcept;

                /// @private
                aws_tls_ctx_pkcs11_options GetUnderlyingHandle() const noexcept;

              private:
                std::shared_ptr<Pkcs11Lib> m_pkcs11Lib;
                Optional<uint64_t> m_slotId;
                Optional<String> m_userPin;
                Optional<String> m_tokenLabel;
                Optional<String> m_privateKeyObjectLabel;
                Optional<String> m_certificateFilePath;
                Optional<String> m_certificateFileContents;
            };

            /**
             * Options specific to a single connection.
             */
            class AWS_CRT_CPP_API TlsConnectionOptions final
            {
              public:
                TlsConnectionOptions() noexcept;
                ~TlsConnectionOptions();
                TlsConnectionOptions(const TlsConnectionOptions &) noexcept;
                TlsConnectionOptions &operator=(const TlsConnectionOptions &) noexcept;
                TlsConnectionOptions(TlsConnectionOptions &&options) noexcept;
                TlsConnectionOptions &operator=(TlsConnectionOptions &&options) noexcept;

                /**
                 * Sets SNI extension, and also the name used for X.509 validation. serverName is copied.
                 *
                 * @return true if the copy succeeded, or false otherwise.
                 */
                bool SetServerName(ByteCursor &serverName) noexcept;

                /**
                 * Sets list of protocols (semi-colon delimited in priority order) used for ALPN extension.
                 * alpnList is copied.
                 *
                 * @return true if the copy succeeded, or false otherwise.
                 */
                bool SetAlpnList(const char *alpnList) noexcept;

                /**
                 * @return true if the instance is in a valid state, false otherwise.
                 */
                explicit operator bool() const noexcept { return isValid(); }

                /**
                 * @return the value of the last aws error encountered by operations on this instance.
                 */
                int LastError() const noexcept { return m_lastError; }

                /// @private
                const aws_tls_connection_options *GetUnderlyingHandle() const noexcept
                {
                    return &m_tls_connection_options;
                }

              private:
                bool isValid() const noexcept { return m_isInit; }

                TlsConnectionOptions(aws_tls_ctx *ctx, Allocator *allocator) noexcept;
                aws_tls_connection_options m_tls_connection_options;
                aws_allocator *m_allocator;
                int m_lastError;
                bool m_isInit;

                friend class TlsContext;
            };

            /**
             * Stateful context for TLS with a given configuration.  Per-connection TLS "contexts"
             * (TlsConnectionOptions) are instantiated from this as needed.
             */
            class AWS_CRT_CPP_API TlsContext final
            {
              public:
                TlsContext() noexcept;
                TlsContext(TlsContextOptions &options, TlsMode mode, Allocator *allocator = ApiAllocator()) noexcept;
                ~TlsContext() = default;
                TlsContext(const TlsContext &) noexcept = default;
                TlsContext &operator=(const TlsContext &) noexcept = default;
                TlsContext(TlsContext &&) noexcept = default;
                TlsContext &operator=(TlsContext &&) noexcept = default;

                /**
                 * @return a new connection-specific TLS context that can be configured with per-connection options
                 * (server name, peer verification, etc...)
                 */
                TlsConnectionOptions NewConnectionOptions() const noexcept;

                /**
                 * @return true if the instance is in a valid state, false otherwise.
                 */
                explicit operator bool() const noexcept { return isValid(); }

                /**
                 * @return the value of the last aws error encountered by operations on this instance.
                 */
                int GetInitializationError() const noexcept { return m_initializationError; }

                /// @private
                aws_tls_ctx *GetUnderlyingHandle() const noexcept { return m_ctx.get(); }

              private:
                bool isValid() const noexcept { return m_ctx && m_initializationError == AWS_ERROR_SUCCESS; }

                std::shared_ptr<aws_tls_ctx> m_ctx;
                int m_initializationError;
            };

            using NewTlsContextImplCallback = std::function<void *(TlsContextOptions &, TlsMode, Allocator *)>;
            using DeleteTlsContextImplCallback = std::function<void(void *)>;
            using IsTlsAlpnSupportedCallback = std::function<bool()>;

            /**
             * BYO_CRYPTO: TLS channel-handler base class.
             */
            class AWS_CRT_CPP_API TlsChannelHandler : public ChannelHandler
            {
              public:
                virtual ~TlsChannelHandler();

                /**
                 * @return negotiated protocol (or empty string if no agreed upon protocol)
                 */
                virtual String GetProtocol() const = 0;

              protected:
                TlsChannelHandler(
                    struct aws_channel_slot *slot,
                    const struct aws_tls_connection_options &options,
                    Allocator *allocator = ApiAllocator());

                /**
                 * Invoke this function from inside your handler after TLS negotiation completes. errorCode ==
                 * AWS_ERROR_SUCCESS or 0 means the session was successfully established and the connection should
                 * continue on.
                 */
                void CompleteTlsNegotiation(int errorCode);

              private:
                aws_tls_on_negotiation_result_fn *m_OnNegotiationResult;
                void *m_userData;

                aws_byte_buf m_protocolByteBuf;
                friend aws_byte_buf(::aws_tls_handler_protocol)(aws_channel_handler *);
            };

            /**
             * BYO_CRYPTO: Client TLS channel-handler base class.
             *
             * If using BYO_CRYPTO, you must define a concrete implementation
             * and set its creation callback via ApiHandle.SetBYOCryptoClientTlsCallback().
             */
            class AWS_CRT_CPP_API ClientTlsChannelHandler : public TlsChannelHandler
            {
              public:
                /**
                 * Initiates the TLS session negotiation. This is called by the common runtime when it's time to start
                 * a new session.
                 */
                virtual void StartNegotiation() = 0;

              protected:
                ClientTlsChannelHandler(
                    struct aws_channel_slot *slot,
                    const struct aws_tls_connection_options &options,
                    Allocator *allocator = ApiAllocator());
            };

            using NewClientTlsHandlerCallback = std::function<std::shared_ptr<ClientTlsChannelHandler>(
                struct aws_channel_slot *slot,
                const struct aws_tls_connection_options &options,
                Allocator *allocator)>;

        } // namespace Io
    } // namespace Crt
} // namespace Aws
