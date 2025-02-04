/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/iot/MqttClient.h>

#include <aws/crt/Api.h>
#include <aws/crt/auth/Credentials.h>
#include <aws/crt/auth/Sigv4Signing.h>
#include <aws/crt/http/HttpRequestResponse.h>
#include <aws/crt/io/Uri.h>

#if !BYO_CRYPTO

namespace Aws
{
    namespace Iot
    {

        MqttClientConnectionConfig::MqttClientConnectionConfig(int lastError) noexcept
            : m_port(0), m_lastError(lastError)
        {
        }

        MqttClientConnectionConfig MqttClientConnectionConfig::CreateInvalid(int lastError) noexcept
        {
            return MqttClientConnectionConfig(lastError);
        }

        MqttClientConnectionConfig::MqttClientConnectionConfig(
            const Crt::String &endpoint,
            uint32_t port,
            const Crt::Io::SocketOptions &socketOptions,
            Crt::Io::TlsContext &&tlsContext)
            : m_endpoint(endpoint), m_port(port), m_context(std::move(tlsContext)), m_socketOptions(socketOptions),
              m_lastError(0)
        {
        }

        MqttClientConnectionConfig::MqttClientConnectionConfig(
            const Crt::String &endpoint,
            uint32_t port,
            const Crt::Io::SocketOptions &socketOptions,
            Crt::Io::TlsContext &&tlsContext,
            Crt::Mqtt::OnWebSocketHandshakeIntercept &&interceptor,
            const Crt::Optional<Crt::Http::HttpClientConnectionProxyOptions> &proxyOptions)
            : m_endpoint(endpoint), m_port(port), m_context(std::move(tlsContext)), m_socketOptions(socketOptions),
              m_webSocketInterceptor(std::move(interceptor)), m_proxyOptions(proxyOptions), m_lastError(0)
        {
        }

        MqttClientConnectionConfig::MqttClientConnectionConfig(
            const Crt::String &endpoint,
            uint32_t port,
            const Crt::Io::SocketOptions &socketOptions,
            Crt::Io::TlsContext &&tlsContext,
            const Crt::Optional<Crt::Http::HttpClientConnectionProxyOptions> &proxyOptions)
            : m_endpoint(endpoint), m_port(port), m_context(std::move(tlsContext)), m_socketOptions(socketOptions),
              m_proxyOptions(proxyOptions), m_lastError(0)
        {
        }

        MqttClientConnectionConfigBuilder::MqttClientConnectionConfigBuilder()
            : MqttClientConnectionConfigBuilder(Crt::ApiAllocator())
        {
            m_lastError = AWS_ERROR_INVALID_STATE;
        }

        // Common setup shared by all valid constructors
        MqttClientConnectionConfigBuilder::MqttClientConnectionConfigBuilder(Crt::Allocator *allocator) noexcept
            : m_allocator(allocator), m_portOverride(0),
#    ifdef AWS_IOT_SDK_VERSION
              m_sdkVersion(AWS_IOT_SDK_VERSION),
#    else
              m_sdkVersion(AWS_CRT_CPP_VERSION),
#    endif
              m_lastError(0)
        {
            m_socketOptions.SetConnectTimeoutMs(3000);
        }

        MqttClientConnectionConfigBuilder::MqttClientConnectionConfigBuilder(
            const char *certPath,
            const char *pkeyPath,
            Crt::Allocator *allocator) noexcept
            : MqttClientConnectionConfigBuilder(allocator)
        {
            m_contextOptions = Crt::Io::TlsContextOptions::InitClientWithMtls(certPath, pkeyPath, allocator);
            if (!m_contextOptions)
            {
                AWS_LOGF_ERROR(
                    AWS_LS_MQTT_CLIENT,
                    "id=%p: Error initializing TLS context from certificate and private key filepaths",
                    (void *)this);
                m_lastError = m_contextOptions.LastError();
                return;
            }
        }

        MqttClientConnectionConfigBuilder::MqttClientConnectionConfigBuilder(
            const Crt::ByteCursor &cert,
            const Crt::ByteCursor &pkey,
            Crt::Allocator *allocator) noexcept
            : MqttClientConnectionConfigBuilder(allocator)
        {
            m_contextOptions = Crt::Io::TlsContextOptions::InitClientWithMtls(cert, pkey, allocator);
            if (!m_contextOptions)
            {
                AWS_LOGF_ERROR(
                    AWS_LS_MQTT_CLIENT,
                    "id=%p: Error initializing TLS context from certificate and private key data",
                    (void *)this);
                m_lastError = m_contextOptions.LastError();
                return;
            }
        }

