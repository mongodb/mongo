/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Api.h>
#include <aws/crt/Config.h>
#include <aws/crt/JsonObject.h>
#include <aws/crt/StlAllocator.h>
#include <aws/crt/io/TlsOptions.h>

#include <aws/auth/auth.h>
#include <aws/common/ref_count.h>
#include <aws/event-stream/event_stream.h>
#include <aws/http/http.h>
#include <aws/mqtt/mqtt.h>
#include <aws/s3/s3.h>

#include <thread>

namespace Aws
{
    namespace Crt
    {
        static Crypto::CreateHashCallback s_BYOCryptoNewMD5Callback;
        static Crypto::CreateHashCallback s_BYOCryptoNewSHA256Callback;
        static Crypto::CreateHashCallback s_BYOCryptoNewSHA1Callback;
        static Crypto::CreateHMACCallback s_BYOCryptoNewSHA256HMACCallback;
        static Io::NewClientTlsHandlerCallback s_BYOCryptoNewClientTlsHandlerCallback;
        static Io::NewTlsContextImplCallback s_BYOCryptoNewTlsContextImplCallback;
        static Io::DeleteTlsContextImplCallback s_BYOCryptoDeleteTlsContextImplCallback;
        static Io::IsTlsAlpnSupportedCallback s_BYOCryptoIsTlsAlpnSupportedCallback;

        Io::ClientBootstrap *ApiHandle::s_static_bootstrap = nullptr;
        Io::EventLoopGroup *ApiHandle::s_static_event_loop_group = nullptr;
        int ApiHandle::s_host_resolver_default_max_hosts = 8;
        Io::HostResolver *ApiHandle::s_static_default_host_resolver = nullptr;
        std::mutex ApiHandle::s_lock_client_bootstrap;
        std::mutex ApiHandle::s_lock_event_loop_group;
        std::mutex ApiHandle::s_lock_default_host_resolver;

        ApiHandle::ApiHandle(Allocator *allocator) noexcept
            : m_logger(), m_shutdownBehavior(ApiHandleShutdownBehavior::Blocking),
              m_version({AWS_CRT_CPP_VERSION_MAJOR, AWS_CRT_CPP_VERSION_MINOR, AWS_CRT_CPP_VERSION_PATCH})
        {
            // sets up the StlAllocator for use.
            g_allocator = allocator;
            aws_mqtt_library_init(allocator);
            aws_s3_library_init(allocator);
            aws_event_stream_library_init(allocator);
            aws_sdkutils_library_init(allocator);

            JsonObject::OnLibraryInit();
        }

        ApiHandle::ApiHandle() noexcept : ApiHandle(DefaultAllocator()) {}

        ApiHandle::~ApiHandle()
        {
            ReleaseStaticDefaultClientBootstrap();
            ReleaseStaticDefaultEventLoopGroup();
            ReleaseStaticDefaultHostResolver();

            if (m_shutdownBehavior == ApiHandleShutdownBehavior::Blocking)
            {
                aws_thread_join_all_managed();
            }

            JsonObject::OnLibraryCleanup();

            if (aws_logger_get() == &m_logger)
            {
                aws_logger_set(NULL);
                aws_logger_clean_up(&m_logger);
            }

            g_allocator = nullptr;
            aws_s3_library_clean_up();
            aws_mqtt_library_clean_up();
            aws_event_stream_library_clean_up();
            aws_sdkutils_library_clean_up();

            s_BYOCryptoNewMD5Callback = nullptr;
            s_BYOCryptoNewSHA256Callback = nullptr;
            s_BYOCryptoNewSHA256HMACCallback = nullptr;
            s_BYOCryptoNewClientTlsHandlerCallback = nullptr;
            s_BYOCryptoNewTlsContextImplCallback = nullptr;
            s_BYOCryptoDeleteTlsContextImplCallback = nullptr;
            s_BYOCryptoIsTlsAlpnSupportedCallback = nullptr;
        }

        void ApiHandle::InitializeLogging(Aws::Crt::LogLevel level, const char *filename)
        {
            struct aws_logger_standard_options options;
            AWS_ZERO_STRUCT(options);

            options.level = (enum aws_log_level)level;
            options.filename = filename;

            InitializeLoggingCommon(options);
        }

