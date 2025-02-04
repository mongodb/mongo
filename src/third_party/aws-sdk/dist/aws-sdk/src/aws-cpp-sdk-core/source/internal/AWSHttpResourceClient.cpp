/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/internal/AWSHttpResourceClient.h>
#include <aws/core/client/DefaultRetryStrategy.h>
#include <aws/core/http/HttpClient.h>
#include <aws/core/http/HttpClientFactory.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/platform/Environment.h>
#include <aws/core/client/AWSError.h>
#include <aws/core/client/CoreErrors.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <mutex>
#include <sstream>

using namespace Aws;
using namespace Aws::Utils;
using namespace Aws::Utils::Logging;
using namespace Aws::Utils::Xml;
using namespace Aws::Http;
using namespace Aws::Client;
using namespace Aws::Internal;

static const char EC2_SECURITY_CREDENTIALS_RESOURCE[] = "/latest/meta-data/iam/security-credentials";
static const char EC2_REGION_RESOURCE[] = "/latest/meta-data/placement/availability-zone";
static const char EC2_IMDS_TOKEN_RESOURCE[] = "/latest/api/token";
static const char EC2_IMDS_TOKEN_TTL_DEFAULT_VALUE[] = "21600";
static const char EC2_IMDS_TOKEN_TTL_HEADER[] = "x-aws-ec2-metadata-token-ttl-seconds";
static const char EC2_IMDS_TOKEN_HEADER[] = "x-aws-ec2-metadata-token";
static const char RESOURCE_CLIENT_CONFIGURATION_ALLOCATION_TAG[] = "AWSHttpResourceClient";
static const char EC2_METADATA_CLIENT_LOG_TAG[] = "EC2MetadataClient";
static const char ECS_CREDENTIALS_CLIENT_LOG_TAG[] = "ECSCredentialsClient";
static const char SSO_GET_ROLE_RESOURCE[] = "/federation/credentials";

namespace Aws
{
    namespace Client
    {
        Aws::String ComputeUserAgentString(ClientConfiguration const * const pConfig);
    }

    namespace Internal
    {
        static ClientConfiguration MakeDefaultHttpResourceClientConfiguration(const char *logtag)
        {
            ClientConfiguration res;

            res.maxConnections = 2;
            res.scheme = Scheme::HTTP;

        #if defined(WIN32) && defined(BYPASS_DEFAULT_PROXY)
            // For security reasons, we must bypass any proxy settings when fetching sensitive information, for example
            // user credentials. On Windows, IXMLHttpRequest2 does not support bypassing proxy settings, therefore,
            // we force using WinHTTP client. On POSIX systems, CURL is set to bypass proxy settings by default.
            res.httpLibOverride = TransferLibType::WIN_HTTP_CLIENT;
            AWS_LOGSTREAM_INFO(logtag, "Overriding the current HTTP client to WinHTTP to bypass proxy settings.");
        #else
            (void) logtag;  // To disable warning about unused variable
        #endif
            // Explicitly set the proxy settings to empty/zero to avoid relying on defaults that could potentially change
            // in the future.
            res.proxyHost = "";
            res.proxyUserName = "";
            res.proxyPassword = "";
            res.proxyPort = 0;

            // EC2MetadataService throttles by delaying the response so the service client should set a large read timeout.
            // EC2MetadataService delay is in order of seconds so it only make sense to retry after a couple of seconds.
            res.connectTimeoutMs = 1000;
            res.requestTimeoutMs = 1000;
            res.retryStrategy = Aws::MakeShared<DefaultRetryStrategy>(RESOURCE_CLIENT_CONFIGURATION_ALLOCATION_TAG, 1, 1000);

            return res;
        }

        AWSHttpResourceClient::AWSHttpResourceClient(const Aws::Client::ClientConfiguration& clientConfiguration, const char* logtag)
        : m_logtag(logtag),
          m_userAgent(Aws::Client::ComputeUserAgentString(&clientConfiguration)),
          m_retryStrategy(clientConfiguration.retryStrategy ? clientConfiguration.retryStrategy : clientConfiguration.configFactories.retryStrategyCreateFn()),
          m_httpClient(nullptr)
        {
            AWS_LOGSTREAM_INFO(m_logtag.c_str(),
                               "Creating AWSHttpResourceClient with max connections "
                                << clientConfiguration.maxConnections
                                << " and scheme "
                                << SchemeMapper::ToString(clientConfiguration.scheme));

            m_httpClient = CreateHttpClient(clientConfiguration);
        }