        MqttClientConnectionConfigBuilder::MqttClientConnectionConfigBuilder(
            const Crt::Io::TlsContextPkcs11Options &pkcs11Options,
            Crt::Allocator *allocator) noexcept
            : MqttClientConnectionConfigBuilder(allocator)
        {
            m_contextOptions = Crt::Io::TlsContextOptions::InitClientWithMtlsPkcs11(pkcs11Options, allocator);
            if (!m_contextOptions)
            {
                AWS_LOGF_ERROR(
                    AWS_LS_MQTT_CLIENT, "id=%p: Error initializing TLS context from PKCS11 options", (void *)this);
                m_lastError = m_contextOptions.LastError();
                return;
            }
        }

        MqttClientConnectionConfigBuilder::MqttClientConnectionConfigBuilder(
            const struct Pkcs12Options &options,
            Crt::Allocator *allocator) noexcept
            : MqttClientConnectionConfigBuilder(allocator)
        {
            m_contextOptions = Crt::Io::TlsContextOptions::InitClientWithMtlsPkcs12(
                options.pkcs12_file.c_str(), options.pkcs12_password.c_str(), allocator);
            if (!m_contextOptions)
            {
                m_lastError = m_contextOptions.LastError();
                return;
            }
        }

        MqttClientConnectionConfigBuilder::MqttClientConnectionConfigBuilder(
            const char *windowsCertStorePath,
            Crt::Allocator *allocator) noexcept
            : MqttClientConnectionConfigBuilder(allocator)
        {
            m_contextOptions =
                Crt::Io::TlsContextOptions::InitClientWithMtlsSystemPath(windowsCertStorePath, allocator);
            if (!m_contextOptions)
            {
                AWS_LOGF_ERROR(
                    AWS_LS_MQTT_CLIENT,
                    "id=%p: Error initializing TLS context from Windows Certificate Store data",
                    (void *)this);
                m_lastError = m_contextOptions.LastError();
                return;
            }
        }

        MqttClientConnectionConfigBuilder::MqttClientConnectionConfigBuilder(
            const WebsocketConfig &config,
            Crt::Allocator *allocator) noexcept
            : MqttClientConnectionConfigBuilder(allocator)
        {
            m_contextOptions = Crt::Io::TlsContextOptions::InitDefaultClient(allocator);
            if (!m_contextOptions)
            {
                AWS_LOGF_ERROR(
                    AWS_LS_MQTT_CLIENT, "id=%p: Error initializing default client TLS context", (void *)this);
                m_lastError = m_contextOptions.LastError();
                return;
            }

            m_websocketConfig = config;
        }

        MqttClientConnectionConfigBuilder MqttClientConnectionConfigBuilder::NewDefaultBuilder() noexcept
        {
            MqttClientConnectionConfigBuilder return_value =
                MqttClientConnectionConfigBuilder(Aws::Crt::ApiAllocator());
            return_value.m_contextOptions = Crt::Io::TlsContextOptions::InitDefaultClient();
            return return_value;
        }

        MqttClientConnectionConfigBuilder &MqttClientConnectionConfigBuilder::WithEndpoint(const Crt::String &endpoint)
        {
            m_endpoint = endpoint;
            return *this;
        }

        MqttClientConnectionConfigBuilder &MqttClientConnectionConfigBuilder::WithEndpoint(Crt::String &&endpoint)
        {
            m_endpoint = std::move(endpoint);
            return *this;
        }

        MqttClientConnectionConfigBuilder &MqttClientConnectionConfigBuilder::WithMetricsCollection(bool enabled)
        {
            m_enableMetricsCollection = enabled;
            return *this;
        }

        MqttClientConnectionConfigBuilder &MqttClientConnectionConfigBuilder::WithSdkName(const Crt::String &sdkName)
        {
            m_sdkName = sdkName;
            return *this;
        }

        MqttClientConnectionConfigBuilder &MqttClientConnectionConfigBuilder::WithSdkVersion(
            const Crt::String &sdkVersion)
        {
            m_sdkVersion = sdkVersion;
            return *this;
        }