        void ApiHandle::InitializeLogging(Aws::Crt::LogLevel level, FILE *fp)
        {
            struct aws_logger_standard_options options;
            AWS_ZERO_STRUCT(options);

            options.level = (enum aws_log_level)level;
            options.file = fp;

            InitializeLoggingCommon(options);
        }

        void ApiHandle::InitializeLoggingCommon(struct aws_logger_standard_options &options)
        {
            if (aws_logger_get() == &m_logger)
            {
                aws_logger_set(NULL);
                aws_logger_clean_up(&m_logger);
                if (options.level == AWS_LL_NONE)
                {
                    AWS_ZERO_STRUCT(m_logger);
                    return;
                }
            }

            if (aws_logger_init_standard(&m_logger, ApiAllocator(), &options))
            {
                return;
            }

            aws_logger_set(&m_logger);
        }

        void ApiHandle::SetShutdownBehavior(ApiHandleShutdownBehavior behavior)
        {
            m_shutdownBehavior = behavior;
        }

#if BYO_CRYPTO
        static struct aws_hash *s_MD5New(struct aws_allocator *allocator)
        {
            if (!s_BYOCryptoNewMD5Callback)
            {
                AWS_LOGF_ERROR(
                    AWS_LS_IO_TLS, "Must call ApiHandle::SetBYOCryptoNewMD5Callback() before MD5 hash can be created");
                aws_raise_error(AWS_ERROR_UNIMPLEMENTED);
                return nullptr;
            }

            auto hash = s_BYOCryptoNewMD5Callback(AWS_MD5_LEN, allocator);
            if (!hash)
            {
                return nullptr;
            }
            return hash->SeatForCInterop(hash);
        }

        void ApiHandle::SetBYOCryptoNewMD5Callback(Crypto::CreateHashCallback &&callback)
        {
            s_BYOCryptoNewMD5Callback = std::move(callback);
            aws_set_md5_new_fn(s_MD5New);
        }

        static struct aws_hash *s_Sha256New(struct aws_allocator *allocator)
        {
            if (!s_BYOCryptoNewSHA256Callback)
            {
                AWS_LOGF_ERROR(
                    AWS_LS_IO_TLS,
                    "Must call ApiHandle::SetBYOCryptoNewSHA256Callback() before SHA256 hash can be created");
                aws_raise_error(AWS_ERROR_UNIMPLEMENTED);
                return nullptr;
            }

            auto hash = s_BYOCryptoNewSHA256Callback(AWS_SHA256_LEN, allocator);
            if (!hash)
            {
                return nullptr;
            }
            return hash->SeatForCInterop(hash);
        }

        void ApiHandle::SetBYOCryptoNewSHA256Callback(Crypto::CreateHashCallback &&callback)
        {
            s_BYOCryptoNewSHA256Callback = std::move(callback);
            aws_set_sha256_new_fn(s_Sha256New);
        }

        static struct aws_hash *s_Sha1New(struct aws_allocator *allocator)
        {
            if (!s_BYOCryptoNewSHA1Callback)
            {
                AWS_LOGF_ERROR(
                    AWS_LS_IO_TLS,
                    "Must call ApiHandle::SetBYOCryptoNewSHA1Callback() before SHA1 hash can be created");
                aws_raise_error(AWS_ERROR_UNIMPLEMENTED);
                return nullptr;
            }

            auto hash = s_BYOCryptoNewSHA1Callback(AWS_SHA1_LEN, allocator);
            if (!hash)
            {
                return nullptr;
            }
            return hash->SeatForCInterop(hash);
        }

        void ApiHandle::SetBYOCryptoNewSHA1Callback(Crypto::CreateHashCallback &&callback)
        {
            s_BYOCryptoNewSHA1Callback = std::move(callback);
            aws_set_sha1_new_fn(s_Sha1New);
        }

        static struct aws_hmac *s_sha256HMACNew(struct aws_allocator *allocator, const struct aws_byte_cursor *secret)
        {
            if (!s_BYOCryptoNewSHA256HMACCallback)
            {
                AWS_LOGF_ERROR(
                    AWS_LS_IO_TLS,
                    "Must call ApiHandle::SetBYOCryptoNewSHA256HMACCallback() before SHA256 HMAC can be created");
                aws_raise_error(AWS_ERROR_UNIMPLEMENTED);
                return nullptr;
            }

            auto hmac = s_BYOCryptoNewSHA256HMACCallback(AWS_SHA256_HMAC_LEN, *secret, allocator);
            if (!hmac)
            {
                return nullptr;
            }
            return hmac->SeatForCInterop(hmac);
        }

