#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Types.h>
#include <aws/crt/crypto/HMAC.h>
#include <aws/crt/crypto/Hash.h>
#include <aws/crt/mqtt/Mqtt5Client.h>
#include <aws/crt/mqtt/MqttClient.h>

#include <aws/common/logging.h>

namespace Aws
{
    namespace Crt
    {
        /**
         * Detail level control for logging output
         */
        enum class LogLevel
        {
            None = AWS_LL_NONE,
            Fatal = AWS_LL_FATAL,
            Error = AWS_LL_ERROR,
            Warn = AWS_LL_WARN,
            Info = AWS_LL_INFO,
            Debug = AWS_LL_DEBUG,
            Trace = AWS_LL_TRACE,

            Count
        };

        /**
         * Should the API Handle destructor block on all shutdown/thread completion logic or not?
         */
        enum class ApiHandleShutdownBehavior
        {
            Blocking,
            NonBlocking
        };

        /**
         * A singleton object representing the init/cleanup state of the entire CRT.  It's invalid to have more than one
         * active simultaneously and it's also invalid to use CRT functionality without one active.
         */
        class AWS_CRT_CPP_API ApiHandle
        {
          public:
            /**
             * Customize the ApiAllocator(), which is be used by any objects
             * constructed without an explicit allocator.
             */
            ApiHandle(Allocator *allocator) noexcept;
            ApiHandle() noexcept;
            ~ApiHandle();
            ApiHandle(const ApiHandle &) = delete;
            ApiHandle(ApiHandle &&) = delete;
            ApiHandle &operator=(const ApiHandle &) = delete;
            ApiHandle &operator=(ApiHandle &&) = delete;

            /**
             * Initialize logging in awscrt.
             * @param level: Display messages of this importance and higher. LogLevel.NoLogs will disable
             * logging.
             * @param filename: Logging destination, a file path from the disk.
             */
            void InitializeLogging(LogLevel level, const char *filename);

            /**
             * Initialize logging in awscrt.
             * @param level: Display messages of this importance and higher. LogLevel.NoLogs will disable
             * logging.
             * @param fp: The FILE object for logging destination.
             */
            void InitializeLogging(LogLevel level, FILE *fp);

            /**
             * Configures the shutdown behavior of the api handle instance
             * @param behavior desired shutdown behavior
             */
            void SetShutdownBehavior(ApiHandleShutdownBehavior behavior);

            /**
             * BYO_CRYPTO: set callback for creating MD5 hashes.
             * If using BYO_CRYPTO, you must call this.
             */
            void SetBYOCryptoNewMD5Callback(Crypto::CreateHashCallback &&callback);

            /**
             * BYO_CRYPTO: set callback for creating SHA256 hashes.
             * If using BYO_CRYPTO, you must call this.
             */
            void SetBYOCryptoNewSHA256Callback(Crypto::CreateHashCallback &&callback);

            /**
             * BYO_CRYPTO: set callback for creating SHA1 hashes.
             * If using BYO_CRYPTO, you must call this.
             */
            void SetBYOCryptoNewSHA1Callback(Crypto::CreateHashCallback &&callback);

            /**
             * BYO_CRYPTO: set callback for creating Streaming SHA256 HMAC objects.
             * If using BYO_CRYPTO, you must call this.
             */
            void SetBYOCryptoNewSHA256HMACCallback(Crypto::CreateHMACCallback &&callback);

            /**
             * BYO_CRYPTO: set callback for creating a ClientTlsChannelHandler.
             * If using BYO_CRYPTO, you must call this prior to creating any client channels in the
             * application.
             */
            void SetBYOCryptoClientTlsCallback(Io::NewClientTlsHandlerCallback &&callback);

            /**
             * BYO_CRYPTO: set callbacks for the TlsContext.
             * If using BYO_CRYPTO, you need to call this function prior to creating a TlsContext.
             *
             * @param newCallback Create custom implementation object, to be stored inside TlsContext.
             *                    Return nullptr if failure occurs.
             * @param deleteCallback Destroy object that was created by newCallback.
             * @param alpnCallback Return whether ALPN is supported.
             */
            void SetBYOCryptoTlsContextCallbacks(
                Io::NewTlsContextImplCallback &&newCallback,
                Io::DeleteTlsContextImplCallback &&deleteCallback,
                Io::IsTlsAlpnSupportedCallback &&alpnCallback);