        MqttClientConnectionConfigBuilder &MqttClientConnectionConfigBuilder::WithPortOverride(uint32_t port) noexcept
        {
            m_portOverride = port;
            return *this;
        }

        MqttClientConnectionConfigBuilder &MqttClientConnectionConfigBuilder::WithCertificateAuthority(
            const char *caPath) noexcept
        {
            if (m_contextOptions)
            {
                if (!m_contextOptions.OverrideDefaultTrustStore(nullptr, caPath))
                {
                    AWS_LOGF_WARN(AWS_LS_MQTT_CLIENT, "id=%p: Error overriding default trust store", (void *)this);
                    m_lastError = m_contextOptions.LastError();
                }
            }
            return *this;
        }

        MqttClientConnectionConfigBuilder &MqttClientConnectionConfigBuilder::WithCertificateAuthority(
            const Crt::ByteCursor &cert) noexcept
        {
            if (m_contextOptions)
            {
                if (!m_contextOptions.OverrideDefaultTrustStore(cert))
                {
                    AWS_LOGF_WARN(AWS_LS_MQTT_CLIENT, "id=%p: Error overriding default trust store", (void *)this);
                    m_lastError = m_contextOptions.LastError();
                }
            }
            return *this;
        }

        MqttClientConnectionConfigBuilder &MqttClientConnectionConfigBuilder::WithTcpKeepAlive() noexcept
        {
            m_socketOptions.SetKeepAlive(true);
            return *this;
        }
        MqttClientConnectionConfigBuilder &MqttClientConnectionConfigBuilder::WithTcpConnectTimeout(
            uint32_t connectTimeoutMs) noexcept
        {
            m_socketOptions.SetConnectTimeoutMs(connectTimeoutMs);
            return *this;
        }

        MqttClientConnectionConfigBuilder &MqttClientConnectionConfigBuilder::WithTcpKeepAliveTimeout(
            uint16_t keepAliveTimeoutSecs) noexcept
        {
            m_socketOptions.SetKeepAliveTimeoutSec(keepAliveTimeoutSecs);
            return *this;
        }

        MqttClientConnectionConfigBuilder &MqttClientConnectionConfigBuilder::WithTcpKeepAliveInterval(
            uint16_t keepAliveIntervalSecs) noexcept
        {
            m_socketOptions.SetKeepAliveIntervalSec(keepAliveIntervalSecs);
            return *this;
        }

        MqttClientConnectionConfigBuilder &MqttClientConnectionConfigBuilder::WithTcpKeepAliveMaxProbes(
            uint16_t maxProbes) noexcept
        {
            m_socketOptions.SetKeepAliveMaxFailedProbes(maxProbes);
            return *this;
        }

        MqttClientConnectionConfigBuilder &MqttClientConnectionConfigBuilder::WithMinimumTlsVersion(
            aws_tls_versions minimumTlsVersion) noexcept
        {
            m_contextOptions.SetMinimumTlsVersion(minimumTlsVersion);
            return *this;
        }

        MqttClientConnectionConfigBuilder &MqttClientConnectionConfigBuilder::WithHttpProxyOptions(
            const Crt::Http::HttpClientConnectionProxyOptions &proxyOptions) noexcept
        {
            m_proxyOptions = proxyOptions;
            return *this;
        }

        Crt::String MqttClientConnectionConfigBuilder::AddToUsernameParameter(
            Crt::String currentUsername,
            Crt::String parameterValue,
            Crt::String parameterPreText)
        {
            Crt::String return_string = currentUsername;
            if (return_string.find("?") != Crt::String::npos)
            {
                return_string += "&";
            }
            else
            {
                return_string += "?";
            }

            if (parameterValue.find(parameterPreText) != Crt::String::npos)
            {
                return return_string + parameterValue;
            }
            else
            {
                return return_string + parameterPreText + parameterValue;
            }
        }

        MqttClientConnectionConfigBuilder &MqttClientConnectionConfigBuilder::WithCustomAuthorizer(
            const Crt::String &username,
            const Crt::String &authorizerName,
            const Crt::String &authorizerSignature,
            const Crt::String &password) noexcept
        {
            return this->WithCustomAuthorizer(username, authorizerName, authorizerSignature, password, "", "");
        }

