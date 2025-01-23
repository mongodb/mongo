/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#include <aws/core/auth/GeneralHTTPCredentialsProvider.h>

#include <aws/crt/Api.h>
#include <aws/core/platform/Environment.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <fstream>

using namespace Aws::Auth;

static const char GEN_HTTP_LOG_TAG[] = "GeneralHTTPCredentialsProvider";

const char GeneralHTTPCredentialsProvider::AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE[] = "AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE";
const char GeneralHTTPCredentialsProvider::AWS_CONTAINER_CREDENTIALS_RELATIVE_URI[] = "AWS_CONTAINER_CREDENTIALS_RELATIVE_URI";
const char GeneralHTTPCredentialsProvider::AWS_CONTAINER_CREDENTIALS_FULL_URI[]     = "AWS_CONTAINER_CREDENTIALS_FULL_URI";
const char GeneralHTTPCredentialsProvider::AWS_CONTAINER_AUTHORIZATION_TOKEN[]      = "AWS_CONTAINER_AUTHORIZATION_TOKEN";

const char GeneralHTTPCredentialsProvider::AWS_ECS_CONTAINER_HOST[]      = "169.254.170.2";
const char GeneralHTTPCredentialsProvider::AWS_EKS_CONTAINER_HOST[]      = "169.254.170.23";
const char GeneralHTTPCredentialsProvider::AWS_EKS_CONTAINER_HOST_IPV6[] = "fd00:ec2::23";


bool IsAllowedIp(const Aws::String& authority)
{
    // address is an ECS / EKS container host
    if (authority == GeneralHTTPCredentialsProvider::AWS_ECS_CONTAINER_HOST) // ECS container host
    {
        return true;
    } else if (authority == GeneralHTTPCredentialsProvider::AWS_EKS_CONTAINER_HOST ||
               authority == GeneralHTTPCredentialsProvider::AWS_EKS_CONTAINER_HOST_IPV6) { // EKS container host
        return true;
    }

    // address is within the loop back
    // IPv4
    if (authority.rfind(Aws::String("127.0.0."), 0) == 0 && authority.size() > 8 && authority.size() <= 11)
    {
        Aws::String octet = authority.substr(8);
        if ((octet.find_first_not_of("0123456789") != Aws::String::npos) ||
            (Aws::Utils::StringUtils::ConvertToInt32(octet.c_str()) > 255)) {
            AWS_LOGSTREAM_WARN(GEN_HTTP_LOG_TAG, "Can't use General HTTP Provider: AWS_CONTAINER_CREDENTIALS_FULL_URI ip address is malformed: " << authority);
            return false;
        } else {
            return true;
        }
    }

    // IPv6
    if (authority == "::1" || authority == "0:0:0:0:0:0:0:1" || authority == "[::1]" || authority == "[0:0:0:0:0:0:0:1]")
    {
        return true;
    }

    return false;
}

