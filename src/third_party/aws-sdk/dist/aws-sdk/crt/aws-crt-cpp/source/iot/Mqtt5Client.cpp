/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/Api.h>
#include <aws/crt/auth/Credentials.h>
#include <aws/crt/auth/Sigv4Signing.h>
#include <aws/crt/http/HttpRequestResponse.h>
#include <aws/crt/io/Uri.h>
#include <aws/crt/mqtt/Mqtt5Packets.h>

#include <aws/iot/Mqtt5Client.h>

#if !BYO_CRYPTO

namespace Aws
{
    namespace Iot
    {
        static Crt::String AddToUsernameParameter(
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

        static bool buildMqtt5FinalUsername(
            Crt::Optional<Mqtt5CustomAuthConfig> customAuthConfig,
            Crt::String &username)
        {
            if (customAuthConfig.has_value())
            {
                /* If we're using token-signing authentication, then all token properties must be set */
                bool usingSigning = false;
                if (customAuthConfig->GetTokenValue().has_value() || customAuthConfig->GetTokenKeyName().has_value() ||
                    customAuthConfig->GetTokenSignature().has_value())
                {
                    usingSigning = true;
                    if (!customAuthConfig->GetTokenValue().has_value() ||
                        !customAuthConfig->GetTokenKeyName().has_value() ||
                        !customAuthConfig->GetTokenSignature().has_value())
                    {
                        return false;
                    }
                }
                Crt::String usernameString = "";

                if (!customAuthConfig->GetUsername().has_value())
                {
                    if (!username.empty())
                    {
                        usernameString += username;
                    }
                }
                else
                {
                    usernameString += customAuthConfig->GetUsername().value();
                }

                if (customAuthConfig->GetAuthorizerName().has_value())
                {
                    usernameString = AddToUsernameParameter(
                        usernameString, customAuthConfig->GetAuthorizerName().value(), "x-amz-customauthorizer-name=");
                }
                if (usingSigning)
                {
                    usernameString = AddToUsernameParameter(
                        usernameString,
                        customAuthConfig->GetTokenValue().value(),
                        customAuthConfig->GetTokenKeyName().value() + "=");
                    usernameString = AddToUsernameParameter(
                        usernameString,
                        customAuthConfig->GetTokenSignature().value(),
                        "x-amz-customauthorizer-signature=");
                }

                username = usernameString;
            }
            return true;
        }

        /*****************************************************
         *
         * Mqtt5ClientOptionsBuilder
         *
         *****************************************************/

        Mqtt5ClientBuilder::Mqtt5ClientBuilder(Crt::Allocator *allocator) noexcept
            : m_allocator(allocator), m_port(0), m_lastError(0), m_enableMetricsCollection(true)
        {
            m_options = new Crt::Mqtt5::Mqtt5ClientOptions(allocator);
        }

        Mqtt5ClientBuilder::Mqtt5ClientBuilder(int error, Crt::Allocator *allocator) noexcept
            : m_allocator(allocator), m_options(nullptr), m_lastError(error)
        {
        }

        Mqtt5ClientBuilder *Mqtt5ClientBuilder::NewMqtt5ClientBuilderWithMtlsFromPath(
            const Crt::String hostName,
            const char *certPath,
            const char *pkeyPath,
            Crt::Allocator *allocator) noexcept
        {
            Mqtt5ClientBuilder *result = new Mqtt5ClientBuilder(allocator);
            result->m_tlsConnectionOptions =
                Crt::Io::TlsContextOptions::InitClientWithMtls(certPath, pkeyPath, allocator);
            if (!result->m_tlsConnectionOptions.value())
            {
                int error_code = result->m_tlsConnectionOptions->LastError();
                AWS_LOGF_ERROR(
                    AWS_LS_MQTT5_GENERAL,
                    "Mqtt5ClientBuilder: Failed to setup TLS connection options with error %d:%s",
                    error_code,
                    aws_error_debug_str(error_code));
                delete result;
                return nullptr;
            }
            result->WithHostName(hostName);
            return result;
        }

        Mqtt5ClientBuilder *Mqtt5ClientBuilder::NewMqtt5ClientBuilderWithMtlsFromMemory(
            const Crt::String hostName,
            const Crt::ByteCursor &cert,
            const Crt::ByteCursor &pkey,
            Crt::Allocator *allocator) noexcept
        {
            Mqtt5ClientBuilder *result = new Mqtt5ClientBuilder(allocator);
            result->m_tlsConnectionOptions = Crt::Io::TlsContextOptions::InitClientWithMtls(cert, pkey, allocator);
            if (!result->m_tlsConnectionOptions.value())
            {
                int error_code = result->m_tlsConnectionOptions->LastError();
                AWS_LOGF_ERROR(
                    AWS_LS_MQTT5_GENERAL,
                    "Mqtt5ClientBuilder: Failed to setup TLS connection options with error %d:%s",
                    error_code,
                    aws_error_debug_str(error_code));
                delete result;
                return nullptr;
            }
            result->WithHostName(hostName);
            return result;
        }

        Mqtt5ClientBuilder *Mqtt5ClientBuilder::NewMqtt5ClientBuilderWithMtlsPkcs11(
            const Crt::String hostName,
            const Crt::Io::TlsContextPkcs11Options &pkcs11Options,
            Crt::Allocator *allocator) noexcept
        {
            Mqtt5ClientBuilder *result = new Mqtt5ClientBuilder(allocator);
            result->m_tlsConnectionOptions =
                Crt::Io::TlsContextOptions::InitClientWithMtlsPkcs11(pkcs11Options, allocator);
            if (!result->m_tlsConnectionOptions.value())
            {
                int error_code = result->m_tlsConnectionOptions->LastError();
                AWS_LOGF_ERROR(
                    AWS_LS_MQTT5_GENERAL,
                    "Mqtt5ClientBuilder: Failed to setup TLS connection options with error %d:%s",
                    error_code,
                    aws_error_debug_str(error_code));
                delete result;
                return nullptr;
            }
            result->WithHostName(hostName);
            return result;
        }

        Mqtt5ClientBuilder *Mqtt5ClientBuilder::NewMqtt5ClientBuilderWithMtlsPkcs12(
            const Crt::String hostName,
            const struct Pkcs12Options &options,
            Crt::Allocator *allocator) noexcept
        {
            Mqtt5ClientBuilder *result = new Mqtt5ClientBuilder(allocator);
            result->m_tlsConnectionOptions = Crt::Io::TlsContextOptions::InitClientWithMtlsPkcs12(
                options.pkcs12_file.c_str(), options.pkcs12_password.c_str(), allocator);
            if (!result->m_tlsConnectionOptions.value())
            {
                int error_code = result->m_tlsConnectionOptions->LastError();
                AWS_LOGF_ERROR(
                    AWS_LS_MQTT5_GENERAL,
                    "Mqtt5ClientBuilder: Failed to setup TLS connection options with error %d:%s",
                    error_code,
                    aws_error_debug_str(error_code));
                delete result;
                return nullptr;
            }
            result->WithHostName(hostName);
            return result;
        }

        Mqtt5ClientBuilder *Mqtt5ClientBuilder::NewMqtt5ClientBuilderWithWindowsCertStorePath(
            const Crt::String hostName,
            const char *windowsCertStorePath,
            Crt::Allocator *allocator) noexcept
        {
            Mqtt5ClientBuilder *result = new Mqtt5ClientBuilder(allocator);
            result->m_tlsConnectionOptions =
                Crt::Io::TlsContextOptions::InitClientWithMtlsSystemPath(windowsCertStorePath, allocator);
            if (!result->m_tlsConnectionOptions.value())
            {
                int error_code = result->m_tlsConnectionOptions->LastError();
                AWS_LOGF_ERROR(
                    AWS_LS_MQTT5_GENERAL,
                    "Mqtt5ClientBuilder: Failed to setup TLS connection options with error %d:%s",
                    error_code,
                    aws_error_debug_str(error_code));
                delete result;
                return nullptr;
            }
            result->WithHostName(hostName);
            return result;
        }

        Mqtt5ClientBuilder *Mqtt5ClientBuilder::NewMqtt5ClientBuilderWithWebsocket(
            const Crt::String hostName,
            const WebsocketConfig &config,
            Crt::Allocator *allocator) noexcept
        {
            Mqtt5ClientBuilder *result = new Mqtt5ClientBuilder(allocator);
            result->m_tlsConnectionOptions = Crt::Io::TlsContextOptions::InitDefaultClient();
            if (!result->m_tlsConnectionOptions.value())
            {
                int error_code = result->m_tlsConnectionOptions->LastError();
                AWS_LOGF_ERROR(
                    AWS_LS_MQTT5_GENERAL,
                    "Mqtt5ClientBuilder: Failed to setup TLS connection options with error %d:%s",
                    error_code,
                    aws_error_debug_str(error_code));
                delete result;
                return nullptr;
            }
            result->WithHostName(hostName);
            result->m_websocketConfig = config;
            return result;
        }

        Mqtt5ClientBuilder *Mqtt5ClientBuilder::NewMqtt5ClientBuilderWithCustomAuthorizer(
            const Crt::String hostName,
            const Mqtt5CustomAuthConfig &customAuthConfig,
            Crt::Allocator *allocator) noexcept
        {
            Mqtt5ClientBuilder *result = new Mqtt5ClientBuilder(allocator);
            result->m_tlsConnectionOptions = Crt::Io::TlsContextOptions::InitDefaultClient();
            if (!result->m_tlsConnectionOptions.value())
            {
                int error_code = result->m_tlsConnectionOptions->LastError();
                AWS_LOGF_ERROR(
                    AWS_LS_MQTT5_GENERAL,
                    "Mqtt5ClientBuilder: Failed to setup TLS connection options with error %d:%s",
                    error_code,
                    aws_error_debug_str(error_code));
                delete result;
                return nullptr;
            }
            result->WithHostName(hostName);
            result->WithCustomAuthorizer(customAuthConfig);
            return result;
        }

        Mqtt5ClientBuilder *Mqtt5ClientBuilder::NewMqtt5ClientBuilderWithCustomAuthorizerWebsocket(
            const Crt::String hostName,
            const Mqtt5CustomAuthConfig &customAuthConfig,
            const WebsocketConfig &config,
            Crt::Allocator *allocator) noexcept
        {
            Mqtt5ClientBuilder *result = new Mqtt5ClientBuilder(allocator);
            result->m_tlsConnectionOptions = Crt::Io::TlsContextOptions::InitDefaultClient();
            if (!result->m_tlsConnectionOptions.value())
            {
                int error_code = result->m_tlsConnectionOptions->LastError();
                AWS_LOGF_ERROR(
                    AWS_LS_MQTT5_GENERAL,
                    "Mqtt5ClientBuilder: Failed to setup TLS connection options with error %d:%s",
                    error_code,
                    aws_error_debug_str(error_code));
                delete result;
                return nullptr;
            }
            result->WithHostName(hostName);
            result->m_websocketConfig = config;
            result->WithCustomAuthorizer(customAuthConfig);
            return result;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithHostName(const Crt::String hostName)
        {
            m_options->WithHostName(hostName);
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithPort(uint32_t port) noexcept
        {
            m_port = port;
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithBootstrap(Crt::Io::ClientBootstrap *bootStrap) noexcept
        {
            m_options->WithBootstrap(bootStrap);
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithCertificateAuthority(const char *caPath) noexcept
        {
            if (m_tlsConnectionOptions)
            {
                if (!m_tlsConnectionOptions->OverrideDefaultTrustStore(nullptr, caPath))
                {
                    m_lastError = m_tlsConnectionOptions->LastError();
                }
            }
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithCertificateAuthority(const Crt::ByteCursor &cert) noexcept
        {
            if (m_tlsConnectionOptions)
            {
                if (!m_tlsConnectionOptions->OverrideDefaultTrustStore(cert))
                {
                    m_lastError = m_tlsConnectionOptions->LastError();
                }
            }
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithSocketOptions(Crt::Io::SocketOptions socketOptions) noexcept
        {
            m_options->WithSocketOptions(std::move(socketOptions));
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithHttpProxyOptions(
            const Crt::Http::HttpClientConnectionProxyOptions &proxyOptions) noexcept
        {
            m_proxyOptions = proxyOptions;
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithCustomAuthorizer(const Iot::Mqtt5CustomAuthConfig &config) noexcept
        {
            m_customAuthConfig = config;
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithConnectOptions(
            std::shared_ptr<ConnectPacket> packetConnect) noexcept
        {
            m_connectOptions = packetConnect;
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithSessionBehavior(ClientSessionBehaviorType sessionBehavior) noexcept
        {
            m_options->WithSessionBehavior(sessionBehavior);
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithClientExtendedValidationAndFlowControl(
            ClientExtendedValidationAndFlowControl clientExtendedValidationAndFlowControl) noexcept
        {
            m_options->WithClientExtendedValidationAndFlowControl(clientExtendedValidationAndFlowControl);
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithOfflineQueueBehavior(
            ClientOperationQueueBehaviorType operationQueueBehavior) noexcept
        {
            m_options->WithOfflineQueueBehavior(operationQueueBehavior);
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithReconnectOptions(ReconnectOptions reconnectOptions) noexcept
        {
            m_options->WithReconnectOptions(reconnectOptions);
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithTopicAliasingOptions(
            TopicAliasingOptions topicAliasingOptions) noexcept
        {
            m_options->WithTopicAliasingOptions(topicAliasingOptions);
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithPingTimeoutMs(uint32_t pingTimeoutMs) noexcept
        {
            m_options->WithPingTimeoutMs(pingTimeoutMs);
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithConnackTimeoutMs(uint32_t connackTimeoutMs) noexcept
        {
            m_options->WithConnackTimeoutMs(connackTimeoutMs);
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithAckTimeoutSec(uint32_t ackTimeoutSec) noexcept
        {
            m_options->WithAckTimeoutSec(ackTimeoutSec);
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithAckTimeoutSeconds(uint32_t ackTimeoutSec) noexcept
        {
            return WithAckTimeoutSec(ackTimeoutSec);
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithSdkName(const Crt::String &sdkName)
        {
            m_sdkName = sdkName;
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithSdkVersion(const Crt::String &sdkVersion)
        {
            m_sdkVersion = sdkVersion;
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithClientConnectionSuccessCallback(
            OnConnectionSuccessHandler callback) noexcept
        {
            m_options->WithClientConnectionSuccessCallback(std::move(callback));
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithClientConnectionFailureCallback(
            OnConnectionFailureHandler callback) noexcept
        {
            m_options->WithClientConnectionFailureCallback(std::move(callback));
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithClientDisconnectionCallback(
            OnDisconnectionHandler callback) noexcept
        {
            m_options->WithClientDisconnectionCallback(std::move(callback));
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithClientStoppedCallback(OnStoppedHandler callback) noexcept
        {
            m_options->WithClientStoppedCallback(std::move(callback));
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithClientAttemptingConnectCallback(
            OnAttemptingConnectHandler callback) noexcept
        {
            m_options->WithClientAttemptingConnectCallback(std::move(callback));
            return *this;
        }

        Mqtt5ClientBuilder &Mqtt5ClientBuilder::WithPublishReceivedCallback(OnPublishReceivedHandler callback) noexcept
        {
            m_options->WithPublishReceivedCallback(std::move(callback));
            return *this;
        }

        std::shared_ptr<Mqtt5Client> Mqtt5ClientBuilder::Build() noexcept
        {
            if (m_lastError != 0)
            {
                return nullptr;
            }

            uint32_t port = m_port;

            if (!port) // port is default to 0
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

            if (port == 443 && !m_websocketConfig && Crt::Io::TlsContextOptions::IsAlpnSupported() &&
                !m_customAuthConfig.has_value())
            {
                if (!m_tlsConnectionOptions->SetAlpnList("x-amzn-mqtt-ca"))
                {
                    return nullptr;
                }
            }

            if (m_customAuthConfig.has_value())
            {
                if (port != 443)
                {
                    AWS_LOGF_WARN(
                        AWS_LS_MQTT5_GENERAL,
                        "Attempting to connect to authorizer with unsupported port. Port is not 443...");
                }
                if (!m_websocketConfig)
                {
                    if (!m_tlsConnectionOptions->SetAlpnList("mqtt"))
                    {
                        return nullptr;
                    }
                }
            }

            // add metrics string to username (if metrics enabled)
            if (m_enableMetricsCollection || m_customAuthConfig.has_value())
            {
                Crt::String username = "";
                if (m_connectOptions != nullptr)
                {
                    if (m_connectOptions->getUsername().has_value())
                        username = m_connectOptions->getUsername().value();
                }
                else
                {
                    m_connectOptions = std::make_shared<ConnectPacket>(m_allocator);
                }

                if (m_customAuthConfig.has_value())
                {
                    if (!buildMqtt5FinalUsername(m_customAuthConfig, username))
                    {
                        AWS_LOGF_ERROR(
                            AWS_LS_MQTT5_GENERAL,
                            "Failed to setup CustomAuthorizerConfig, please check if the parameters are set "
                            "correctly.");
                        return nullptr;
                    }
                    if (m_customAuthConfig->GetPassword().has_value())
                    {
                        m_connectOptions->WithPassword(m_customAuthConfig->GetPassword().value());
                    }
                }

                if (m_enableMetricsCollection)
                {
                    username = AddToUsernameParameter(username, "SDK", m_sdkName);
                    username = AddToUsernameParameter(username, "Version", m_sdkName);
                }
                m_connectOptions->WithUserName(username);
            }

            auto tlsContext =
                Crt::Io::TlsContext(m_tlsConnectionOptions.value(), Crt::Io::TlsMode::CLIENT, m_allocator);
            if (!tlsContext)
            {
                return nullptr;
            }

            m_options->WithPort(port).WithTlsConnectionOptions(tlsContext.NewConnectionOptions());

            if (m_connectOptions != nullptr)
            {
                m_options->WithConnectOptions(m_connectOptions);
            }

            bool proxyOptionsSet = false;

            if (m_websocketConfig.has_value())
            {
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

                m_options->WithWebsocketHandshakeTransformCallback(signerTransform);
                bool useWebsocketProxyOptions =
                    m_websocketConfig->ProxyOptions.has_value() && !m_proxyOptions.has_value();
                if (useWebsocketProxyOptions)
                {
                    m_options->WithHttpProxyOptions(m_websocketConfig->ProxyOptions.value());
                    proxyOptionsSet = true;
                }
                else if (m_proxyOptions.has_value())
                {
                    m_options->WithHttpProxyOptions(m_proxyOptions.value());
                    proxyOptionsSet = true;
                }
            }

            if (m_proxyOptions.has_value() && !proxyOptionsSet)
            {
                m_options->WithHttpProxyOptions(m_proxyOptions.value());
            }

            return Crt::Mqtt5::Mqtt5Client::NewMqtt5Client(*m_options, m_allocator);
        }

        Aws::Iot::Mqtt5CustomAuthConfig::Mqtt5CustomAuthConfig(Crt::Allocator *allocator) noexcept
            : m_allocator(allocator)
        {
            AWS_ZERO_STRUCT(m_passwordStorage);
        }

        Aws::Iot::Mqtt5CustomAuthConfig::~Mqtt5CustomAuthConfig()
        {
            aws_byte_buf_clean_up(&m_passwordStorage);
        }

        Aws::Iot::Mqtt5CustomAuthConfig::Mqtt5CustomAuthConfig(const Mqtt5CustomAuthConfig &rhs)
        {
            if (&rhs != this)
            {
                m_allocator = rhs.m_allocator;
                if (rhs.m_authorizerName.has_value())
                {
                    m_authorizerName = rhs.m_authorizerName.value();
                }
                if (rhs.m_tokenKeyName.has_value())
                {
                    m_tokenKeyName = rhs.m_tokenKeyName.value();
                }
                if (rhs.m_tokenSignature.has_value())
                {
                    m_tokenSignature = rhs.m_tokenSignature.value();
                }
                if (rhs.m_tokenValue.has_value())
                {
                    m_tokenValue = rhs.m_tokenValue.value();
                }
                if (rhs.m_username.has_value())
                {
                    m_username = rhs.m_username.value();
                }
                if (rhs.m_password.has_value())
                {
                    AWS_ZERO_STRUCT(m_passwordStorage);
                    aws_byte_buf_init_copy_from_cursor(&m_passwordStorage, m_allocator, rhs.m_password.value());
                    m_password = aws_byte_cursor_from_buf(&m_passwordStorage);
                }
            }
        }

        Mqtt5CustomAuthConfig &Aws::Iot::Mqtt5CustomAuthConfig::operator=(const Mqtt5CustomAuthConfig &rhs)
        {
            if (&rhs != this)
            {
                m_allocator = rhs.m_allocator;
                if (rhs.m_authorizerName.has_value())
                {
                    m_authorizerName = rhs.m_authorizerName.value();
                }
                if (rhs.m_tokenKeyName.has_value())
                {
                    m_tokenKeyName = rhs.m_tokenKeyName.value();
                }
                if (rhs.m_tokenSignature.has_value())
                {
                    m_tokenSignature = rhs.m_tokenSignature.value();
                }
                if (rhs.m_tokenValue.has_value())
                {
                    m_tokenValue = rhs.m_tokenValue.value();
                }
                if (rhs.m_username.has_value())
                {
                    m_username = rhs.m_username.value();
                }
                if (rhs.m_password.has_value())
                {
                    aws_byte_buf_clean_up(&m_passwordStorage);
                    AWS_ZERO_STRUCT(m_passwordStorage);
                    aws_byte_buf_init_copy_from_cursor(&m_passwordStorage, m_allocator, rhs.m_password.value());
                    m_password = aws_byte_cursor_from_buf(&m_passwordStorage);
                }
            }
            return *this;
        }

        const Crt::Optional<Crt::String> &Mqtt5CustomAuthConfig::GetAuthorizerName()
        {
            return m_authorizerName;
        }

        const Crt::Optional<Crt::String> &Mqtt5CustomAuthConfig::GetUsername()
        {
            return m_username;
        }

        const Crt::Optional<Crt::ByteCursor> &Mqtt5CustomAuthConfig::GetPassword()
        {
            return m_password;
        }

        const Crt::Optional<Crt::String> &Mqtt5CustomAuthConfig::GetTokenKeyName()
        {
            return m_tokenKeyName;
        }

        const Crt::Optional<Crt::String> &Mqtt5CustomAuthConfig::GetTokenValue()
        {
            return m_tokenValue;
        }

        const Crt::Optional<Crt::String> &Mqtt5CustomAuthConfig::GetTokenSignature()
        {
            return m_tokenSignature;
        }

        Mqtt5CustomAuthConfig &Aws::Iot::Mqtt5CustomAuthConfig::WithAuthorizerName(Crt::String authName)
        {
            m_authorizerName = std::move(authName);
            return *this;
        }

        Mqtt5CustomAuthConfig &Aws::Iot::Mqtt5CustomAuthConfig::WithUsername(Crt::String username)
        {
            m_username = std::move(username);
            return *this;
        }

        Mqtt5CustomAuthConfig &Aws::Iot::Mqtt5CustomAuthConfig::WithPassword(Crt::ByteCursor password)
        {
            aws_byte_buf_clean_up(&m_passwordStorage);
            AWS_ZERO_STRUCT(m_passwordStorage);
            aws_byte_buf_init_copy_from_cursor(&m_passwordStorage, m_allocator, password);
            m_password = aws_byte_cursor_from_buf(&m_passwordStorage);
            return *this;
        }

        Mqtt5CustomAuthConfig &Aws::Iot::Mqtt5CustomAuthConfig::WithTokenKeyName(Crt::String tokenKeyName)
        {
            m_tokenKeyName = std::move(tokenKeyName);
            return *this;
        }

        Mqtt5CustomAuthConfig &Aws::Iot::Mqtt5CustomAuthConfig::WithTokenValue(Crt::String tokenValue)
        {
            m_tokenValue = std::move(tokenValue);
            return *this;
        }

        Mqtt5CustomAuthConfig &Aws::Iot::Mqtt5CustomAuthConfig::WithTokenSignature(Crt::String tokenSignature)
        {
            if (tokenSignature.find('%') != tokenSignature.npos)
            {
                // We can assume that a base 64 value that contains a '%' character has already been uri encoded
                m_tokenSignature = std::move(tokenSignature);
            }
            else
            {
                m_tokenSignature =
                    Aws::Crt::Io::EncodeQueryParameterValue(aws_byte_cursor_from_c_str(tokenSignature.c_str()));
            }

            return *this;
        }

    } // namespace Iot
} // namespace Aws

#endif // !BYO_CRYPTO
