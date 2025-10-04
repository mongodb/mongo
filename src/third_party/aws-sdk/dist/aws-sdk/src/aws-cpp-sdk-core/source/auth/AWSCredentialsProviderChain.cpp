/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/auth/STSCredentialsProvider.h>
#include <aws/core/auth/SSOCredentialsProvider.h>
#include <aws/core/platform/Environment.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/logging/LogMacros.h>

using namespace Aws::Auth;
using namespace Aws::Utils::Threading;

static const char AWS_EC2_METADATA_DISABLED[] = "AWS_EC2_METADATA_DISABLED";
static const char DefaultCredentialsProviderChainTag[] = "DefaultAWSCredentialsProviderChain";

AWSCredentials AWSCredentialsProviderChain::GetAWSCredentials()
{
    ReaderLockGuard lock(m_cachedProviderLock);
    if (m_cachedProvider) {
      AWSCredentials credentials = m_cachedProvider->GetAWSCredentials();
      if (!credentials.GetAWSAccessKeyId().empty() && !credentials.GetAWSSecretKey().empty())
      {
        return credentials;
      }
    }
    lock.UpgradeToWriterLock();
    for (auto&& credentialsProvider : m_providerChain)
    {
        AWSCredentials credentials = credentialsProvider->GetAWSCredentials();
        if (!credentials.GetAWSAccessKeyId().empty() && !credentials.GetAWSSecretKey().empty())
        {
            m_cachedProvider = credentialsProvider;
            return credentials;
        }
    }
    return AWSCredentials();
}

DefaultAWSCredentialsProviderChain::DefaultAWSCredentialsProviderChain() : AWSCredentialsProviderChain()
{
    AddProvider(Aws::MakeShared<EnvironmentAWSCredentialsProvider>(DefaultCredentialsProviderChainTag));
    AddProvider(Aws::MakeShared<ProfileConfigFileAWSCredentialsProvider>(DefaultCredentialsProviderChainTag));
    AddProvider(Aws::MakeShared<ProcessCredentialsProvider>(DefaultCredentialsProviderChainTag));
    AddProvider(Aws::MakeShared<STSAssumeRoleWebIdentityCredentialsProvider>(DefaultCredentialsProviderChainTag));
    AddProvider(Aws::MakeShared<SSOCredentialsProvider>(DefaultCredentialsProviderChainTag));
    
    // General HTTP Credentials (prev. known as ECS TaskRole credentials) only available when ENVIRONMENT VARIABLE is set
    const auto relativeUri = Aws::Environment::GetEnv(GeneralHTTPCredentialsProvider::AWS_CONTAINER_CREDENTIALS_RELATIVE_URI);
    AWS_LOGSTREAM_DEBUG(DefaultCredentialsProviderChainTag, "The environment variable value " << GeneralHTTPCredentialsProvider::AWS_CONTAINER_CREDENTIALS_RELATIVE_URI
            << " is " << relativeUri);

    const auto absoluteUri = Aws::Environment::GetEnv(GeneralHTTPCredentialsProvider::AWS_CONTAINER_CREDENTIALS_FULL_URI);
    AWS_LOGSTREAM_DEBUG(DefaultCredentialsProviderChainTag, "The environment variable value " << GeneralHTTPCredentialsProvider::AWS_CONTAINER_CREDENTIALS_FULL_URI
            << " is " << absoluteUri);

    const auto ec2MetadataDisabled = Aws::Environment::GetEnv(AWS_EC2_METADATA_DISABLED);
    AWS_LOGSTREAM_DEBUG(DefaultCredentialsProviderChainTag, "The environment variable value " << AWS_EC2_METADATA_DISABLED
            << " is " << ec2MetadataDisabled);

    if (!relativeUri.empty() || !absoluteUri.empty())
    {
        const Aws::String token = Aws::Environment::GetEnv(GeneralHTTPCredentialsProvider::AWS_CONTAINER_AUTHORIZATION_TOKEN);
        const Aws::String tokenPath = Aws::Environment::GetEnv(GeneralHTTPCredentialsProvider::AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE);

        auto genProvider = Aws::MakeShared<GeneralHTTPCredentialsProvider>(DefaultCredentialsProviderChainTag,
                                                                           relativeUri, absoluteUri, token, tokenPath);
        if (genProvider && genProvider->IsValid()) {
            AddProvider(std::move(genProvider));
            auto& uri = !relativeUri.empty() ? relativeUri : absoluteUri;
            AWS_LOGSTREAM_INFO(DefaultCredentialsProviderChainTag, "Added General HTTP / ECS credentials provider with ur: ["
                << uri << "] to the provider chain with a" << ((token.empty() && tokenPath.empty()) ? "n empty " : " non-empty ")
                << "authorization token.");
        } else {
            AWS_LOGSTREAM_ERROR(DefaultCredentialsProviderChainTag, "Unable to create GeneralHTTPCredentialsProvider");
        }
    }
    else if (Aws::Utils::StringUtils::ToLower(ec2MetadataDisabled.c_str()) != "true")
    {
        AddProvider(Aws::MakeShared<InstanceProfileCredentialsProvider>(DefaultCredentialsProviderChainTag));
        AWS_LOGSTREAM_INFO(DefaultCredentialsProviderChainTag, "Added EC2 metadata service credentials provider to the provider chain.");
    }
}

DefaultAWSCredentialsProviderChain::DefaultAWSCredentialsProviderChain(const DefaultAWSCredentialsProviderChain& chain) {
    for (const auto& provider: chain.GetProviders()) {
        AddProvider(provider);
    }
}
