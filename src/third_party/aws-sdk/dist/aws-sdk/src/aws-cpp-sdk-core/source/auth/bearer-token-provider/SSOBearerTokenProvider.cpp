/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#include <aws/core/auth/bearer-token-provider/SSOBearerTokenProvider.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/config/AWSProfileConfigLoader.h>
#include <aws/core/internal/AWSHttpResourceClient.h>
#include <aws/core/platform/Environment.h>
#include <aws/core/platform/FileSystem.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/FileSystemUtils.h>
#include <aws/core/client/SpecifiedRetryableErrorsRetryStrategy.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/json/JsonSerializer.h>

using namespace Aws::Auth;

using Aws::Utils::Threading::ReaderLockGuard;


static const char SSO_BEARER_TOKEN_PROVIDER_LOG_TAG[] = "SSOBearerTokenProvider";
static const char SSO_GRANT_TYPE[] = "refresh_token";

const size_t SSOBearerTokenProvider::REFRESH_WINDOW_BEFORE_EXPIRATION_S = 600;
const size_t SSOBearerTokenProvider::REFRESH_ATTEMPT_INTERVAL_S = 30;

SSOBearerTokenProvider::SSOBearerTokenProvider() : SSOBearerTokenProvider(Aws::Auth::GetConfigProfileName(), nullptr)
{
}

SSOBearerTokenProvider::SSOBearerTokenProvider(const Aws::String& awsProfile) : SSOBearerTokenProvider(awsProfile, nullptr)
{
}

SSOBearerTokenProvider::SSOBearerTokenProvider(const Aws::String& awsProfile, std::shared_ptr<const Aws::Client::ClientConfiguration> config)
    : m_profileToUse(awsProfile),
    m_config(config ? std::move(config) : Aws::MakeShared<Client::ClientConfiguration>(SSO_BEARER_TOKEN_PROVIDER_LOG_TAG)),
    m_lastUpdateAttempt((int64_t)0)
{
    AWS_LOGSTREAM_INFO(SSO_BEARER_TOKEN_PROVIDER_LOG_TAG, "Setting sso bearerToken provider to read config from " << m_profileToUse);
}

AWSBearerToken SSOBearerTokenProvider::GetAWSBearerToken()
{
    Aws::Utils::Threading::ReaderLockGuard guard(m_reloadLock);
    if(m_token.IsEmpty())
    {
        Reload();
    }
    if(!m_token.IsEmpty())
    {
        const Aws::Utils::DateTime now = Aws::Utils::DateTime::Now();
        if (now >= m_token.GetExpiration() - std::chrono::seconds(REFRESH_WINDOW_BEFORE_EXPIRATION_S) &&
            m_lastUpdateAttempt + std::chrono::seconds(REFRESH_ATTEMPT_INTERVAL_S) < now)
        {
            guard.UpgradeToWriterLock();
            RefreshFromSso();
        }
    }

    if(m_token.IsExpiredOrEmpty())
    {
        /* If a loaded token has expired and has insufficient metadata to perform a refresh the SSO token
         provider must raise an exception that the token has expired and cannot be refreshed.
         Error logging and returning an empty object instead because of disabled exceptions and poor legacy API design. */
        AWS_LOGSTREAM_ERROR(SSO_BEARER_TOKEN_PROVIDER_LOG_TAG, "SSOBearerTokenProvider is unable to provide a token");
        return Aws::Auth::AWSBearerToken("", Aws::Utils::DateTime(0.0));
    }
    return m_token;
}

void SSOBearerTokenProvider::Reload()
{
    CachedSsoToken cachedSsoToken = LoadAccessTokenFile();
    if(cachedSsoToken.accessToken.empty()) {
        AWS_LOGSTREAM_TRACE(SSO_BEARER_TOKEN_PROVIDER_LOG_TAG, "Access token for SSO not available");
        return;
    }
    m_token.SetToken(cachedSsoToken.accessToken);
    m_token.SetExpiration(cachedSsoToken.expiresAt);

    const Aws::Utils::DateTime now = Aws::Utils::DateTime::Now();
    if(cachedSsoToken.expiresAt < now) {
        AWS_LOGSTREAM_ERROR(SSO_BEARER_TOKEN_PROVIDER_LOG_TAG, "Cached Token is already expired at " << cachedSsoToken.expiresAt.ToGmtString(Aws::Utils::DateFormat::ISO_8601));
        return;
    }
}