        void ApiHandle::SetBYOCryptoNewSHA256HMACCallback(Crypto::CreateHMACCallback &&callback)
        {
            s_BYOCryptoNewSHA256HMACCallback = std::move(callback);
            aws_set_sha256_hmac_new_fn(s_sha256HMACNew);
        }

        static struct aws_channel_handler *s_NewClientTlsHandler(
            struct aws_allocator *allocator,
            struct aws_tls_connection_options *options,
            struct aws_channel_slot *slot,
            void *)
        {
            if (!s_BYOCryptoNewClientTlsHandlerCallback)
            {
                AWS_LOGF_ERROR(
                    AWS_LS_IO_TLS,
                    "Must call ApiHandle::SetBYOCryptoClientTlsCallback() before client TLS handler can be created");
                aws_raise_error(AWS_ERROR_UNIMPLEMENTED);
                return nullptr;
            }

            auto clientHandlerSelfReferencing = s_BYOCryptoNewClientTlsHandlerCallback(slot, *options, allocator);
            if (!clientHandlerSelfReferencing)
            {
                return nullptr;
            }
            return clientHandlerSelfReferencing->SeatForCInterop(clientHandlerSelfReferencing);
        }

        static int s_ClientTlsHandlerStartNegotiation(struct aws_channel_handler *handler, void *)
        {
            auto *clientHandler = reinterpret_cast<Io::ClientTlsChannelHandler *>(handler->impl);
            if (clientHandler->ChannelsThreadIsCallersThread())
            {
                clientHandler->StartNegotiation();
            }
            else
            {
                clientHandler->ScheduleTask([clientHandler](Io::TaskStatus) { clientHandler->StartNegotiation(); });
            }
            return AWS_OP_SUCCESS;
        }

        void ApiHandle::SetBYOCryptoClientTlsCallback(Io::NewClientTlsHandlerCallback &&callback)
        {
            s_BYOCryptoNewClientTlsHandlerCallback = std::move(callback);
            struct aws_tls_byo_crypto_setup_options setupOptions;
            setupOptions.new_handler_fn = s_NewClientTlsHandler;
            setupOptions.start_negotiation_fn = s_ClientTlsHandlerStartNegotiation;
            setupOptions.user_data = nullptr;
            aws_tls_byo_crypto_set_client_setup_options(&setupOptions);
        }

        void ApiHandle::SetBYOCryptoTlsContextCallbacks(
            Io::NewTlsContextImplCallback &&newCallback,
            Io::DeleteTlsContextImplCallback &&deleteCallback,
            Io::IsTlsAlpnSupportedCallback &&alpnCallback)
        {
            s_BYOCryptoNewTlsContextImplCallback = newCallback;
            s_BYOCryptoDeleteTlsContextImplCallback = deleteCallback;
            s_BYOCryptoIsTlsAlpnSupportedCallback = alpnCallback;
        }

#else  // BYO_CRYPTO
        void ApiHandle::SetBYOCryptoNewMD5Callback(Crypto::CreateHashCallback &&)
        {
            AWS_LOGF_WARN(AWS_LS_IO_TLS, "SetBYOCryptoNewMD5Callback() has no effect unless compiled with BYO_CRYPTO");
        }

        void ApiHandle::SetBYOCryptoNewSHA256Callback(Crypto::CreateHashCallback &&)
        {
            AWS_LOGF_WARN(
                AWS_LS_IO_TLS, "SetBYOCryptoNewSHA256Callback() has no effect unless compiled with BYO_CRYPTO");
        }

        void ApiHandle::SetBYOCryptoNewSHA1Callback(Crypto::CreateHashCallback &&)
        {
            AWS_LOGF_WARN(AWS_LS_IO_TLS, "SetBYOCryptoNewSHA1Callback() has no effect unless compiled with BYO_CRYPTO");
        }

        void ApiHandle::SetBYOCryptoNewSHA256HMACCallback(Crypto::CreateHMACCallback &&)
        {
            AWS_LOGF_WARN(
                AWS_LS_IO_TLS, "SetBYOCryptoNewSHA256HMACCallback() has no effect unless compiled with BYO_CRYPTO");
        }

        void ApiHandle::SetBYOCryptoClientTlsCallback(Io::NewClientTlsHandlerCallback &&)
        {
            AWS_LOGF_WARN(
                AWS_LS_IO_TLS, "SetBYOCryptoClientTlsCallback() has no effect unless compiled with BYO_CRYPTO");
        }

