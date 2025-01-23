/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#include <aws/core/auth/SSOCredentialsProvider.h>
#include <aws/core/config/AWSProfileConfigLoader.h>
#include <aws/core/internal/AWSHttpResourceClient.h>
#include <aws/core/platform/Environment.h>
#include <aws/core/platform/FileSystem.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/FileSystemUtils.h>
#include <aws/core/client/SpecifiedRetryableErrorsRetryStrategy.h>
#include <aws/core/utils/UUID.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/json/JsonSerializer.h>

using namespace Aws::Utils;
using namespace Aws::Utils::Logging;
using namespace Aws::Auth;
using namespace Aws::Internal;
using namespace Aws::FileSystem;
using namespace Aws::Client;
using Aws::Utils::Threading::ReaderLockGuard;


static const char SSO_CREDENTIALS_PROVIDER_LOG_TAG[] = "SSOCredentialsProvider";

SSOCredentialsProvider::SSOCredentialsProvider() : SSOCredentialsProvider(GetConfigProfileName(), nullptr)
{
}

SSOCredentialsProvider::SSOCredentialsProvider(const Aws::String& profile) : SSOCredentialsProvider(profile, nullptr)
{
}

SSOCredentialsProvider::SSOCredentialsProvider(const Aws::String& profile, std::shared_ptr<const Aws::Client::ClientConfiguration> config) :
    m_profileToUse(profile),
    m_bearerTokenProvider(profile),
    m_config(std::move(config))
{
    AWS_LOGSTREAM_INFO(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Setting sso credentials provider to read config from " << m_profileToUse);
    if (!m_config)
    {
        auto defaultConfig = Aws::MakeShared<Client::ClientConfiguration>(SSO_CREDENTIALS_PROVIDER_LOG_TAG);
        defaultConfig->scheme = Aws::Http::Scheme::HTTPS;
        // We cannot set region to m_ssoRegion because it is not yet known at this point. But it's not obtained from the client config either way.
        Aws::Vector<Aws::String> retryableErrors{ "TooManyRequestsException" };
        defaultConfig->retryStrategy = Aws::MakeShared<SpecifiedRetryableErrorsRetryStrategy>(SSO_CREDENTIALS_PROVIDER_LOG_TAG, std::move(retryableErrors), 3/*maxRetries*/);
        m_config = std::move(defaultConfig);
    }
}

AWSCredentials SSOCredentialsProvider::GetAWSCredentials()
{
    RefreshIfExpired();
    ReaderLockGuard guard(m_reloadLock);
    return m_credentials;
}

void SSOCredentialsProvider::Reload()
{
    auto profile = Aws::Config::GetCachedConfigProfile(m_profileToUse);
    const auto accessToken = [&]() -> Aws::String {
        // If we have an SSO Session set, use the refreshed token.
        if (profile.IsSsoSessionSet()) {
            m_ssoRegion = profile.GetSsoSession().GetSsoRegion();
            auto token = m_bearerTokenProvider.GetAWSBearerToken();
            m_expiresAt = token.GetExpiration();
            return token.GetToken();
        }
        Aws::String hashedStartUrl = Aws::Utils::HashingUtils::HexEncode(Aws::Utils::HashingUtils::CalculateSHA1(profile.GetSsoStartUrl()));
        auto profileDirectory = ProfileConfigFileAWSCredentialsProvider::GetProfileDirectory();
        Aws::StringStream ssToken;
        ssToken << profileDirectory;
        ssToken << PATH_DELIM << "sso"  << PATH_DELIM << "cache" << PATH_DELIM << hashedStartUrl << ".json";
        auto ssoTokenPath = ssToken.str();
        AWS_LOGSTREAM_DEBUG(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Loading token from: " << ssoTokenPath)
        m_ssoRegion = profile.GetSsoRegion();
        return LoadAccessTokenFile(ssoTokenPath);
    }();
    if (accessToken.empty()) {
        AWS_LOGSTREAM_TRACE(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Access token for SSO not available");
        return;
    }
    if (m_expiresAt < Aws::Utils::DateTime::Now()) {
        AWS_LOGSTREAM_ERROR(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Cached Token expired at " << m_expiresAt.ToGmtString(DateFormat::ISO_8601));
        return;
    }
    SSOCredentialsClient::SSOGetRoleCredentialsRequest request;
    request.m_ssoAccountId = profile.GetSsoAccountId();
    request.m_ssoRoleName = profile.GetSsoRoleName();
    request.m_accessToken = accessToken;

    m_client = Aws::MakeUnique<Aws::Internal::SSOCredentialsClient>(SSO_CREDENTIALS_PROVIDER_LOG_TAG, *m_config, Aws::Http::Scheme::HTTPS, m_ssoRegion);

    AWS_LOGSTREAM_TRACE(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Requesting credentials with AWS_ACCESS_KEY: " << m_ssoAccountId);
    auto result = m_client->GetSSOCredentials(request);
    AWS_LOGSTREAM_TRACE(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Successfully retrieved credentials with AWS_ACCESS_KEY: " << result.creds.GetAWSAccessKeyId());

    m_credentials = result.creds;
}

void SSOCredentialsProvider::RefreshIfExpired()
{
    ReaderLockGuard guard(m_reloadLock);
    if (!m_credentials.IsExpiredOrEmpty())
    {
        return;
    }

    guard.UpgradeToWriterLock();
    if (!m_credentials.IsExpiredOrEmpty()) // double-checked lock to avoid refreshing twice
    {
        return;
    }

    Reload();
}

Aws::String SSOCredentialsProvider::LoadAccessTokenFile(const Aws::String& ssoAccessTokenPath)
{
    AWS_LOGSTREAM_DEBUG(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Preparing to load token from: " << ssoAccessTokenPath);

    Aws::IFStream inputFile(ssoAccessTokenPath.c_str());
    if(inputFile)
    {
        AWS_LOGSTREAM_DEBUG(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Reading content from token file: " << ssoAccessTokenPath);

        Json::JsonValue tokenDoc(inputFile);
        if (!tokenDoc.WasParseSuccessful())
        {
            AWS_LOGSTREAM_ERROR(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Failed to parse token file: " << ssoAccessTokenPath);
            return "";
        }
        Utils::Json::JsonView tokenView(tokenDoc);
        Aws::String tmpAccessToken, expirationStr;
        tmpAccessToken = tokenView.GetString("accessToken");
        expirationStr = tokenView.GetString("expiresAt");
        DateTime expiration(expirationStr, DateFormat::ISO_8601);

        AWS_LOGSTREAM_TRACE(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Token cache file contains accessToken [" << tmpAccessToken << "], expiration [" << expirationStr << "]");

        if (tmpAccessToken.empty() || !expiration.WasParseSuccessful()) {
            AWS_LOG_ERROR(SSO_CREDENTIALS_PROVIDER_LOG_TAG, R"(The SSO session associated with this profile has expired or is otherwise invalid. To refresh this SSO session run aws sso login with the corresponding profile.)");
            AWS_LOGSTREAM_TRACE(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Token cache file failed because "
                                 << (tmpAccessToken.empty()?"AccessToken was empty ":"")
                                 << (!expiration.WasParseSuccessful()? "failed to parse expiration":""));
            return "";
        }
        m_expiresAt = expiration;
        return  tmpAccessToken;
    }
    else
    {
        AWS_LOGSTREAM_INFO(SSO_CREDENTIALS_PROVIDER_LOG_TAG,"Unable to open token file on path: " << ssoAccessTokenPath);
        return "";
    }
}