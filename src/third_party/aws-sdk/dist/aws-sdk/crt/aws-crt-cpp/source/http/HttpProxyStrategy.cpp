/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/http/HttpProxyStrategy.h>

#include <aws/common/string.h>
#include <aws/crt/http/HttpConnection.h>
#include <aws/http/proxy.h>

namespace Aws
{
    namespace Crt
    {
        namespace Http
        {
            HttpProxyStrategy::HttpProxyStrategy(struct aws_http_proxy_strategy *strategy) : m_strategy(strategy) {}

            HttpProxyStrategy::~HttpProxyStrategy()
            {
                aws_http_proxy_strategy_release(m_strategy);
            }

            HttpProxyStrategyBasicAuthConfig::HttpProxyStrategyBasicAuthConfig()
                : ConnectionType(AwsHttpProxyConnectionType::Legacy), Username(), Password()
            {
            }

            std::shared_ptr<HttpProxyStrategy> HttpProxyStrategy::CreateBasicHttpProxyStrategy(
                const HttpProxyStrategyBasicAuthConfig &config,
                Allocator *allocator)
            {
                struct aws_http_proxy_strategy_basic_auth_options basicConfig;
                AWS_ZERO_STRUCT(basicConfig);
                basicConfig.proxy_connection_type = (enum aws_http_proxy_connection_type)config.ConnectionType;
                basicConfig.user_name = aws_byte_cursor_from_c_str(config.Username.c_str());
                basicConfig.password = aws_byte_cursor_from_c_str(config.Password.c_str());

                struct aws_http_proxy_strategy *strategy =
                    aws_http_proxy_strategy_new_basic_auth(allocator, &basicConfig);
                if (strategy == NULL)
                {
                    return NULL;
                }

                return Aws::Crt::MakeShared<HttpProxyStrategy>(allocator, strategy);
            }

            class AdaptiveHttpProxyStrategy : public HttpProxyStrategy
            {
              public:
                AdaptiveHttpProxyStrategy(
                    Allocator *allocator,
                    const KerberosGetTokenFunction &kerberosGetToken,
                    const KerberosGetTokenFunction &ntlmGetCredential,
                    const NtlmGetTokenFunction &ntlmGetToken)
                    : HttpProxyStrategy(nullptr), m_Allocator(allocator), m_KerberosGetToken(kerberosGetToken),
                      m_NtlmGetCredential(ntlmGetCredential), m_NtlmGetToken(ntlmGetToken)
                {
                }

                void SetStrategy(struct aws_http_proxy_strategy *strategy)
                {
                    aws_http_proxy_strategy_release(m_strategy);
                    m_strategy = strategy;
                }

                static struct aws_string *NtlmGetCredential(void *user_data, int *error_code)
                {
                    AdaptiveHttpProxyStrategy *strategy = reinterpret_cast<AdaptiveHttpProxyStrategy *>(user_data);

                    String ntlmCredential;
                    if (strategy->m_NtlmGetCredential(ntlmCredential))
                    {
                        struct aws_string *token =
                            aws_string_new_from_c_str(strategy->m_Allocator, ntlmCredential.c_str());

                        if (token != NULL)
                        {
                            return token;
                        }

                        *error_code = aws_last_error();
                    }
                    else
                    {
                        *error_code = AWS_ERROR_HTTP_PROXY_STRATEGY_TOKEN_RETRIEVAL_FAILURE;
                    }

                    return NULL;
                }

                static struct aws_string *KerberosGetToken(void *user_data, int *error_code)
                {
                    AdaptiveHttpProxyStrategy *strategy = reinterpret_cast<AdaptiveHttpProxyStrategy *>(user_data);

                    String kerberosToken;
                    if (strategy->m_KerberosGetToken(kerberosToken))
                    {
                        struct aws_string *token =
                            aws_string_new_from_c_str(strategy->m_Allocator, kerberosToken.c_str());

                        if (token != NULL)
                        {
                            return token;
                        }

                        *error_code = aws_last_error();
                    }
                    else
                    {
                        *error_code = AWS_ERROR_HTTP_PROXY_STRATEGY_TOKEN_RETRIEVAL_FAILURE;
                    }

                    return NULL;
                }

                static struct aws_string *NtlmGetToken(
                    void *user_data,
                    const struct aws_byte_cursor *challenge_cursor,
                    int *error_code)
                {
                    AdaptiveHttpProxyStrategy *strategy = reinterpret_cast<AdaptiveHttpProxyStrategy *>(user_data);

                    String ntlmToken;
                    String challengeToken((const char *)challenge_cursor->ptr, challenge_cursor->len);
                    if (strategy->m_NtlmGetToken(challengeToken, ntlmToken))
                    {
                        struct aws_string *token = aws_string_new_from_c_str(strategy->m_Allocator, ntlmToken.c_str());

                        if (token != NULL)
                        {
                            return token;
                        }

                        *error_code = aws_last_error();
                    }
                    else
                    {
                        *error_code = AWS_ERROR_HTTP_PROXY_STRATEGY_TOKEN_RETRIEVAL_FAILURE;
                    }

                    return NULL;
                }

              private:
                Allocator *m_Allocator;

                KerberosGetTokenFunction m_KerberosGetToken;
                KerberosGetTokenFunction m_NtlmGetCredential;
                NtlmGetTokenFunction m_NtlmGetToken;
            };

            std::shared_ptr<HttpProxyStrategy> HttpProxyStrategy::CreateAdaptiveHttpProxyStrategy(
                const HttpProxyStrategyAdaptiveConfig &config,
                Allocator *allocator)
            {
                std::shared_ptr<AdaptiveHttpProxyStrategy> adaptiveStrategy =
                    Aws::Crt::MakeShared<AdaptiveHttpProxyStrategy>(
                        allocator, allocator, config.KerberosGetToken, config.NtlmGetCredential, config.NtlmGetToken);

                struct aws_http_proxy_strategy_tunneling_kerberos_options kerberosConfig;
                AWS_ZERO_STRUCT(kerberosConfig);
                kerberosConfig.get_token = AdaptiveHttpProxyStrategy::KerberosGetToken;
                kerberosConfig.get_token_user_data = adaptiveStrategy.get();

                struct aws_http_proxy_strategy_tunneling_ntlm_options ntlmConfig;
                AWS_ZERO_STRUCT(ntlmConfig);
                ntlmConfig.get_challenge_token = AdaptiveHttpProxyStrategy::NtlmGetToken;
                ntlmConfig.get_token = AdaptiveHttpProxyStrategy::NtlmGetCredential;
                ntlmConfig.get_challenge_token_user_data = adaptiveStrategy.get();

                struct aws_http_proxy_strategy_tunneling_adaptive_options adaptiveConfig;
                AWS_ZERO_STRUCT(adaptiveConfig);

                if (config.KerberosGetToken)
                {
                    adaptiveConfig.kerberos_options = &kerberosConfig;
                }

                if (config.NtlmGetToken)
                {
                    adaptiveConfig.ntlm_options = &ntlmConfig;
                }

                struct aws_http_proxy_strategy *strategy =
                    aws_http_proxy_strategy_new_tunneling_adaptive(allocator, &adaptiveConfig);
                if (strategy == NULL)
                {
                    return NULL;
                }

                adaptiveStrategy->SetStrategy(strategy);

                return adaptiveStrategy;
            }
        } // namespace Http
    } // namespace Crt
} // namespace Aws
