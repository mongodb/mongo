/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/io/TlsOptions.h>

#include <aws/crt/io/Pkcs11.h>

#include <aws/crt/Api.h>
#include <aws/io/logging.h>
#include <aws/io/tls_channel_handler.h>

namespace Aws
{
    namespace Crt
    {
        namespace Io
        {
            TlsContextOptions::~TlsContextOptions()
            {
                if (m_isInit)
                {
                    aws_tls_ctx_options_clean_up(&m_options);
                }
            }

            TlsContextOptions::TlsContextOptions() noexcept : m_isInit(false)
            {
                AWS_ZERO_STRUCT(m_options);
            }

            TlsContextOptions::TlsContextOptions(TlsContextOptions &&other) noexcept
            {
                m_options = other.m_options;
                m_isInit = other.m_isInit;
                AWS_ZERO_STRUCT(other.m_options);
                other.m_isInit = false;
            }

            TlsContextOptions &TlsContextOptions::operator=(TlsContextOptions &&other) noexcept
            {
                if (&other != this)
                {
                    if (m_isInit)
                    {
                        aws_tls_ctx_options_clean_up(&m_options);
                    }

                    m_options = other.m_options;
                    m_isInit = other.m_isInit;
                    AWS_ZERO_STRUCT(other.m_options);
                    other.m_isInit = false;
                }

                return *this;
            }

            TlsContextOptions TlsContextOptions::InitDefaultClient(Allocator *allocator) noexcept
            {
                TlsContextOptions ctxOptions;
                aws_tls_ctx_options_init_default_client(&ctxOptions.m_options, allocator);
                ctxOptions.m_isInit = true;
                return ctxOptions;
            }

            TlsContextOptions TlsContextOptions::InitClientWithMtls(
                const char *certPath,
                const char *pKeyPath,
                Allocator *allocator) noexcept
            {
                TlsContextOptions ctxOptions;
                if (!aws_tls_ctx_options_init_client_mtls_from_path(
                        &ctxOptions.m_options, allocator, certPath, pKeyPath))
                {
                    ctxOptions.m_isInit = true;
                }
                return ctxOptions;
            }

            TlsContextOptions TlsContextOptions::InitClientWithMtls(
                const ByteCursor &cert,
                const ByteCursor &pkey,
                Allocator *allocator) noexcept
            {
                TlsContextOptions ctxOptions;
                if (!aws_tls_ctx_options_init_client_mtls(
                        &ctxOptions.m_options,
                        allocator,
                        const_cast<ByteCursor *>(&cert),
                        const_cast<ByteCursor *>(&pkey)))
                {
                    ctxOptions.m_isInit = true;
                }
                return ctxOptions;
            }

            TlsContextOptions TlsContextOptions::InitClientWithMtlsPkcs11(
                const TlsContextPkcs11Options &pkcs11Options,
                Allocator *allocator) noexcept
            {
                TlsContextOptions ctxOptions;
                aws_tls_ctx_pkcs11_options nativePkcs11Options = pkcs11Options.GetUnderlyingHandle();
                if (!aws_tls_ctx_options_init_client_mtls_with_pkcs11(
                        &ctxOptions.m_options, allocator, &nativePkcs11Options))
                {
                    ctxOptions.m_isInit = true;
                }
                return ctxOptions;
            }

            TlsContextOptions TlsContextOptions::InitClientWithMtlsPkcs12(
                const char *pkcs12Path,
                const char *pkcs12Pwd,
                Allocator *allocator) noexcept
            {
                TlsContextOptions ctxOptions;
                struct aws_byte_cursor password = aws_byte_cursor_from_c_str(pkcs12Pwd);
                if (!aws_tls_ctx_options_init_client_mtls_pkcs12_from_path(
                        &ctxOptions.m_options, allocator, pkcs12Path, &password))
                {
                    ctxOptions.m_isInit = true;
                }
                return ctxOptions;
            }

            bool TlsContextOptions::SetKeychainPath(ByteCursor &keychain_path) noexcept
            {
                AWS_ASSERT(m_isInit);
                return aws_tls_ctx_options_set_keychain_path(&m_options, &keychain_path) == 0;
            }

            TlsContextOptions TlsContextOptions::InitClientWithMtlsSystemPath(
                const char *windowsCertStorePath,
                Allocator *allocator) noexcept
            {
                TlsContextOptions ctxOptions;
                if (!aws_tls_ctx_options_init_client_mtls_from_system_path(
                        &ctxOptions.m_options, allocator, windowsCertStorePath))
                {
                    ctxOptions.m_isInit = true;
                }
                return ctxOptions;
            }