        AWSHttpResourceClient::AWSHttpResourceClient(const char* logtag)
        : AWSHttpResourceClient(MakeDefaultHttpResourceClientConfiguration(logtag), logtag)
        {
        }

        AWSHttpResourceClient::~AWSHttpResourceClient()
        {
        }

        Aws::String AWSHttpResourceClient::GetResource(const char* endpoint, const char* resource, const char* authToken) const
        {
            return GetResourceWithAWSWebServiceResult(endpoint, resource, authToken).GetPayload();
        }

        AmazonWebServiceResult<Aws::String> AWSHttpResourceClient::GetResourceWithAWSWebServiceResult(const char *endpoint, const char *resource, const char *authToken) const
        {
            Aws::StringStream ss;
            ss << endpoint;
            if (resource)
            {
                ss << resource;
            }
            std::shared_ptr<HttpRequest> request(CreateHttpRequest(ss.str(), HttpMethod::HTTP_GET,
                                                                   Aws::Utils::Stream::DefaultResponseStreamFactoryMethod));

            request->SetUserAgent(m_userAgent);

            if (authToken)
            {
                request->SetHeaderValue(Aws::Http::AWS_AUTHORIZATION_HEADER, authToken);
            }

            return GetResourceWithAWSWebServiceResult(request);
        }

        AmazonWebServiceResult<Aws::String> AWSHttpResourceClient::GetResourceWithAWSWebServiceResult(const std::shared_ptr<HttpRequest> &httpRequest) const
        {
            AWS_LOGSTREAM_TRACE(m_logtag.c_str(), "Retrieving credentials from " << httpRequest->GetURIString());
            if (!m_httpClient)
            {
                AWS_LOGSTREAM_FATAL(m_logtag.c_str(), "Unable to get a response: missing http client!");
                return {{}, {}, HttpResponseCode::REQUEST_NOT_MADE};
            }

            for (long retries = 0;; retries++)
            {
                std::shared_ptr<HttpResponse> response(m_httpClient->MakeRequest(httpRequest));
                if (!response)
                {
                    AWS_LOGSTREAM_FATAL(m_logtag.c_str(), "Unable to get a response: http client returned a nullptr!");
                    return {{}, {}, HttpResponseCode::NO_RESPONSE};
                }

                if (response->GetResponseCode() == HttpResponseCode::OK)
                {
                    Aws::IStreamBufIterator eos;
                    return {Aws::String(Aws::IStreamBufIterator(response->GetResponseBody()), eos), response->GetHeaders(), HttpResponseCode::OK};
                }

                const Aws::Client::AWSError<Aws::Client::CoreErrors> error = [this, &response]() {
                    if (response->HasClientError() || response->GetResponseCode() == HttpResponseCode::REQUEST_NOT_MADE)
                    {
                        AWS_LOGSTREAM_ERROR(m_logtag.c_str(), "Http request to retrieve credentials failed");
                        return AWSError<CoreErrors>(CoreErrors::NETWORK_CONNECTION, true); // Retryable
                    }
                    else if (m_errorMarshaller && response->GetResponseBody().tellp() > 0)
                    {
                        return m_errorMarshaller->Marshall(*response);
                    }
                    else
                    {
                        const auto responseCode = response->GetResponseCode();

                        AWS_LOGSTREAM_ERROR(m_logtag.c_str(), "Http request to retrieve credentials failed with error code "
                                                              << static_cast<int>(responseCode));
                        return CoreErrorsMapper::GetErrorForHttpResponseCode(responseCode);
                    }
                }();

                if (!m_retryStrategy->ShouldRetry(error, retries))
                {
                    AWS_LOGSTREAM_ERROR(m_logtag.c_str(), "Can not retrieve resource from " << httpRequest->GetURIString());
                    return {{}, response->GetHeaders(), error.GetResponseCode()};
                }
                auto sleepMillis = m_retryStrategy->CalculateDelayBeforeNextRetry(error, retries);
                AWS_LOGSTREAM_WARN(m_logtag.c_str(), "Request failed, now waiting " << sleepMillis << " ms before attempting again.");
                m_httpClient->RetryRequestSleep(std::chrono::milliseconds(sleepMillis));
            }
        }

        EC2MetadataClient::EC2MetadataClient(const char *endpoint) :
            AWSHttpResourceClient(EC2_METADATA_CLIENT_LOG_TAG),
            m_endpoint(endpoint),
            m_disableIMDS(false),
            m_tokenRequired(true)
        {

        }