bool GeneralHTTPCredentialsProvider::ShouldCreateGeneralHTTPProvider(const Aws::String& relativeUri, const Aws::String& absoluteUri, const Aws::String authToken)
{
    if (authToken.find("\r\n") != Aws::String::npos)
    {
        AWS_LOGSTREAM_WARN(GEN_HTTP_LOG_TAG, "Can't use General HTTP Provider: AWS_CONTAINER_AUTHORIZATION_TOKEN env value contains invalid characters (\\r\\n)");
        return false;
    }

    if (!relativeUri.empty())
    {
        // The provider MAY choose to assert syntactical validity of the resulting URI
        // perform very basic check here
        if (relativeUri[0] != '/') {
          AWS_LOGSTREAM_WARN(GEN_HTTP_LOG_TAG, "Can't use General HTTP Provider: AWS_CONTAINER_CREDENTIALS_RELATIVE_URI does not begin with /");
          return false;
        } else {
          // full URI is not used in case of a relative one present
          return true;
        }
    }

    if (!absoluteUri.empty())
    {
        // If the resolved URI’s scheme is HTTPS, its hostname may be used in the request
        if (Aws::Utils::StringUtils::ToLower(absoluteUri.c_str()).rfind(Aws::String("https://"), 0) == 0) // if starts_with
        {
            return true;
        }

        Aws::Http::URI absUri(absoluteUri);
        const Aws::String& authority = absUri.GetAuthority();

        // Otherwise, implementations MUST fail to resolve when the URI hostname does not satisfy any of the following conditions
        if (IsAllowedIp(authority))
        {
            return true;
        }

        Aws::Crt::Io::HostResolver* pHostResolver = Aws::Crt::ApiHandle::GetOrCreateStaticDefaultHostResolver();
        if (pHostResolver)
        {
          bool shouldAllow = false;
          bool hostResolved = false;
          std::mutex hostResolverMutex;
          std::condition_variable hostResolverCV;
          auto onHostResolved = [&shouldAllow, &hostResolved, &hostResolverCV, &hostResolverMutex](Aws::Crt::Io::HostResolver &resolver, const Aws::Crt::Vector<Aws::Crt::Io::HostAddress> &addresses, int errorCode)
            {
              AWS_UNREFERENCED_PARAM(resolver);
              if (AWS_ERROR_SUCCESS == errorCode)
              {
                for(const auto& address : addresses)
                {
                  if (!IsAllowedIp(Aws::String((const char*) address.address->bytes, address.address->len)))
                  {
                    return;
                  }
                }
                std::unique_lock<std::mutex> lock(hostResolverMutex);
                shouldAllow = !addresses.empty();
                hostResolved = true;
                hostResolverCV.notify_one();
              }
              else
              {
                std::unique_lock<std::mutex> lock(hostResolverMutex);
                hostResolverCV.notify_one();
              }
            };
          pHostResolver->ResolveHost(authority.c_str(), onHostResolved);
          std::unique_lock<std::mutex> lock(hostResolverMutex);
          if (!hostResolved) {
            hostResolverCV.wait_for(lock, std::chrono::milliseconds(1000));
          }
          if (shouldAllow)
          {
            return true;
          }
        }

        AWS_LOGSTREAM_WARN(GEN_HTTP_LOG_TAG, "Can't use General HTTP Provider: AWS_CONTAINER_CREDENTIALS_FULL_URI is not HTTPS and is not within loop back CIDR: " << authority);
        return false;
    }

    // both relativeUri and absoluteUri are empty
    return false;
}

GeneralHTTPCredentialsProvider::GeneralHTTPCredentialsProvider(const Aws::String& relativeUri,
                                                               const Aws::String& absoluteUri,
                                                               const Aws::String& authToken,
                                                               const Aws::String& authTokenFilePath,
                                                               long refreshRateMs,
                                                               ShouldCreateFunc shouldCreateFunc) :
    m_authTokenFilePath(authTokenFilePath),
    m_loadFrequencyMs(refreshRateMs)
{
    if (shouldCreateFunc(relativeUri, absoluteUri, authToken))
    {
        AWS_LOGSTREAM_INFO(GEN_HTTP_LOG_TAG, "Creating GeneralHTTPCredentialsProvider with refresh rate " << refreshRateMs);
        if (!relativeUri.empty()) {
            m_ecsCredentialsClient = Aws::MakeShared<Aws::Internal::ECSCredentialsClient>(GEN_HTTP_LOG_TAG, relativeUri.c_str(), AWS_ECS_CONTAINER_HOST, authToken.c_str());
        } else if (!absoluteUri.empty()) {
            m_ecsCredentialsClient = Aws::MakeShared<Aws::Internal::ECSCredentialsClient>(GEN_HTTP_LOG_TAG, "", absoluteUri.c_str(), authToken.c_str());
        }
    }
}

GeneralHTTPCredentialsProvider::GeneralHTTPCredentialsProvider(
        const std::shared_ptr<Aws::Internal::ECSCredentialsClient>& client, long refreshRateMs) :
    m_ecsCredentialsClient(client),
    m_loadFrequencyMs(refreshRateMs)
{
    AWS_LOGSTREAM_INFO(GEN_HTTP_LOG_TAG, "Creating GeneralHTTPCredentialsProvider with a pre-allocated client " << refreshRateMs);
}