            int TlsContextOptions::LastError() const noexcept
            {
                return LastErrorOrUnknown();
            }

            bool TlsContextOptions::IsAlpnSupported() noexcept
            {
                return aws_tls_is_alpn_available();
            }

            bool TlsContextOptions::SetAlpnList(const char *alpn_list) noexcept
            {
                AWS_ASSERT(m_isInit);
                return aws_tls_ctx_options_set_alpn_list(&m_options, alpn_list) == 0;
            }

            void TlsContextOptions::SetVerifyPeer(bool verify_peer) noexcept
            {
                AWS_ASSERT(m_isInit);
                aws_tls_ctx_options_set_verify_peer(&m_options, verify_peer);
            }

            void TlsContextOptions::SetMinimumTlsVersion(aws_tls_versions minimumTlsVersion)
            {
                AWS_ASSERT(m_isInit);
                aws_tls_ctx_options_set_minimum_tls_version(&m_options, minimumTlsVersion);
            }

            void TlsContextOptions::SetTlsCipherPreference(aws_tls_cipher_pref cipher_pref)
            {
                AWS_ASSERT(m_isInit);
                aws_tls_ctx_options_set_tls_cipher_preference(&m_options, cipher_pref);
            }

            bool TlsContextOptions::OverrideDefaultTrustStore(const char *caPath, const char *caFile) noexcept
            {
                AWS_ASSERT(m_isInit);
                return aws_tls_ctx_options_override_default_trust_store_from_path(&m_options, caPath, caFile) == 0;
            }

            bool TlsContextOptions::OverrideDefaultTrustStore(const ByteCursor &ca) noexcept
            {
                AWS_ASSERT(m_isInit);
                return aws_tls_ctx_options_override_default_trust_store(&m_options, const_cast<ByteCursor *>(&ca)) == 0;
            }

            TlsContextPkcs11Options::TlsContextPkcs11Options(
                const std::shared_ptr<Pkcs11Lib> &pkcs11Lib,
                Allocator *) noexcept
                : m_pkcs11Lib{pkcs11Lib}
            {
            }

            void TlsContextPkcs11Options::SetUserPin(const String &pin) noexcept
            {
                m_userPin = pin;
            }

            void TlsContextPkcs11Options::SetSlotId(const uint64_t id) noexcept
            {
                m_slotId = id;
            }

            void TlsContextPkcs11Options::SetTokenLabel(const String &label) noexcept
            {
                m_tokenLabel = label;
            }

            void TlsContextPkcs11Options::SetPrivateKeyObjectLabel(const String &label) noexcept
            {
                m_privateKeyObjectLabel = label;
            }

            void TlsContextPkcs11Options::SetCertificateFilePath(const String &path) noexcept
            {
                m_certificateFilePath = path;
            }

            void TlsContextPkcs11Options::SetCertificateFileContents(const String &contents) noexcept
            {
                m_certificateFileContents = contents;
            }

            aws_tls_ctx_pkcs11_options TlsContextPkcs11Options::GetUnderlyingHandle() const noexcept
            {
                aws_tls_ctx_pkcs11_options options;
                AWS_ZERO_STRUCT(options);

                if (m_pkcs11Lib)
                {
                    options.pkcs11_lib = m_pkcs11Lib->GetNativeHandle();
                }

                if (m_slotId)
                {
                    options.slot_id = &(*m_slotId);
                }

                if (m_userPin)
                {
                    options.user_pin = ByteCursorFromString(*m_userPin);
                }

                if (m_tokenLabel)
                {
                    options.token_label = ByteCursorFromString(*m_tokenLabel);
                }

                if (m_privateKeyObjectLabel)
                {
                    options.private_key_object_label = ByteCursorFromString(*m_privateKeyObjectLabel);
                }

                if (m_certificateFilePath)
                {
                    options.cert_file_path = ByteCursorFromString(*m_certificateFilePath);
                }

                if (m_certificateFileContents)
                {
                    options.cert_file_contents = ByteCursorFromString(*m_certificateFileContents);
                }

                return options;
            }

            TlsConnectionOptions::TlsConnectionOptions() noexcept : m_lastError(AWS_ERROR_SUCCESS), m_isInit(false) {}

            TlsConnectionOptions::TlsConnectionOptions(aws_tls_ctx *ctx, Allocator *allocator) noexcept
                : m_allocator(allocator), m_lastError(AWS_ERROR_SUCCESS), m_isInit(true)
            {
                aws_tls_connection_options_init_from_ctx(&m_tls_connection_options, ctx);
            }