        EC2MetadataClient::EC2MetadataClient(const Aws::Client::ClientConfiguration &clientConfiguration,
            const char *endpoint) :
            AWSHttpResourceClient(clientConfiguration, EC2_METADATA_CLIENT_LOG_TAG),
            m_endpoint(endpoint),
            m_disableIMDS(clientConfiguration.disableIMDS),
            m_tokenRequired(true),
            m_disableIMDSV1(clientConfiguration.disableImdsV1)
        {
#if defined(DISABLE_IMDSV1)
            AWS_UNREFERENCED_PARAM(m_disableIMDSV1);
            m_disableIMDSV1 = true;
            AWS_LOGSTREAM_TRACE(m_logtag.c_str(), "IMDSv1 had been disabled at the SDK build time");
#endif
        }

        EC2MetadataClient::~EC2MetadataClient()
        {

        }

        Aws::String EC2MetadataClient::GetResource(const char* resourcePath) const
        {
            return GetResource(m_endpoint.c_str(), resourcePath, nullptr/*authToken*/);
        }

#if !defined(DISABLE_IMDSV1)
        Aws::String EC2MetadataClient::GetDefaultCredentials() const
        {
            if (m_disableIMDS) {
                AWS_LOGSTREAM_TRACE(m_logtag.c_str(), "Skipping call to IMDS Service");
                return {};
            }
            if (m_disableIMDSV1) {
                AWS_LOGSTREAM_INFO(m_logtag.c_str(), "Attempting to call IMDSv1 Service while disabled");
                return {};
            }
            std::unique_lock<std::recursive_mutex> locker(m_tokenMutex);
            if (m_tokenRequired)
            {
                return GetDefaultCredentialsSecurely();
            }

            AWS_LOGSTREAM_TRACE(m_logtag.c_str(), "Getting default credentials for ec2 instance from " << m_endpoint);
            auto result = GetResourceWithAWSWebServiceResult(m_endpoint.c_str(), EC2_SECURITY_CREDENTIALS_RESOURCE, nullptr);
            Aws::String credentialsString = result.GetPayload();
            auto httpResponseCode = result.GetResponseCode();

            // Note, if service is insane, it might return 404 for our initial secure call,
            // then when we fall back to insecure call, it might return 401 ask for secure call,
            // Then, SDK might get into a recursive loop call situation between secure and insecure call.
            if (httpResponseCode == Http::HttpResponseCode::UNAUTHORIZED)
            {
                m_tokenRequired = true;
                return {};
            }
            locker.unlock();

            Aws::String trimmedCredentialsString = StringUtils::Trim(credentialsString.c_str());
            if (trimmedCredentialsString.empty()) return {};

            Aws::Vector<Aws::String> securityCredentials = StringUtils::Split(trimmedCredentialsString, '\n');

            AWS_LOGSTREAM_DEBUG(m_logtag.c_str(), "Calling EC2MetadataService resource, " << EC2_SECURITY_CREDENTIALS_RESOURCE
                                                    << " returned credential string " << trimmedCredentialsString);

            if (securityCredentials.size() == 0)
            {
                AWS_LOGSTREAM_WARN(m_logtag.c_str(), "Initial call to ec2Metadataservice to get credentials failed");
                return {};
            }

            Aws::StringStream ss;
            ss << EC2_SECURITY_CREDENTIALS_RESOURCE << "/" << securityCredentials[0];
            AWS_LOGSTREAM_DEBUG(m_logtag.c_str(), "Calling EC2MetadataService resource " << ss.str());
            return GetResource(ss.str().c_str());
        }
#endif