void SSOBearerTokenProvider::RefreshFromSso()
{
    CachedSsoToken cachedSsoToken = LoadAccessTokenFile();

    if(!m_client)
    {
        auto scheme = Aws::Http::Scheme::HTTPS;
        /* The SSO token provider must not resolve if any SSO configuration values are present directly on the profile
         * instead of an `sso-session` section. The SSO token provider must ignore these configuration values if these
         * values are present directly on the profile instead of an `sso-session` section. */
        // auto& region = m_profile.GetSsoRegion(); // <- intentionally not used per comment above
        auto& region = cachedSsoToken.region;
        // m_config->region might not be the same as the SSO region, but the former is not used by the SSO client.
        m_client = Aws::MakeUnique<Aws::Internal::SSOCredentialsClient>(SSO_BEARER_TOKEN_PROVIDER_LOG_TAG, *m_config, scheme, region);
    }

    Aws::Internal::SSOCredentialsClient::SSOCreateTokenRequest ssoCreateTokenRequest;
    ssoCreateTokenRequest.clientId = cachedSsoToken.clientId;
    ssoCreateTokenRequest.clientSecret = cachedSsoToken.clientSecret;
    ssoCreateTokenRequest.grantType = SSO_GRANT_TYPE;
    ssoCreateTokenRequest.refreshToken = cachedSsoToken.refreshToken;

    if(!m_client) {
        AWS_LOGSTREAM_FATAL(SSO_BEARER_TOKEN_PROVIDER_LOG_TAG, "Unexpected nullptr in SSOBearerTokenProvider::m_client");
        return;
    }
    Aws::Internal::SSOCredentialsClient::SSOCreateTokenResult result = m_client->CreateToken(ssoCreateTokenRequest);
    if(!result.accessToken.empty())
    {
        cachedSsoToken.accessToken = result.accessToken;
        cachedSsoToken.expiresAt = Aws::Utils::DateTime::Now() + std::chrono::seconds(result.expiresIn);
        if(!result.refreshToken.empty()) {
            cachedSsoToken.refreshToken = result.refreshToken;
        }
        if(!result.clientId.empty()) {
            cachedSsoToken.clientId = result.clientId;
        }
    }

    if(WriteAccessTokenFile(cachedSsoToken))
    {
        m_token.SetToken(cachedSsoToken.accessToken);
        m_token.SetExpiration(cachedSsoToken.expiresAt);
    }

}

SSOBearerTokenProvider::CachedSsoToken SSOBearerTokenProvider::LoadAccessTokenFile() const
{
    SSOBearerTokenProvider::CachedSsoToken retValue;

    const Aws::Config::Profile& profile = Aws::Config::GetCachedConfigProfile(m_profileToUse);
    if(!profile.IsSsoSessionSet()) {
        AWS_LOGSTREAM_ERROR(SSO_BEARER_TOKEN_PROVIDER_LOG_TAG, "SSOBearerTokenProvider set to use a profile " << m_profileToUse << " without a sso_session. Unable to load cached token.");
        return retValue;
    }

    Aws::String hashedStartUrl = Aws::Utils::HashingUtils::HexEncode(Aws::Utils::HashingUtils::CalculateSHA1(profile.GetSsoSession().GetName()));
    Aws::String profileDirectory = ProfileConfigFileAWSCredentialsProvider::GetProfileDirectory();
    Aws::StringStream ssToken;
    ssToken << profileDirectory;
    ssToken << Aws::FileSystem::PATH_DELIM << "sso"  << Aws::FileSystem::PATH_DELIM << "cache" << Aws::FileSystem::PATH_DELIM << hashedStartUrl << ".json";
    auto ssoAccessTokenPath = ssToken.str();
    AWS_LOGSTREAM_DEBUG(SSO_BEARER_TOKEN_PROVIDER_LOG_TAG, "Preparing to load token from: " << ssoAccessTokenPath);

    Aws::IFStream inputFile(ssoAccessTokenPath.c_str());
    if(inputFile)
    {
        AWS_LOGSTREAM_DEBUG(SSO_BEARER_TOKEN_PROVIDER_LOG_TAG, "Reading content from token file: " << ssoAccessTokenPath);

        Aws::Utils::Json::JsonValue tokenDoc(inputFile);
        if (!tokenDoc.WasParseSuccessful())
        {
            AWS_LOGSTREAM_ERROR(SSO_BEARER_TOKEN_PROVIDER_LOG_TAG, "Failed to parse token file: " << ssoAccessTokenPath);
            return retValue;
        }
        Utils::Json::JsonView tokenView(tokenDoc);

        retValue.accessToken = tokenView.GetString("accessToken");
        retValue.expiresAt = Aws::Utils::DateTime(tokenView.GetString("expiresAt"), Aws::Utils::DateFormat::ISO_8601);
        retValue.refreshToken = tokenView.GetString("refreshToken");
        retValue.clientId = tokenView.GetString("clientId");
        retValue.clientSecret = tokenView.GetString("clientSecret");
        retValue.registrationExpiresAt = Aws::Utils::DateTime(tokenView.GetString("registrationExpiresAt"), Aws::Utils::DateFormat::ISO_8601);
        retValue.region = tokenView.GetString("region");
        retValue.startUrl = tokenView.GetString("startUrl");

        return retValue;
    }
    else
    {
        AWS_LOGSTREAM_INFO(SSO_BEARER_TOKEN_PROVIDER_LOG_TAG, "Unable to open token file on path: " << ssoAccessTokenPath);
        return retValue;
    }
}