            TlsConnectionOptions::~TlsConnectionOptions()
            {
                if (m_isInit)
                {
                    aws_tls_connection_options_clean_up(&m_tls_connection_options);
                    m_isInit = false;
                }
            }

            TlsConnectionOptions::TlsConnectionOptions(const TlsConnectionOptions &options) noexcept
            {
                m_isInit = false;
                AWS_ZERO_STRUCT(m_tls_connection_options);

                if (options.m_isInit)
                {
                    m_allocator = options.m_allocator;

                    if (!aws_tls_connection_options_copy(&m_tls_connection_options, &options.m_tls_connection_options))
                    {
                        m_isInit = true;
                    }
                    else
                    {
                        m_lastError = LastErrorOrUnknown();
                    }
                }
            }

            TlsConnectionOptions &TlsConnectionOptions::operator=(const TlsConnectionOptions &options) noexcept
            {
                if (this != &options)
                {
                    if (m_isInit)
                    {
                        aws_tls_connection_options_clean_up(&m_tls_connection_options);
                    }

                    m_isInit = false;
                    AWS_ZERO_STRUCT(m_tls_connection_options);

                    if (options.m_isInit)
                    {
                        m_allocator = options.m_allocator;
                        if (!aws_tls_connection_options_copy(
                                &m_tls_connection_options, &options.m_tls_connection_options))
                        {
                            m_isInit = true;
                        }
                        else
                        {
                            m_lastError = LastErrorOrUnknown();
                        }
                    }
                }

                return *this;
            }

            TlsConnectionOptions::TlsConnectionOptions(TlsConnectionOptions &&options) noexcept
                : m_isInit(options.m_isInit)
            {
                if (options.m_isInit)
                {
                    m_tls_connection_options = options.m_tls_connection_options;
                    m_allocator = options.m_allocator;
                    AWS_ZERO_STRUCT(options.m_tls_connection_options);
                    options.m_isInit = false;
                }
            }

            TlsConnectionOptions &TlsConnectionOptions::operator=(TlsConnectionOptions &&options) noexcept
            {
                if (this != &options)
                {
                    if (m_isInit)
                    {
                        aws_tls_connection_options_clean_up(&m_tls_connection_options);
                    }

                    m_isInit = false;

                    if (options.m_isInit)
                    {
                        m_tls_connection_options = options.m_tls_connection_options;
                        AWS_ZERO_STRUCT(options.m_tls_connection_options);
                        options.m_isInit = false;
                        m_isInit = true;
                        m_allocator = options.m_allocator;
                    }
                }

                return *this;
            }

            bool TlsConnectionOptions::SetServerName(ByteCursor &serverName) noexcept
            {
                if (!isValid())
                {
                    m_lastError = LastErrorOrUnknown();
                    return false;
                }

                if (aws_tls_connection_options_set_server_name(&m_tls_connection_options, m_allocator, &serverName))
                {
                    m_lastError = LastErrorOrUnknown();
                    return false;
                }

                return true;
            }

            bool TlsConnectionOptions::SetAlpnList(const char *alpnList) noexcept
            {
                if (!isValid())
                {
                    m_lastError = LastErrorOrUnknown();
                    return false;
                }

                if (aws_tls_connection_options_set_alpn_list(&m_tls_connection_options, m_allocator, alpnList))
                {
                    m_lastError = LastErrorOrUnknown();
                    return false;
                }

                return true;
            }

            TlsContext::TlsContext() noexcept : m_ctx(nullptr), m_initializationError(AWS_ERROR_SUCCESS) {}