        Aws::String EC2MetadataClient::GetDefaultCredentialsSecurely() const
        {
            if (m_disableIMDS) {
                AWS_LOGSTREAM_TRACE(m_logtag.c_str(), "Skipping call to IMDS Service");
                return {};
            }
            std::unique_lock<std::recursive_mutex> locker(m_tokenMutex);
#if !defined(DISABLE_IMDSV1)
            if (!m_disableIMDSV1 && !m_tokenRequired) {
                return GetDefaultCredentials();
            }
#endif

            Aws::StringStream ss;
            ss << m_endpoint << EC2_IMDS_TOKEN_RESOURCE;
            std::shared_ptr<HttpRequest> tokenRequest(CreateHttpRequest(ss.str(), HttpMethod::HTTP_PUT,
                                                                        Aws::Utils::Stream::DefaultResponseStreamFactoryMethod));
            tokenRequest->SetHeaderValue(EC2_IMDS_TOKEN_TTL_HEADER, EC2_IMDS_TOKEN_TTL_DEFAULT_VALUE);
            tokenRequest->SetUserAgent(m_userAgent);
            AWS_LOGSTREAM_TRACE(m_logtag.c_str(), "Calling EC2MetadataService to get token");
            auto result = GetResourceWithAWSWebServiceResult(tokenRequest);
            Aws::String tokenString = result.GetPayload();
            Aws::String trimmedTokenString = StringUtils::Trim(tokenString.c_str());

            if (result.GetResponseCode() == HttpResponseCode::BAD_REQUEST)
            {
                return {};
            }
#if !defined(DISABLE_IMDSV1)
            if (!m_disableIMDSV1 && (result.GetResponseCode() != HttpResponseCode::OK || trimmedTokenString.empty()))
            {
                m_tokenRequired = false;
                AWS_LOGSTREAM_TRACE(m_logtag.c_str(), "Calling EC2MetadataService to get token failed, falling back to less secure way.");
                return GetDefaultCredentials();
            }
#endif
            m_token = trimmedTokenString;
            locker.unlock();
            ss.str("");
            ss << m_endpoint << EC2_SECURITY_CREDENTIALS_RESOURCE;
            std::shared_ptr<HttpRequest> profileRequest(CreateHttpRequest(ss.str(), HttpMethod::HTTP_GET,
                                                                          Aws::Utils::Stream::DefaultResponseStreamFactoryMethod));
            profileRequest->SetHeaderValue(EC2_IMDS_TOKEN_HEADER, trimmedTokenString);
            profileRequest->SetUserAgent(m_userAgent);
            Aws::String profileString = GetResourceWithAWSWebServiceResult(profileRequest).GetPayload();

            Aws::String trimmedProfileString = StringUtils::Trim(profileString.c_str());
            Aws::Vector<Aws::String> securityCredentials = StringUtils::Split(trimmedProfileString, '\n');

            AWS_LOGSTREAM_DEBUG(m_logtag.c_str(), "Calling EC2MetadataService resource, " << EC2_SECURITY_CREDENTIALS_RESOURCE
                                                    << " with token returned profile string " << trimmedProfileString);
            if (securityCredentials.empty())
            {
                AWS_LOGSTREAM_WARN(m_logtag.c_str(), "Calling EC2Metadataservice to get profiles failed");
                return {};
            }

            ss.str("");
            ss << m_endpoint << EC2_SECURITY_CREDENTIALS_RESOURCE << "/" << securityCredentials[0];
            std::shared_ptr<HttpRequest> credentialsRequest(CreateHttpRequest(ss.str(), HttpMethod::HTTP_GET,
                                                                              Aws::Utils::Stream::DefaultResponseStreamFactoryMethod));
            credentialsRequest->SetHeaderValue(EC2_IMDS_TOKEN_HEADER, trimmedTokenString);
            credentialsRequest->SetUserAgent(m_userAgent);
            AWS_LOGSTREAM_DEBUG(m_logtag.c_str(), "Calling EC2MetadataService resource " << ss.str() << " with token.");
            return GetResourceWithAWSWebServiceResult(credentialsRequest).GetPayload();
        }

        Aws::String EC2MetadataClient::GetCurrentRegion() const
        {
            if (m_disableIMDS) {
                AWS_LOGSTREAM_TRACE(m_logtag.c_str(), "Skipping call to IMDS Service");
                return {};
            }
            if (!m_region.empty())
            {
                return m_region;
            }

            AWS_LOGSTREAM_TRACE(m_logtag.c_str(), "Getting current region for ec2 instance");

            Aws::StringStream ss;
            ss << m_endpoint << EC2_REGION_RESOURCE;
            std::shared_ptr<HttpRequest> regionRequest(CreateHttpRequest(ss.str(), HttpMethod::HTTP_GET,
                                                                         Aws::Utils::Stream::DefaultResponseStreamFactoryMethod));
            {
                std::lock_guard<std::recursive_mutex> locker(m_tokenMutex);
                if (m_tokenRequired)
                {
                    GetDefaultCredentialsSecurely();
                    regionRequest->SetHeaderValue(EC2_IMDS_TOKEN_HEADER, m_token);
                }
            }
            regionRequest->SetUserAgent(m_userAgent);
            Aws::String azString = GetResourceWithAWSWebServiceResult(regionRequest).GetPayload();

            if (azString.empty())
            {
                AWS_LOGSTREAM_INFO(m_logtag.c_str() ,
                        "Unable to pull region from instance metadata service ");
                return {};
            }

            Aws::String trimmedAZString = StringUtils::Trim(azString.c_str());
            AWS_LOGSTREAM_DEBUG(m_logtag.c_str(), "Calling EC2MetadataService resource "
                    << EC2_REGION_RESOURCE << " , returned credential string " << trimmedAZString);

            Aws::String region;
            region.reserve(trimmedAZString.length());

            bool digitFound = false;
            for (auto character : trimmedAZString)
            {
                if(digitFound && !isdigit(character))
                {
                    break;
                }
                if (isdigit(character))
                {
                    digitFound = true;
                }

                region.append(1, character);
            }

            AWS_LOGSTREAM_INFO(m_logtag.c_str(), "Detected current region as " << region);
            m_region = region;
            return region;
        }