            /// @private
            static const Io::NewTlsContextImplCallback &GetBYOCryptoNewTlsContextImplCallback();
            /// @private
            static const Io::DeleteTlsContextImplCallback &GetBYOCryptoDeleteTlsContextImplCallback();
            /// @private
            static const Io::IsTlsAlpnSupportedCallback &GetBYOCryptoIsTlsAlpnSupportedCallback();

            /**
             * Gets the static default ClientBootstrap, creating it if necessary.
             *
             * This default will be used when a ClientBootstrap is not explicitly passed but is needed
             * to allow the process to function. An example of this would be in the MQTT connection creation workflow.
             * The default ClientBootstrap will use the default EventLoopGroup and HostResolver, creating them if
             * necessary.
             *
             * The default ClientBootstrap will be automatically managed and released by the API handle when it's
             * resources are being freed, not requiring any manual memory management.
             *
             * @return ClientBootstrap* A pointer to the static default ClientBootstrap
             */
            static Io::ClientBootstrap *GetOrCreateStaticDefaultClientBootstrap();

            /**
             * Gets the static default EventLoopGroup, creating it if necessary.
             *
             * This default will be used when a EventLoopGroup is not explicitly passed but is needed
             * to allow the process to function. An example of this would be in the MQTT connection creation workflow.
             *
             * The EventLoopGroup will automatically pick a default number of threads based on the system. You can
             * manually adjust the number of threads being used by creating a EventLoopGroup and passing it through
             * the SetDefaultEventLoopGroup function.
             *
             * The default EventLoopGroup will be automatically managed and released by the API handle when it's
             * resources are being freed, not requiring any manual memory management.
             *
             * @return EventLoopGroup* A pointer to the static default EventLoopGroup
             */
            static Io::EventLoopGroup *GetOrCreateStaticDefaultEventLoopGroup();

            /**
             * Gets the static default HostResolver, creating it if necessary.
             *
             * This default will be used when a HostResolver is not explicitly passed but is needed
             * to allow the process to function. An example of this would be in the MQTT connection creation workflow.
             *
             * The HostResolver will be set to have a maximum of 8 entries by default. You can
             * manually adjust the maximum number of entries being used by creating a HostResolver and passing it
             * through the SetDefaultEventLoopGroup function.
             *
             * The default HostResolver will be automatically managed and released by the API handle when it's
             * resources are being freed, not requiring any manual memory management.
             *
             * @return HostResolver* A pointer to the static default HostResolver
             */
            static Io::HostResolver *GetOrCreateStaticDefaultHostResolver();

#pragma pack(push, 1)
            struct Version
            {
                uint16_t major;
                uint16_t minor;
                uint16_t patch;
            };
#pragma pack(pop)
            /**
             * Gets the version of the AWS-CRT-CPP library
             * @return Version representing the library version
             */
            Version GetCrtVersion() const;

          private:
            void InitializeLoggingCommon(struct aws_logger_standard_options &options);

            aws_logger m_logger;

            ApiHandleShutdownBehavior m_shutdownBehavior;

            static Io::ClientBootstrap *s_static_bootstrap;
            static std::mutex s_lock_client_bootstrap;
            static void ReleaseStaticDefaultClientBootstrap();

            static Io::EventLoopGroup *s_static_event_loop_group;
            static std::mutex s_lock_event_loop_group;
            static void ReleaseStaticDefaultEventLoopGroup();

            static int s_host_resolver_default_max_hosts;
            static Io::HostResolver *s_static_default_host_resolver;
            static std::mutex s_lock_default_host_resolver;
            static void ReleaseStaticDefaultHostResolver();

            Version m_version = {0, 0, 0};
        };

        /**
         * Gets a string description of a CRT error code
         * @param error error code to get a descriptive string for
         * @return a string description of the error code
         */
        AWS_CRT_CPP_API const char *ErrorDebugString(int error) noexcept;

        /**
         * @return the value of the last aws error on the current thread. Return 0 if no aws-error raised before.
         */
        AWS_CRT_CPP_API int LastError() noexcept;

        /**
         * @return the value of the last aws error on the current thread. Return AWS_ERROR_UNKNOWN, if no aws-error
         * raised before.
         */
        AWS_CRT_CPP_API int LastErrorOrUnknown() noexcept;
    } // namespace Crt
} // namespace Aws