AWSCredentials GeneralHTTPCredentialsProvider::GetAWSCredentials()
{
    RefreshIfExpired();
    Aws::Utils::Threading::ReaderLockGuard guard(m_reloadLock);
    return m_credentials;
}

bool GeneralHTTPCredentialsProvider::ExpiresSoon() const
{
    return ((m_credentials.GetExpiration() - Aws::Utils::DateTime::Now()).count() < AWS_CREDENTIAL_PROVIDER_EXPIRATION_GRACE_PERIOD);
}

Aws::String GeneralHTTPCredentialsProvider::LoadTokenFromFile() const
{
      Aws::IFStream tokenFile(m_authTokenFilePath.c_str());
      if (tokenFile.is_open() && tokenFile.good())
      {
          Aws::StringStream memoryStream;
          std::copy(std::istreambuf_iterator<char>(tokenFile), std::istreambuf_iterator<char>(), std::ostreambuf_iterator<char>(memoryStream));
          Aws::String tokenFileStr = memoryStream.str();
          if (tokenFileStr.find("\r\n") != Aws::String::npos)
          {
              AWS_LOGSTREAM_ERROR(GEN_HTTP_LOG_TAG, "Unable to retrieve credentials: file in AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE contains invalid characters (\\r\\n)");
              return {};
          }
          return tokenFileStr;
      } else {
          AWS_LOGSTREAM_ERROR(GEN_HTTP_LOG_TAG, "Unable to retrieve credentials: failed to open Auth Token file .");
          return {};
      }
}

void GeneralHTTPCredentialsProvider::Reload()
{
    AWS_LOGSTREAM_INFO(GEN_HTTP_LOG_TAG, "Credentials have expired or will expire, attempting to re-pull from ECS IAM Service.");
    if (!m_ecsCredentialsClient)
    {
        AWS_LOGSTREAM_ERROR(GEN_HTTP_LOG_TAG, "Unable to retrieve credentials: ECS Credentials client is not initialized.");
        return;
    }

    if (!m_authTokenFilePath.empty())
    {
        Aws::String token = LoadTokenFromFile();
        m_ecsCredentialsClient->SetToken(std::move(token));
    }

    auto credentialsStr = m_ecsCredentialsClient->GetECSCredentials();
    if (credentialsStr.empty()) return;

    Aws::Utils::Json::JsonValue credentialsDoc(credentialsStr);
    if (!credentialsDoc.WasParseSuccessful())
    {
        AWS_LOGSTREAM_ERROR(GEN_HTTP_LOG_TAG, "Failed to parse output from ECSCredentialService.");
        return;
    }

    Aws::String accessKey, secretKey, token;
    Utils::Json::JsonView credentialsView(credentialsDoc);
    accessKey = credentialsView.GetString("AccessKeyId");
    secretKey = credentialsView.GetString("SecretAccessKey");
    token = credentialsView.GetString("Token");
    AWS_LOGSTREAM_DEBUG(GEN_HTTP_LOG_TAG, "Successfully pulled credentials from metadata service with access key " << accessKey);

    m_credentials.SetAWSAccessKeyId(accessKey);
    m_credentials.SetAWSSecretKey(secretKey);
    m_credentials.SetSessionToken(token);
    m_credentials.SetExpiration(Aws::Utils::DateTime(credentialsView.GetString("Expiration"), Aws::Utils::DateFormat::ISO_8601));
    AWSCredentialsProvider::Reload();
}

void GeneralHTTPCredentialsProvider::RefreshIfExpired()
{
    AWS_LOGSTREAM_DEBUG(GEN_HTTP_LOG_TAG, "Checking if latest credential pull has expired.");
    Aws::Utils::Threading::ReaderLockGuard guard(m_reloadLock);
    if (!m_credentials.IsEmpty() && !IsTimeToRefresh(m_loadFrequencyMs) && !ExpiresSoon())
    {
        return;
    }

    guard.UpgradeToWriterLock();

    if (!m_credentials.IsEmpty() && !IsTimeToRefresh(m_loadFrequencyMs) && !ExpiresSoon())
    {
        return;
    }

    Reload();
}