        void EC2MetadataClient::SetEndpoint(const Aws::String& endpoint)
        {
            m_endpoint = endpoint;
        }

        Aws::String EC2MetadataClient::GetEndpoint() const
        {
            return Aws::String(m_endpoint);
        }

        #ifdef _MSC_VER
            // VS2015 compiler's bug, warning s_ec2metadataClient: symbol will be dynamically initialized (implementation limitation)
            AWS_SUPPRESS_WARNING(4592,
                static std::shared_ptr<EC2MetadataClient> s_ec2metadataClient(nullptr);
            )
        #else
            static std::shared_ptr<EC2MetadataClient> s_ec2metadataClient(nullptr);
        #endif

        void InitEC2MetadataClient()
        {
            if (s_ec2metadataClient)
            {
                return;
            }
            Aws::String ec2MetadataServiceEndpoint = Aws::Environment::GetEnv("AWS_EC2_METADATA_SERVICE_ENDPOINT");
            if (ec2MetadataServiceEndpoint.empty())
            {
                Aws::String ec2MetadataServiceEndpointMode = Aws::Environment::GetEnv("AWS_EC2_METADATA_SERVICE_ENDPOINT_MODE").c_str();
                if (ec2MetadataServiceEndpointMode.length() == 0 )
                {
                    ec2MetadataServiceEndpoint = "http://169.254.169.254"; //default to IPv4 default endpoint
                }
                else
                {
                    if (ec2MetadataServiceEndpointMode.length() == 4 )
                    {
                        if (Aws::Utils::StringUtils::CaselessCompare(ec2MetadataServiceEndpointMode.c_str(), "ipv4"))
                        {
                            ec2MetadataServiceEndpoint = "http://169.254.169.254"; //default to IPv4 default endpoint
                        }
                        else if (Aws::Utils::StringUtils::CaselessCompare(ec2MetadataServiceEndpointMode.c_str(), "ipv6"))
                        {
                            ec2MetadataServiceEndpoint = "http://[fd00:ec2::254]";
                        }
                        else
                        {
                            AWS_LOGSTREAM_ERROR(EC2_METADATA_CLIENT_LOG_TAG, "AWS_EC2_METADATA_SERVICE_ENDPOINT_MODE can only be set to ipv4 or ipv6, received: " << ec2MetadataServiceEndpointMode );
                        }
                    }
                    else
                    {
                        AWS_LOGSTREAM_ERROR(EC2_METADATA_CLIENT_LOG_TAG, "AWS_EC2_METADATA_SERVICE_ENDPOINT_MODE can only be set to ipv4 or ipv6, received: " << ec2MetadataServiceEndpointMode );
                    }
                }
            }
            AWS_LOGSTREAM_INFO(EC2_METADATA_CLIENT_LOG_TAG, "Using IMDS endpoint: " << ec2MetadataServiceEndpoint);
            s_ec2metadataClient = Aws::MakeShared<EC2MetadataClient>(EC2_METADATA_CLIENT_LOG_TAG, ec2MetadataServiceEndpoint.c_str());
        }

        void CleanupEC2MetadataClient()
        {
            if (!s_ec2metadataClient)
            {
                return;
            }
            s_ec2metadataClient = nullptr;
        }

        std::shared_ptr<EC2MetadataClient> GetEC2MetadataClient()
        {
            return s_ec2metadataClient;
        }

        ECSCredentialsClient::ECSCredentialsClient(const char* resourcePath, const char* endpoint, const char* token)
            : AWSHttpResourceClient(ECS_CREDENTIALS_CLIENT_LOG_TAG),
            m_resourcePath(resourcePath), m_endpoint(endpoint), m_token(token)
        {
        }