        MqttClientConnectionConfigBuilder &MqttClientConnectionConfigBuilder::WithCustomAuthorizer(
            const Crt::String &username,
            const Crt::String &authorizerName,
            const Crt::String &authorizerSignature,
            const Crt::String &password,
            const Crt::String &tokenKeyName,
            const Crt::String &tokenValue) noexcept
        {
            if (!m_contextOptions.IsAlpnSupported())
            {
                AWS_LOGF_ERROR(
                    AWS_LS_MQTT_CLIENT,
                    "id=%p: Alpn is not supported on this platform and therefore cannot use custom authentication",
                    (void *)this);
                m_lastError = AWS_ERROR_INVALID_STATE;
                return *this;
            }

            m_isUsingCustomAuthorizer = true;
            Crt::String usernameString = "";

            if (username.empty())
            {
                if (!m_username.empty())
                {
                    usernameString += m_username;
                }
            }
            else
            {
                usernameString += username;
            }

            if (!authorizerName.empty())
            {
                usernameString = AddToUsernameParameter(usernameString, authorizerName, "x-amz-customauthorizer-name=");
            }

            if (!authorizerSignature.empty() || !tokenKeyName.empty() || !tokenValue.empty())
            {
                if (authorizerSignature.empty() || tokenKeyName.empty() || tokenValue.empty())
                {
                    AWS_LOGF_WARN(
                        AWS_LS_MQTT_CLIENT,
                        "id=%p: Signed custom authorizers with signature will not work without a token key name and "
                        "token value. Your connection may be rejected/stalled on the IoT Core side due to this. Please "
                        "use the non-deprecated API and pass both the token key name and token value to connect to a "
                        "signed custom authorizer.",
                        (void *)this);
                }
            }

            if (!authorizerSignature.empty())
            {
                Crt::String encodedSignature;
                if (authorizerSignature.find('%') != authorizerSignature.npos)
                {
                    // We can assume that a base 64 value that contains a '%' character has already been uri encoded
                    encodedSignature = std::move(authorizerSignature);
                }
                else
                {
                    encodedSignature = Aws::Crt::Io::EncodeQueryParameterValue(
                        aws_byte_cursor_from_c_str(authorizerSignature.c_str()));
                }

                usernameString =
                    AddToUsernameParameter(usernameString, encodedSignature, "x-amz-customauthorizer-signature=");
            }

            if (!tokenKeyName.empty() && !tokenValue.empty())
            {
                usernameString = AddToUsernameParameter(usernameString, tokenValue, tokenKeyName + "=");
            }

            m_username = usernameString;
            m_password = password;

            if (!m_websocketConfig)
            {
                if (!m_contextOptions.SetAlpnList("mqtt"))
                {
                    m_lastError = m_contextOptions.LastError();
                }
                m_portOverride = 443;
            }
            return *this;
        }

        MqttClientConnectionConfigBuilder &MqttClientConnectionConfigBuilder::WithUsername(
            const Crt::String &username) noexcept
        {
            m_username = username;
            return *this;
        }

        MqttClientConnectionConfigBuilder &MqttClientConnectionConfigBuilder::WithPassword(
            const Crt::String &password) noexcept
        {
            m_password = password;
            return *this;
        }