            TlsContext::TlsContext(TlsContextOptions &options, TlsMode mode, Allocator *allocator) noexcept
                : m_ctx(nullptr), m_initializationError(AWS_ERROR_SUCCESS)
            {
#if BYO_CRYPTO
                if (!ApiHandle::GetBYOCryptoNewTlsContextImplCallback() ||
                    !ApiHandle::GetBYOCryptoDeleteTlsContextImplCallback())
                {
                    AWS_LOGF_ERROR(
                        AWS_LS_IO_TLS,
                        "Must call ApiHandle::SetBYOCryptoTlsContextCallbacks() before TlsContext can be created");
                    m_initializationError = AWS_IO_TLS_CTX_ERROR;
                    return;
                }

                void *impl = ApiHandle::GetBYOCryptoNewTlsContextImplCallback()(options, mode, allocator);
                if (!impl)
                {
                    AWS_LOGF_ERROR(
                        AWS_LS_IO_TLS, "Creation callback from ApiHandle::SetBYOCryptoTlsContextCallbacks() failed");
                    m_initializationError = AWS_IO_TLS_CTX_ERROR;
                    return;
                }

                auto underlying_tls_ctx = static_cast<aws_tls_ctx *>(aws_mem_calloc(allocator, 1, sizeof(aws_tls_ctx)));
                underlying_tls_ctx->alloc = allocator;
                underlying_tls_ctx->impl = impl;

                aws_ref_count_init(
                    &underlying_tls_ctx->ref_count,
                    underlying_tls_ctx,
                    [](void *userdata)
                    {
                        auto dying_ctx = static_cast<aws_tls_ctx *>(userdata);
                        ApiHandle::GetBYOCryptoDeleteTlsContextImplCallback()(dying_ctx->impl);
                        aws_mem_release(dying_ctx->alloc, dying_ctx);
                    });

                m_ctx.reset(underlying_tls_ctx, aws_tls_ctx_release);
#else
                if (mode == TlsMode::CLIENT)
                {
                    aws_tls_ctx *underlying_tls_ctx = aws_tls_client_ctx_new(allocator, &options.m_options);
                    if (underlying_tls_ctx != nullptr)
                    {
                        m_ctx.reset(underlying_tls_ctx, aws_tls_ctx_release);
                    }
                }
                else
                {
                    aws_tls_ctx *underlying_tls_ctx = aws_tls_server_ctx_new(allocator, &options.m_options);
                    if (underlying_tls_ctx != nullptr)
                    {
                        m_ctx.reset(underlying_tls_ctx, aws_tls_ctx_release);
                    }
                }
                if (!m_ctx)
                {
                    m_initializationError = Aws::Crt::LastErrorOrUnknown();
                }
#endif // BYO_CRYPTO
            }

            TlsConnectionOptions TlsContext::NewConnectionOptions() const noexcept
            {
                if (!isValid())
                {
                    AWS_LOGF_ERROR(
                        AWS_LS_IO_TLS, "Trying to call TlsContext::NewConnectionOptions from an invalid TlsContext.");
                    return TlsConnectionOptions();
                }

                return TlsConnectionOptions(m_ctx.get(), m_ctx->alloc);
            }

            TlsChannelHandler::TlsChannelHandler(
                struct aws_channel_slot *,
                const struct aws_tls_connection_options &options,
                Allocator *allocator)
                : ChannelHandler(allocator)
            {
                m_OnNegotiationResult = options.on_negotiation_result;
                m_userData = options.user_data;
                aws_byte_buf_init(&m_protocolByteBuf, allocator, 16);
            }

            TlsChannelHandler::~TlsChannelHandler()
            {
                aws_byte_buf_clean_up(&m_protocolByteBuf);
            }

            void TlsChannelHandler::CompleteTlsNegotiation(int errorCode)
            {
                m_OnNegotiationResult(&this->m_handler, GetSlot(), errorCode, m_userData);
            }

            ClientTlsChannelHandler::ClientTlsChannelHandler(
                struct aws_channel_slot *slot,
                const struct aws_tls_connection_options &options,
                Allocator *allocator)
                : TlsChannelHandler(slot, options, allocator)
            {
            }

        } // namespace Io
    } // namespace Crt
} // namespace Aws

#if BYO_CRYPTO
AWS_EXTERN_C_BEGIN

bool aws_tls_is_alpn_available(void)
{
    const auto &callback = Aws::Crt::ApiHandle::GetBYOCryptoIsTlsAlpnSupportedCallback();
    if (!callback)
    {
        AWS_LOGF_ERROR(
            AWS_LS_IO_TLS, "Must call ApiHandle::SetBYOCryptoTlsContextCallbacks() before ALPN can be queried");
        return false;
    }
    return callback();
}

struct aws_byte_buf aws_tls_handler_protocol(struct aws_channel_handler *handler)
{
    auto *channelHandler = reinterpret_cast<Aws::Crt::Io::ChannelHandler *>(handler->impl);
    auto *tlsHandler = static_cast<Aws::Crt::Io::TlsChannelHandler *>(channelHandler);
    Aws::Crt::String protocolString = const_cast<const Aws::Crt::Io::TlsChannelHandler *>(tlsHandler)->GetProtocol();

    tlsHandler->m_protocolByteBuf.len = 0;
    aws_byte_cursor protocolCursor = Aws::Crt::ByteCursorFromString(protocolString);
    aws_byte_buf_append_dynamic(&tlsHandler->m_protocolByteBuf, &protocolCursor);
    return tlsHandler->m_protocolByteBuf;
}

AWS_EXTERN_C_END
#endif /* BYO_CRYPTO */