        ECSCredentialsClient::ECSCredentialsClient(const Aws::Client::ClientConfiguration& clientConfiguration, const char* resourcePath, const char* endpoint, const char* token)
            : AWSHttpResourceClient(clientConfiguration, ECS_CREDENTIALS_CLIENT_LOG_TAG),
            m_resourcePath(resourcePath), m_endpoint(endpoint), m_token(token)
        {
        }

        static const char STS_RESOURCE_CLIENT_LOG_TAG[] = "STSResourceClient";
        STSCredentialsClient::STSCredentialsClient(const Aws::Client::ClientConfiguration& clientConfiguration)
            : AWSHttpResourceClient(clientConfiguration, STS_RESOURCE_CLIENT_LOG_TAG)
        {
            SetErrorMarshaller(Aws::MakeUnique<Aws::Client::XmlErrorMarshaller>(STS_RESOURCE_CLIENT_LOG_TAG));

            Aws::StringStream ss;
            if (clientConfiguration.scheme == Aws::Http::Scheme::HTTP)
            {
                ss << "http://";
            }
            else
            {
                ss << "https://";
            }

            static const int CN_NORTH_1_HASH = Aws::Utils::HashingUtils::HashString(Aws::Region::CN_NORTH_1);
            static const int CN_NORTHWEST_1_HASH = Aws::Utils::HashingUtils::HashString(Aws::Region::CN_NORTHWEST_1);
            auto hash = Aws::Utils::HashingUtils::HashString(clientConfiguration.region.c_str());

            ss << "sts." << clientConfiguration.region << ".amazonaws.com";
            if (hash == CN_NORTH_1_HASH || hash == CN_NORTHWEST_1_HASH)
            {
                ss << ".cn";
            }
            m_endpoint =  ss.str();

            AWS_LOGSTREAM_INFO(STS_RESOURCE_CLIENT_LOG_TAG, "Creating STS ResourceClient with endpoint: " << m_endpoint);
        }

        STSCredentialsClient::STSAssumeRoleWithWebIdentityResult STSCredentialsClient::GetAssumeRoleWithWebIdentityCredentials(const STSAssumeRoleWithWebIdentityRequest& request)
        {
            //Calculate query string
            Aws::StringStream ss;
            ss << "Action=AssumeRoleWithWebIdentity"
                << "&Version=2011-06-15"
                << "&RoleSessionName=" << Aws::Utils::StringUtils::URLEncode(request.roleSessionName.c_str())
                << "&RoleArn=" << Aws::Utils::StringUtils::URLEncode(request.roleArn.c_str())
                << "&WebIdentityToken=" << Aws::Utils::StringUtils::URLEncode(request.webIdentityToken.c_str());

            std::shared_ptr<HttpRequest> httpRequest(CreateHttpRequest(m_endpoint, HttpMethod::HTTP_POST,
                                                                Aws::Utils::Stream::DefaultResponseStreamFactoryMethod));

            httpRequest->SetUserAgent(m_userAgent);

            std::shared_ptr<Aws::IOStream> body = Aws::MakeShared<Aws::StringStream>("STS_RESOURCE_CLIENT_LOG_TAG");
            *body << ss.str();

            httpRequest->AddContentBody(body);
            body->seekg(0, body->end);
            auto streamSize = body->tellg();
            body->seekg(0, body->beg);
            Aws::StringStream contentLength;
            contentLength << streamSize;
            httpRequest->SetContentLength(contentLength.str());
            httpRequest->SetContentType("application/x-www-form-urlencoded");

            Aws::String credentialsStr = GetResourceWithAWSWebServiceResult(httpRequest).GetPayload();

            //Parse credentials
            STSAssumeRoleWithWebIdentityResult result;
            if (credentialsStr.empty())
            {
                AWS_LOGSTREAM_WARN(STS_RESOURCE_CLIENT_LOG_TAG, "Get an empty credential from sts");
                return result;
            }

            const Utils::Xml::XmlDocument xmlDocument = XmlDocument::CreateFromXmlString(credentialsStr);
            XmlNode rootNode = xmlDocument.GetRootElement();
            XmlNode resultNode = rootNode;
            if (!rootNode.IsNull() && (rootNode.GetName() != "AssumeRoleWithWebIdentityResult"))
            {
                resultNode = rootNode.FirstChild("AssumeRoleWithWebIdentityResult");
            }

            if (!resultNode.IsNull())
            {
                XmlNode credentialsNode = resultNode.FirstChild("Credentials");
                if (!credentialsNode.IsNull())
                {
                    XmlNode accessKeyIdNode = credentialsNode.FirstChild("AccessKeyId");
                    if (!accessKeyIdNode.IsNull())
                    {
                        result.creds.SetAWSAccessKeyId(accessKeyIdNode.GetText());
                    }

                    XmlNode secretAccessKeyNode = credentialsNode.FirstChild("SecretAccessKey");
                    if (!secretAccessKeyNode.IsNull())
                    {
                        result.creds.SetAWSSecretKey(secretAccessKeyNode.GetText());
                    }

                    XmlNode sessionTokenNode = credentialsNode.FirstChild("SessionToken");
                    if (!sessionTokenNode.IsNull())
                    {
                        result.creds.SetSessionToken(sessionTokenNode.GetText());
                    }

                    XmlNode expirationNode = credentialsNode.FirstChild("Expiration");
                    if (!expirationNode.IsNull())
                    {
                        result.creds.SetExpiration(DateTime(StringUtils::Trim(expirationNode.GetText().c_str()).c_str(), DateFormat::ISO_8601));
                    }
                }
            }
            return result;
        }

