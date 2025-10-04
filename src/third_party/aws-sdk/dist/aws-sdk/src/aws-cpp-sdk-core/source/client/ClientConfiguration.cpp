/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/config/defaults/ClientConfigurationDefaults.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/client/DefaultRetryStrategy.h>
#include <aws/core/client/AdaptiveRetryStrategy.h>
#include <aws/core/platform/Environment.h>
#include <aws/core/platform/OSVersionInfo.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/threading/Executor.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/core/Version.h>
#include <aws/core/config/AWSProfileConfigLoader.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <smithy/tracing/NoopTelemetryProvider.h>

#include <aws/crt/Config.h>


namespace Aws
{
namespace Auth
{
    AWS_CORE_API Aws::String GetConfigProfileFilename();
}
namespace Client
{

static const char* CLIENT_CONFIG_TAG = "ClientConfiguration";
static const char* DISABLE_REQUEST_COMPRESSION_ENV_VAR = "DISABLE_REQUEST_COMPRESSION";
static const char* DISABLE_REQUEST_COMPRESSION_CONFIG_VAR = "disable_request_compression";
static const char* REQUEST_MIN_COMPRESSION_SIZE_BYTES_ENV_VAR = "REQUEST_MIN_COMPRESSION_SIZE_BYTES";
static const char* REQUEST_MIN_COMPRESSION_SIZE_BYTES_CONFIG_VAR = "request_min_compression_size_bytes";
static const char* AWS_EXECUTION_ENV = "AWS_EXECUTION_ENV";
static const char* DISABLE_IMDSV1_CONFIG_VAR = "AWS_EC2_METADATA_V1_DISABLED";
static const char* DISABLE_IMDSV1_ENV_VAR = "ec2_metadata_v1_disabled";

ClientConfiguration::ProviderFactories ClientConfiguration::ProviderFactories::defaultFactories = []()
{
    ProviderFactories factories;

    factories.retryStrategyCreateFn = [](){return InitRetryStrategy();};
    factories.executorCreateFn = [](){return Aws::MakeShared<Aws::Utils::Threading::DefaultExecutor>(CLIENT_CONFIG_TAG);};
    factories.writeRateLimiterCreateFn = [](){return nullptr;};
    factories.readRateLimiterCreateFn = [](){return nullptr;};
    factories.telemetryProviderCreateFn = [](){return smithy::components::tracing::NoopTelemetryProvider::CreateProvider();};

    return factories;
}();

Aws::String FilterUserAgentToken(char const * const source)
{
  // Tokens are short textual identifiers that do not include whitespace or delimiters.
  Aws::String copy;
  if(!source)
    return copy;
  size_t sourceLength = std::min(strlen(source), (size_t) 256); // 256 is arbitrary here
  copy.resize(sourceLength);

  // "/" is not listed as a valid char per spec, however, it appears to be not replaced.
  static const char TOKEN_ALLOWED_CHARACTERS[] = R"(!#$%&'*+-.^_`|~ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz1234567890)" R"(/)";
  static const size_t TOKEN_ALLOWED_CHARACTERS_SZ = sizeof(TOKEN_ALLOWED_CHARACTERS) - 1;
  // replace not allowed with '-', except ' ' to be replaced with '_'
  std::transform(source, source + sourceLength, copy.begin(),
                 [](unsigned char c) -> unsigned char
                 {
                    if (c == ' ')
                      return '_';
                    if(std::find(TOKEN_ALLOWED_CHARACTERS,
                                 TOKEN_ALLOWED_CHARACTERS + TOKEN_ALLOWED_CHARACTERS_SZ,
                                 c) == TOKEN_ALLOWED_CHARACTERS + TOKEN_ALLOWED_CHARACTERS_SZ)
                    {
                      return '-';
                    }
                    return c;
                 });

  return copy;
}

Aws::String ComputeUserAgentString(ClientConfiguration const * const pConfig)
{
  if (pConfig && !pConfig->userAgent.empty())
  {
    AWS_LOGSTREAM_INFO(CLIENT_CONFIG_TAG, "User agent is overridden in the config: " << pConfig->userAgent);
    return pConfig->userAgent;
  }
  Aws::StringStream ss;
  ss << "aws-sdk-cpp/" << FilterUserAgentToken(Version::GetVersionString()) << " "
     << "ua/2.0 "
     << "md/aws-crt#" << FilterUserAgentToken(AWS_CRT_CPP_VERSION) << " "
     << "os/" << FilterUserAgentToken(Aws::OSVersionInfo::ComputeOSVersionString().c_str());
  Aws::String arch = Aws::OSVersionInfo::ComputeOSVersionArch();
  if(!arch.empty())
  {
    ss << " md/arch#" << FilterUserAgentToken(arch.c_str());
  }
  ss << " lang/c++#" << FilterUserAgentToken(Version::GetCPPStandard()) << " "
     << "md/" << FilterUserAgentToken(Version::GetCompilerVersionString());
  if (pConfig && pConfig->retryStrategy && pConfig->retryStrategy->GetStrategyName())
  {
    ss << " cfg/retry-mode#" << FilterUserAgentToken(pConfig->retryStrategy->GetStrategyName());
  }

#if defined(AWS_USER_AGENT_CUSTOMIZATION)
#define XSTR(V) STR(V)
#define STR(V) #V
  ss << " " << FilterUserAgentToken(XSTR(AWS_USER_AGENT_CUSTOMIZATION));
#undef STR
#undef XSTR
#endif

  Aws::String awsExecEnv = Aws::Environment::GetEnv(AWS_EXECUTION_ENV);
  if(!awsExecEnv.empty())
  {
    ss << " exec-env/" << FilterUserAgentToken(awsExecEnv.c_str());
  }

  const Aws::String& appId = pConfig ? pConfig->appId :
          ClientConfiguration::LoadConfigFromEnvOrProfile("AWS_SDK_UA_APP_ID", "default", "sdk_ua_app_id", {}, "");
  if(!appId.empty())
  {
    ss << " app/" << appId;
  }

  return ss.str();
}

void setLegacyClientConfigurationParameters(ClientConfiguration& clientConfig)
{
    clientConfig.scheme = Aws::Http::Scheme::HTTPS;
    clientConfig.useDualStack = false;
    clientConfig.useFIPS = false;
    clientConfig.maxConnections = 25;
    clientConfig.httpRequestTimeoutMs = 0;
    clientConfig.requestTimeoutMs = 3000;
    clientConfig.connectTimeoutMs = 1000;
    clientConfig.enableTcpKeepAlive = true;
    clientConfig.tcpKeepAliveIntervalMs = 30000;
    clientConfig.lowSpeedLimit = 1;
    clientConfig.proxyScheme = Aws::Http::Scheme::HTTP;
    clientConfig.proxyPort = 0;
    clientConfig.verifySSL = true;
    clientConfig.writeRateLimiter = nullptr;
    clientConfig.readRateLimiter = nullptr;
    clientConfig.httpLibOverride = Aws::Http::TransferLibType::DEFAULT_CLIENT;
    clientConfig.followRedirects = FollowRedirectsPolicy::DEFAULT;
    clientConfig.disableExpectHeader = false;
    clientConfig.enableClockSkewAdjustment = true;
    clientConfig.enableHostPrefixInjection = true;
    clientConfig.enableHttpClientTrace = false;
    if (clientConfig.profileName.empty())
    {
        clientConfig.profileName = Aws::Auth::GetConfigProfileName();
    }

    Aws::String disableCompressionConfig = clientConfig.LoadConfigFromEnvOrProfile(
        DISABLE_REQUEST_COMPRESSION_ENV_VAR,
        clientConfig.profileName,
        DISABLE_REQUEST_COMPRESSION_CONFIG_VAR,
        {"TRUE", "FALSE", "true", "false"},
        "false"
        );

    if (Aws::Utils::StringUtils::ToLower(disableCompressionConfig.c_str())  == "true") {
      clientConfig.requestCompressionConfig.useRequestCompression = Aws::Client::UseRequestCompression::DISABLE;
      AWS_LOGSTREAM_DEBUG(CLIENT_CONFIG_TAG, "Request Compression disabled");
    } else {
      //Using default to true for forward compatibility in case new config is added but SDK is not updated.
      clientConfig.requestCompressionConfig.useRequestCompression = Aws::Client::UseRequestCompression::ENABLE;
      AWS_LOGSTREAM_DEBUG(CLIENT_CONFIG_TAG, "Request Compression enabled");
    }

    // Getting min request compression length
    Aws::String minRequestCompressionString = Aws::Environment::GetEnv(REQUEST_MIN_COMPRESSION_SIZE_BYTES_ENV_VAR);
    if (minRequestCompressionString.empty())
    {
      minRequestCompressionString = Aws::Config::GetCachedConfigValue(REQUEST_MIN_COMPRESSION_SIZE_BYTES_CONFIG_VAR);
    }
    if (!minRequestCompressionString.empty()) {
      clientConfig.requestCompressionConfig.requestMinCompressionSizeBytes = static_cast<int>(Aws::Utils::StringUtils::ConvertToInt32(minRequestCompressionString.c_str()));
      if (clientConfig.requestCompressionConfig.requestMinCompressionSizeBytes > 10485760) {
        AWS_LOGSTREAM_ERROR(CLIENT_CONFIG_TAG, "ClientConfiguration for MinReqCompression is unsupported, received: " << clientConfig.requestCompressionConfig.requestMinCompressionSizeBytes);
      }
    }
    AWS_LOGSTREAM_DEBUG(CLIENT_CONFIG_TAG, "ClientConfiguration will use MinReqCompression: " << clientConfig.requestCompressionConfig.requestMinCompressionSizeBytes);

    AWS_LOGSTREAM_DEBUG(CLIENT_CONFIG_TAG, "ClientConfiguration will use SDK Auto Resolved profile: [" << clientConfig.profileName << "] if not specified by users.");

    // Automatically determine the AWS region from environment variables, configuration file and EC2 metadata.
    clientConfig.region = Aws::Environment::GetEnv("AWS_DEFAULT_REGION");
    if (!clientConfig.region.empty())
    {
        return;
    }

    clientConfig.region = Aws::Environment::GetEnv("AWS_REGION");
    if (!clientConfig.region.empty())
    {
        return;
    }

    clientConfig.region = Aws::Config::GetCachedConfigValue("region");
    if (!clientConfig.region.empty())
    {
        return;
    }

    // Set the endpoint to interact with EC2 instance's metadata service
    Aws::String ec2MetadataServiceEndpoint = Aws::Environment::GetEnv("AWS_EC2_METADATA_SERVICE_ENDPOINT");
    if (! ec2MetadataServiceEndpoint.empty())
    {
        //By default we use the IPv4 default metadata service address
        auto client = Aws::Internal::GetEC2MetadataClient();
        if (client != nullptr)
        {
            client->SetEndpoint(ec2MetadataServiceEndpoint);
        }
    }

    clientConfig.appId = clientConfig.LoadConfigFromEnvOrProfile(
            "AWS_SDK_UA_APP_ID",
            clientConfig.profileName,
            "sdk_ua_app_id",
            {},
            ""
    );
}

void setConfigFromEnvOrProfile(ClientConfiguration &config)
{
    Aws::String disableIMDSv1 = ClientConfiguration::LoadConfigFromEnvOrProfile(DISABLE_IMDSV1_ENV_VAR,
        config.profileName,
        DISABLE_IMDSV1_CONFIG_VAR,
        {"true", "false"},
        "false");
    if (disableIMDSv1 == "true") {
        config.disableImdsV1 = true;
    }
}

ClientConfiguration::ClientConfiguration()
{
    this->disableIMDS = false;
    setLegacyClientConfigurationParameters(*this);

    if (!this->disableIMDS &&
        region.empty() &&
        Aws::Utils::StringUtils::ToLower(Aws::Environment::GetEnv("AWS_EC2_METADATA_DISABLED").c_str()) != "true")
    {
        auto client = Aws::Internal::GetEC2MetadataClient();
        if (client)
        {
            region = client->GetCurrentRegion();
        }
    }
    if (!region.empty())
    {
        return;
    }
    region = Aws::String(Aws::Region::US_EAST_1);
    setConfigFromEnvOrProfile(*this);
}

ClientConfiguration::ClientConfiguration(const ClientConfigurationInitValues &configuration)
{
    this->disableIMDS = configuration.shouldDisableIMDS;
    setLegacyClientConfigurationParameters(*this);

    if (!this->disableIMDS &&
        region.empty() &&
        Aws::Utils::StringUtils::ToLower(Aws::Environment::GetEnv("AWS_EC2_METADATA_DISABLED").c_str()) != "true")
    {
        auto client = Aws::Internal::GetEC2MetadataClient();
        if (client)
        {
            region = client->GetCurrentRegion();
        }
    }
    if (!region.empty())
    {
        return;
    }
    region = Aws::String(Aws::Region::US_EAST_1);
    setConfigFromEnvOrProfile(*this);
}

ClientConfiguration::ClientConfiguration(const char* profile, bool shouldDisableIMDS)
{
    this->disableIMDS = shouldDisableIMDS;
    if (profile && Aws::Config::HasCachedConfigProfile(profile)) {
        this->profileName = Aws::String(profile);
    }
    setLegacyClientConfigurationParameters(*this);
    // Call EC2 Instance Metadata service only once
    Aws::String ec2MetadataRegion;
    bool hasEc2MetadataRegion = false;
    if (!this->disableIMDS &&
        region.empty() &&
        Aws::Utils::StringUtils::ToLower(Aws::Environment::GetEnv("AWS_EC2_METADATA_DISABLED").c_str()) != "true") {
        auto client = Aws::Internal::GetEC2MetadataClient();
        if (client)
        {
            ec2MetadataRegion = client->GetCurrentRegion();
            hasEc2MetadataRegion = true;
            region = ec2MetadataRegion;
        }
    }

    if(region.empty())
    {
        region = Aws::String(Aws::Region::US_EAST_1);
    }

    if (profile && Aws::Config::HasCachedConfigProfile(profile)) {
        AWS_LOGSTREAM_DEBUG(CLIENT_CONFIG_TAG,
                            "Use user specified profile: [" << this->profileName << "] for ClientConfiguration.");
        auto tmpRegion = Aws::Config::GetCachedConfigProfile(this->profileName).GetRegion();
        if (!tmpRegion.empty()) {
            region = tmpRegion;
        }

        Aws::String profileDefaultsMode = Aws::Config::GetCachedConfigProfile(this->profileName).GetDefaultsMode();
        Aws::Config::Defaults::SetSmartDefaultsConfigurationParameters(*this, profileDefaultsMode,
                                                                       hasEc2MetadataRegion, ec2MetadataRegion);
        return;
    }

    AWS_LOGSTREAM_WARN(CLIENT_CONFIG_TAG, "User specified profile: [" << profile << "] is not found, will use the SDK resolved one.");
    setConfigFromEnvOrProfile(*this);
}

ClientConfiguration::ClientConfiguration(bool /*useSmartDefaults*/, const char* defaultMode, bool shouldDisableIMDS)
{
    this->disableIMDS = shouldDisableIMDS;
    setLegacyClientConfigurationParameters(*this);

    // Call EC2 Instance Metadata service only once
    Aws::String ec2MetadataRegion;
    bool hasEc2MetadataRegion = false;
    if (!this->disableIMDS &&
        region.empty() &&
        Aws::Utils::StringUtils::ToLower(Aws::Environment::GetEnv("AWS_EC2_METADATA_DISABLED").c_str()) != "true")
    {
        auto client = Aws::Internal::GetEC2MetadataClient();
        if (client)
        {
            ec2MetadataRegion = client->GetCurrentRegion();
            hasEc2MetadataRegion = true;
            region = ec2MetadataRegion;
        }
    }
    if (region.empty())
    {
        region = Aws::String(Aws::Region::US_EAST_1);
    }

    Aws::Config::Defaults::SetSmartDefaultsConfigurationParameters(*this, defaultMode, hasEc2MetadataRegion, ec2MetadataRegion);
    setConfigFromEnvOrProfile(*this);
}

std::shared_ptr<RetryStrategy> InitRetryStrategy(Aws::String retryMode)
{
    int maxAttempts = 0;
    Aws::String maxAttemptsString = Aws::Environment::GetEnv("AWS_MAX_ATTEMPTS");
    if (maxAttemptsString.empty())
    {
        maxAttemptsString = Aws::Config::GetCachedConfigValue("max_attempts");
    }
    // In case users specify 0 explicitly to disable retry.
    if (maxAttemptsString == "0")
    {
        maxAttempts = 0;
    }
    else
    {
        maxAttempts = static_cast<int>(Aws::Utils::StringUtils::ConvertToInt32(maxAttemptsString.c_str()));
        if (maxAttempts == 0)
        {
            AWS_LOGSTREAM_INFO(CLIENT_CONFIG_TAG, "Retry Strategy will use the default max attempts.");
            maxAttempts = -1;
        }
    }

    if (retryMode.empty())
    {
        retryMode = Aws::Environment::GetEnv("AWS_RETRY_MODE");
    }
    if (retryMode.empty())
    {
        retryMode = Aws::Config::GetCachedConfigValue("retry_mode");
    }

    std::shared_ptr<RetryStrategy> retryStrategy;
    if (retryMode == "standard")
    {
        if (maxAttempts < 0)
        {
            // negative value set above force usage of default max attempts
            retryStrategy = Aws::MakeShared<StandardRetryStrategy>(CLIENT_CONFIG_TAG);
        }
        else
        {
            retryStrategy = Aws::MakeShared<StandardRetryStrategy>(CLIENT_CONFIG_TAG, maxAttempts);
        }
    }
    else if (retryMode == "adaptive")
    {
        if (maxAttempts < 0)
        {
            // negative value set above force usage of default max attempts
            retryStrategy = Aws::MakeShared<AdaptiveRetryStrategy>(CLIENT_CONFIG_TAG);
        }
        else
        {
            retryStrategy = Aws::MakeShared<AdaptiveRetryStrategy>(CLIENT_CONFIG_TAG, maxAttempts);
        }
    }
    else
    {
        retryStrategy = Aws::MakeShared<DefaultRetryStrategy>(CLIENT_CONFIG_TAG);
    }

    return retryStrategy;
}

Aws::String ClientConfiguration::LoadConfigFromEnvOrProfile(const Aws::String& envKey,
                                                            const Aws::String& profile,
                                                            const Aws::String& profileProperty,
                                                            const Aws::Vector<Aws::String>& allowedValues,
                                                            const Aws::String& defaultValue)
{
    Aws::String option = Aws::Environment::GetEnv(envKey.c_str());
    if (option.empty()) {
        option = Aws::Config::GetCachedConfigValue(profile, profileProperty);
    }
    option = Aws::Utils::StringUtils::ToLower(option.c_str());
    if (option.empty()) {
        return defaultValue;
    }

    if (!allowedValues.empty() && std::find(allowedValues.cbegin(), allowedValues.cend(), option) == allowedValues.cend()) {
        Aws::OStringStream expectedStr;
        expectedStr << "[";
        for(const auto& allowed : allowedValues) {
            expectedStr << allowed << ";";
        }
        expectedStr << "]";

        AWS_LOGSTREAM_WARN(CLIENT_CONFIG_TAG, "Unrecognised value for " << envKey << ": " << option <<
                                              ". Using default instead: " << defaultValue <<
                                              ". Expected empty or one of: " << expectedStr.str());
        option = defaultValue;
    }
    return option;
}

} // namespace Client
} // namespace Aws