        MqttClientConnectionConfig MqttClientConnectionConfigBuilder::Build() noexcept
        {
            if (m_lastError != 0)
            {
                return MqttClientConnectionConfig::CreateInvalid(m_lastError);
            }

            uint32_t port = m_portOverride;

            if (!m_portOverride)
            {
                if (m_websocketConfig || Crt::Io::TlsContextOptions::IsAlpnSupported())
                {
                    port = 443;
                }
                else
                {
                    port = 8883;
                }
            }

            Crt::String username = m_username;
            Crt::String password = m_password;

            // Check to see if a custom authorizer is being used but not through the builder
            if (!m_isUsingCustomAuthorizer)
            {
                if (!m_username.empty())
                {
                    if (m_username.find_first_of("x-amz-customauthorizer-name=") != Crt::String::npos ||
                        m_username.find_first_of("x-amz-customauthorizer-signature=") != Crt::String::npos)
                    {
                        m_isUsingCustomAuthorizer = true;
                    }
                }
            }

            if (port == 443 && !m_websocketConfig && Crt::Io::TlsContextOptions::IsAlpnSupported() &&
                !m_isUsingCustomAuthorizer)
            {
                if (!m_contextOptions.SetAlpnList("x-amzn-mqtt-ca"))
                {
                    return MqttClientConnectionConfig::CreateInvalid(m_contextOptions.LastError());
                }
            }

            // Is the user trying to connect using a custom authorizer?
            if (m_isUsingCustomAuthorizer)
            {
                if (port != 443)
                {
                    AWS_LOGF_WARN(
                        AWS_LS_MQTT_GENERAL,
                        "Attempting to connect to authorizer with unsupported port. Port is not 443...");
                }
            }

            // add metrics string to username (if metrics enabled)
            if (m_enableMetricsCollection)
            {
                if (username.find('?') != Crt::String::npos)
                {
                    username += "&";
                }
                else
                {
                    username += "?";
                }
                username += "SDK=";
                username += m_sdkName;
                username += "&Version=";
                username += m_sdkVersion;
            }

            auto tlsContext = Crt::Io::TlsContext(m_contextOptions, Crt::Io::TlsMode::CLIENT, m_allocator);
            if (!tlsContext)
            {
                return MqttClientConnectionConfig::CreateInvalid(tlsContext.GetInitializationError());
            }

            if (!m_websocketConfig)
            {
                auto config = MqttClientConnectionConfig(
                    m_endpoint, port, m_socketOptions, std::move(tlsContext), m_proxyOptions);
                config.m_username = username;
                config.m_password = password;
                return config;
            }

            auto websocketConfig = m_websocketConfig.value();
            auto signerTransform = [websocketConfig](
                                       std::shared_ptr<Crt::Http::HttpRequest> req,
                                       const Crt::Mqtt::OnWebSocketHandshakeInterceptComplete &onComplete)
            {
                // it is only a very happy coincidence that these function signatures match. This is the callback
                // for signing to be complete. It invokes the callback for websocket handshake to be complete.
                auto signingComplete =
                    [onComplete](const std::shared_ptr<Aws::Crt::Http::HttpRequest> &req1, int errorCode)
                { onComplete(req1, errorCode); };

                auto signerConfig = websocketConfig.CreateSigningConfigCb();

                websocketConfig.Signer->SignRequest(req, *signerConfig, signingComplete);
            };

            bool useWebsocketProxyOptions = m_websocketConfig->ProxyOptions.has_value() && !m_proxyOptions.has_value();

            auto config = MqttClientConnectionConfig(
                m_endpoint,
                port,
                m_socketOptions,
                std::move(tlsContext),
                signerTransform,
                useWebsocketProxyOptions ? m_websocketConfig->ProxyOptions : m_proxyOptions);
            config.m_username = username;
            config.m_password = password;
            return config;
        }

        MqttClient::MqttClient(Crt::Io::ClientBootstrap &bootstrap, Crt::Allocator *allocator) noexcept
            : m_client(bootstrap, allocator), m_lastError(0)
        {
            if (!m_client)
            {
                m_lastError = m_client.LastError();
            }
        }

        MqttClient::MqttClient(Crt::Allocator *allocator) noexcept
            : MqttClient(*Crt::ApiHandle::GetOrCreateStaticDefaultClientBootstrap(), allocator)
        {
        }

        std::shared_ptr<Crt::Mqtt::MqttConnection> MqttClient::NewConnection(
            const MqttClientConnectionConfig &config) noexcept
        {
            if (!config)
            {
                m_lastError = config.LastError();
                return nullptr;
            }

            bool useWebsocket = config.m_webSocketInterceptor.operator bool();
            auto newConnection = m_client.NewConnection(
                config.m_endpoint.c_str(), config.m_port, config.m_socketOptions, config.m_context, useWebsocket);

            if (!newConnection)
            {
                m_lastError = m_client.LastError();
                return nullptr;
            }

            if (!(*newConnection))
            {
                m_lastError = newConnection->LastError();
                return nullptr;
            }

            if (!config.m_username.empty() || !config.m_password.empty())
            {
                if (!newConnection->SetLogin(config.m_username.c_str(), config.m_password.c_str()))
                {
                    m_lastError = newConnection->LastError();
                    return nullptr;
                }
            }

            if (useWebsocket)
            {
                newConnection->WebsocketInterceptor = config.m_webSocketInterceptor;
            }

            if (config.m_proxyOptions)
            {
                newConnection->SetHttpProxyOptions(config.m_proxyOptions.value());
            }

            return newConnection;
        }
    } // namespace Iot
} // namespace Aws

#endif // !BYO_CRYPTO