        static const char SSO_RESOURCE_CLIENT_LOG_TAG[] = "SSOResourceClient";
        SSOCredentialsClient::SSOCredentialsClient(const Aws::Client::ClientConfiguration& clientConfiguration)
                : SSOCredentialsClient(clientConfiguration, clientConfiguration.scheme, clientConfiguration.region)
        {
        }

        SSOCredentialsClient::SSOCredentialsClient(const Aws::Client::ClientConfiguration& clientConfiguration, Aws::Http::Scheme scheme, const Aws::String& region)
                : AWSHttpResourceClient(clientConfiguration, SSO_RESOURCE_CLIENT_LOG_TAG)
        {
            SetErrorMarshaller(Aws::MakeUnique<Aws::Client::JsonErrorMarshaller>(SSO_RESOURCE_CLIENT_LOG_TAG));

            m_endpoint = buildEndpoint(scheme, region, "portal.sso.", "federation/credentials");
            m_oidcEndpoint = buildEndpoint(scheme, region, "oidc.", "token");

            AWS_LOGSTREAM_INFO(SSO_RESOURCE_CLIENT_LOG_TAG, "Creating SSO ResourceClient with endpoint: " << m_endpoint);
        }

        Aws::String SSOCredentialsClient::buildEndpoint(
            Aws::Http::Scheme scheme,
            const Aws::String& region,
            const Aws::String& domain,
            const Aws::String& endpoint)
        {
            Aws::StringStream ss;
            if (scheme == Aws::Http::Scheme::HTTP)
            {
                ss << "http://";
            }
            else
            {
                ss << "https://";
            }

            static const int CN_NORTH_1_HASH = Aws::Utils::HashingUtils::HashString(Aws::Region::CN_NORTH_1);
            static const int CN_NORTHWEST_1_HASH = Aws::Utils::HashingUtils::HashString(Aws::Region::CN_NORTHWEST_1);
            auto hash = Aws::Utils::HashingUtils::HashString(region.c_str());

            AWS_LOGSTREAM_DEBUG(SSO_RESOURCE_CLIENT_LOG_TAG, "Preparing SSO client for region: " << region);
            ss << domain << region << ".amazonaws.com/" << endpoint;
            if (hash == CN_NORTH_1_HASH || hash == CN_NORTHWEST_1_HASH)
            {
                ss << ".cn";
            }
            return ss.str();
        }