        void ApiHandle::SetBYOCryptoTlsContextCallbacks(
            Io::NewTlsContextImplCallback &&,
            Io::DeleteTlsContextImplCallback &&,
            Io::IsTlsAlpnSupportedCallback &&)
        {
            AWS_LOGF_WARN(
                AWS_LS_IO_TLS, "SetBYOCryptoClientTlsCallback() has no effect unless compiled with BYO_CRYPTO");
        }
#endif // BYO_CRYPTO

        Io::ClientBootstrap *ApiHandle::GetOrCreateStaticDefaultClientBootstrap()
        {
            std::lock_guard<std::mutex> lock(s_lock_client_bootstrap);
            if (s_static_bootstrap == nullptr)
            {
                s_static_bootstrap = Aws::Crt::New<Io::ClientBootstrap>(
                    ApiAllocator(), *GetOrCreateStaticDefaultEventLoopGroup(), *GetOrCreateStaticDefaultHostResolver());
            }
            return s_static_bootstrap;
        }

        Io::EventLoopGroup *ApiHandle::GetOrCreateStaticDefaultEventLoopGroup()
        {
            std::lock_guard<std::mutex> lock(s_lock_event_loop_group);
            if (s_static_event_loop_group == nullptr)
            {
                s_static_event_loop_group = Aws::Crt::New<Io::EventLoopGroup>(ApiAllocator(), (uint16_t)0);
            }
            return s_static_event_loop_group;
        }

        Io::HostResolver *ApiHandle::GetOrCreateStaticDefaultHostResolver()
        {
            std::lock_guard<std::mutex> lock(s_lock_default_host_resolver);
            if (s_static_default_host_resolver == nullptr)
            {
                s_static_default_host_resolver = Aws::Crt::New<Io::DefaultHostResolver>(
                    ApiAllocator(), *GetOrCreateStaticDefaultEventLoopGroup(), 1, s_host_resolver_default_max_hosts);
            }
            return s_static_default_host_resolver;
        }

        void ApiHandle::ReleaseStaticDefaultClientBootstrap()
        {
            std::lock_guard<std::mutex> lock(s_lock_client_bootstrap);
            if (s_static_bootstrap != nullptr)
            {
                Aws::Crt::Delete(s_static_bootstrap, ApiAllocator());
                s_static_bootstrap = nullptr;
            }
        }

        void ApiHandle::ReleaseStaticDefaultEventLoopGroup()
        {
            std::lock_guard<std::mutex> lock(s_lock_event_loop_group);
            if (s_static_event_loop_group != nullptr)
            {
                Aws::Crt::Delete(s_static_event_loop_group, ApiAllocator());
                s_static_event_loop_group = nullptr;
            }
        }

        void ApiHandle::ReleaseStaticDefaultHostResolver()
        {
            std::lock_guard<std::mutex> lock(s_lock_default_host_resolver);
            if (s_static_default_host_resolver != nullptr)
            {
                Aws::Crt::Delete(s_static_default_host_resolver, ApiAllocator());
                s_static_default_host_resolver = nullptr;
            }
        }

        const Io::NewTlsContextImplCallback &ApiHandle::GetBYOCryptoNewTlsContextImplCallback()
        {
            return s_BYOCryptoNewTlsContextImplCallback;
        }

        const Io::DeleteTlsContextImplCallback &ApiHandle::GetBYOCryptoDeleteTlsContextImplCallback()
        {
            return s_BYOCryptoDeleteTlsContextImplCallback;
        }

        const Io::IsTlsAlpnSupportedCallback &ApiHandle::GetBYOCryptoIsTlsAlpnSupportedCallback()
        {
            return s_BYOCryptoIsTlsAlpnSupportedCallback;
        }

        ApiHandle::Version ApiHandle::GetCrtVersion() const
        {
            return m_version;
        }

        const char *ErrorDebugString(int error) noexcept
        {
            return aws_error_debug_str(error);
        }

        int LastError() noexcept
        {
            return aws_last_error();
        }

        int LastErrorOrUnknown() noexcept
        {
            int last_error = aws_last_error();
            if (last_error == AWS_ERROR_SUCCESS)
            {
                last_error = AWS_ERROR_UNKNOWN;
            }

            return last_error;
        }

    } // namespace Crt
} // namespace Aws