bool SSOBearerTokenProvider::WriteAccessTokenFile(const CachedSsoToken& token) const
{
    const Aws::Config::Profile& profile = Aws::Config::GetCachedConfigProfile(m_profileToUse);
    if(!profile.IsSsoSessionSet()) {
        AWS_LOGSTREAM_ERROR(SSO_BEARER_TOKEN_PROVIDER_LOG_TAG, "SSOBearerTokenProvider set to use a profile "
                        << m_profileToUse << " without a sso_session. Unable to write a cached token.");
        return false;
    }

    Aws::String hashedStartUrl = Aws::Utils::HashingUtils::HexEncode(Aws::Utils::HashingUtils::CalculateSHA1(profile.GetSsoSession().GetName()));
    Aws::String profileDirectory = ProfileConfigFileAWSCredentialsProvider::GetProfileDirectory();
    Aws::StringStream ssToken;
    ssToken << profileDirectory;
    ssToken << Aws::FileSystem::PATH_DELIM << "sso"  << Aws::FileSystem::PATH_DELIM << "cache" << Aws::FileSystem::PATH_DELIM << hashedStartUrl << ".json";
    auto ssoAccessTokenPath = ssToken.str();
    AWS_LOGSTREAM_DEBUG(SSO_BEARER_TOKEN_PROVIDER_LOG_TAG, "Preparing to write token to: " << ssoAccessTokenPath);

    Aws::OFStream outputFileStream(ssoAccessTokenPath.c_str(), std::ios_base::out | std::ios_base::trunc);
    if(outputFileStream && outputFileStream.good())
    {
        AWS_LOGSTREAM_DEBUG(SSO_BEARER_TOKEN_PROVIDER_LOG_TAG, "Writing content to token file: " << ssoAccessTokenPath);

        Aws::Utils::Json::JsonValue cachedTokenDoc;
        if(!token.accessToken.empty()) {
            cachedTokenDoc.WithString("accessToken", token.accessToken);
        }
        if(token.expiresAt != 0.0) {
            cachedTokenDoc.WithString("expiresAt", token.expiresAt.ToGmtString(Aws::Utils::DateFormat::ISO_8601));
        }
        if(!token.refreshToken.empty()) {
            cachedTokenDoc.WithString("refreshToken", token.refreshToken);
        }
        if(!token.clientId.empty()) {
            cachedTokenDoc.WithString("clientId", token.clientId);
        }
        if(!token.clientSecret.empty()) {
            cachedTokenDoc.WithString("clientSecret", token.clientSecret);
        }
        if(token.registrationExpiresAt != 0.0) {
            cachedTokenDoc.WithString("registrationExpiresAt", token.registrationExpiresAt.ToGmtString(Aws::Utils::DateFormat::ISO_8601));
        }
        if(!token.region.empty()) {
            cachedTokenDoc.WithString("region", token.region);
        }
        if(!token.startUrl.empty()) {
            cachedTokenDoc.WithString("startUrl", token.startUrl);
        }

        const Aws::String& resultingJsonStr = cachedTokenDoc.View().WriteReadable();;
        outputFileStream << resultingJsonStr;

        return outputFileStream.good();
    }
    else
    {
        AWS_LOGSTREAM_INFO(SSO_BEARER_TOKEN_PROVIDER_LOG_TAG, "Unable to open token file on path for writing: " << ssoAccessTokenPath);
        return false;
    }
}