        SSOCredentialsClient::SSOGetRoleCredentialsResult SSOCredentialsClient::GetSSOCredentials(const SSOGetRoleCredentialsRequest &request)
        {
            Aws::StringStream ssUri;
            ssUri << m_endpoint << SSO_GET_ROLE_RESOURCE;

            std::shared_ptr<HttpRequest> httpRequest(CreateHttpRequest(m_endpoint, HttpMethod::HTTP_GET,
                                                                       Aws::Utils::Stream::DefaultResponseStreamFactoryMethod));

            httpRequest->SetHeaderValue("x-amz-sso_bearer_token", request.m_accessToken);

            httpRequest->SetUserAgent(m_userAgent);

            httpRequest->AddQueryStringParameter("account_id", Aws::Utils::StringUtils::URLEncode(request.m_ssoAccountId.c_str()));
            httpRequest->AddQueryStringParameter("role_name", Aws::Utils::StringUtils::URLEncode(request.m_ssoRoleName.c_str()));

            Aws::String credentialsStr = GetResourceWithAWSWebServiceResult(httpRequest).GetPayload();

            Json::JsonValue credentialsDoc(credentialsStr);
            AWS_LOGSTREAM_TRACE(SSO_RESOURCE_CLIENT_LOG_TAG, "Raw creds returned: " << credentialsStr);
            Aws::Auth::AWSCredentials creds;
            if (!credentialsDoc.WasParseSuccessful())
            {
                AWS_LOGSTREAM_ERROR(SSO_RESOURCE_CLIENT_LOG_TAG, "Failed to load credential from running. Error: " << credentialsStr);
                return SSOGetRoleCredentialsResult{creds};
            }
            Utils::Json::JsonView credentialsView(credentialsDoc);
            auto roleCredentials = credentialsView.GetObject("roleCredentials");
            creds.SetAWSAccessKeyId(roleCredentials.GetString("accessKeyId"));
            creds.SetAWSSecretKey(roleCredentials.GetString("secretAccessKey"));
            creds.SetSessionToken(roleCredentials.GetString("sessionToken"));
            creds.SetExpiration(roleCredentials.GetInt64("expiration"));
            SSOCredentialsClient::SSOGetRoleCredentialsResult result;
            result.creds = creds;
            return result;
        }

        // An internal SSO CreateToken implementation to lightweight core package and not introduce a dependency on sso-oidc
        SSOCredentialsClient::SSOCreateTokenResult SSOCredentialsClient::CreateToken(const SSOCreateTokenRequest& request)
        {
            std::shared_ptr<HttpRequest> httpRequest(CreateHttpRequest(m_oidcEndpoint, HttpMethod::HTTP_POST,
                                                                       Aws::Utils::Stream::DefaultResponseStreamFactoryMethod));
            SSOCreateTokenResult result;
            if(!httpRequest) {
                AWS_LOGSTREAM_FATAL(SSO_RESOURCE_CLIENT_LOG_TAG, "Failed to CreateHttpRequest: nullptr returned");
                return result;
            }
            httpRequest->SetUserAgent(ComputeUserAgentString());

            Json::JsonValue requestDoc;
            if(!request.clientId.empty()) {
                requestDoc.WithString("clientId", request.clientId);
            }
            if(!request.clientSecret.empty()) {
                requestDoc.WithString("clientSecret", request.clientSecret);
            }
            if(!request.grantType.empty()) {
                requestDoc.WithString("grantType", request.grantType);
            }
            if(!request.refreshToken.empty()) {
                requestDoc.WithString("refreshToken", request.refreshToken);
            }

            std::shared_ptr<Aws::IOStream> body = Aws::MakeShared<Aws::StringStream>("SSO_BEARER_TOKEN_CREATE_TOKEN");
            if(!body) {
                AWS_LOGSTREAM_FATAL(SSO_RESOURCE_CLIENT_LOG_TAG, "Failed to allocate body");  // exceptions disabled
                return result;
            }
            *body << requestDoc.View().WriteReadable();;

            httpRequest->AddContentBody(body);
            body->seekg(0, body->end);
            auto streamSize = body->tellg();
            body->seekg(0, body->beg);
            Aws::StringStream contentLength;
            contentLength << streamSize;
            httpRequest->SetContentLength(contentLength.str());
            httpRequest->SetContentType("application/json");

            Aws::String rawReply = GetResourceWithAWSWebServiceResult(httpRequest).GetPayload();
            Json::JsonValue refreshTokenDoc(rawReply);
            Utils::Json::JsonView jsonValue = refreshTokenDoc.View();

            if(jsonValue.ValueExists("accessToken")) {
                result.accessToken = jsonValue.GetString("accessToken");
            }
            if(jsonValue.ValueExists("tokenType")) {
                result.tokenType = jsonValue.GetString("tokenType");
            }
            if(jsonValue.ValueExists("expiresIn")) {
                result.expiresIn = jsonValue.GetInteger("expiresIn");
            }
            if(jsonValue.ValueExists("idToken")) {
                result.idToken = jsonValue.GetString("idToken");
            }
            if(jsonValue.ValueExists("refreshToken")) {
                result.refreshToken = jsonValue.GetString("refreshToken");
            }

            return result;
        }
    }
}
