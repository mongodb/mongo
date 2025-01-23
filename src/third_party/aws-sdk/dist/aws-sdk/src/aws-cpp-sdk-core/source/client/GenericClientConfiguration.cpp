/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/client/GenericClientConfiguration.h>
#include <aws/core/platform/Environment.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/threading/Executor.h>


namespace Aws
{
namespace Client
{
struct AWS_CORE_API GenericClientConfiguration;

bool IsEndpointDiscoveryEnabled(const Aws::String& endpointOverride, const Aws::String &profileName, const bool defaultValue)
{
  bool enabled = defaultValue;  // default value for AWS Services with enabled discovery trait
  if (!endpointOverride.empty())
  {
    enabled = false;
  }
  else
  {
    static const char* AWS_ENABLE_ENDPOINT_DISCOVERY_ENV_KEY = "AWS_ENABLE_ENDPOINT_DISCOVERY";
    static const char* AWS_ENABLE_ENDPOINT_DISCOVERY_PROFILE_KEY = "AWS_ENABLE_ENDPOINT_DISCOVERY";
    static const char* AWS_ENABLE_ENDPOINT_ENABLED = "true";
    static const char* AWS_ENABLE_ENDPOINT_DISABLED = "false";

    Aws::String enableEndpointDiscovery = ClientConfiguration::LoadConfigFromEnvOrProfile(AWS_ENABLE_ENDPOINT_DISCOVERY_ENV_KEY,
                                                                                          profileName,
                                                                                          AWS_ENABLE_ENDPOINT_DISCOVERY_PROFILE_KEY,
                                                                                          {AWS_ENABLE_ENDPOINT_ENABLED, AWS_ENABLE_ENDPOINT_DISABLED},
                                                                                          AWS_ENABLE_ENDPOINT_ENABLED);

    if (AWS_ENABLE_ENDPOINT_DISABLED == enableEndpointDiscovery)
    {
      // enabled by default unless explicitly disabled in ENV, profile config file, or programmatically later
      enabled = false;
    }
  }
  return enabled;
}

#if 0
GenericClientConfiguration<true>::GenericClientConfiguration(const ClientConfigurationInitValues &configuration)
    : ClientConfiguration(configuration),
      enableHostPrefixInjection(ClientConfiguration::enableHostPrefixInjection),
      enableEndpointDiscovery(ClientConfiguration::enableEndpointDiscovery)
{
    enableEndpointDiscovery = IsEndpointDiscoveryEnabled(this->endpointOverride, this->profileName, EndpointDiscoveryDefaultValue);
    enableHostPrefixInjection = false; // disabled by default in the SDK
}

GenericClientConfiguration<true>::GenericClientConfiguration(const char* inputProfileName, bool shouldDisableIMDS)
    : ClientConfiguration(inputProfileName, shouldDisableIMDS),
      enableHostPrefixInjection(ClientConfiguration::enableHostPrefixInjection),
      enableEndpointDiscovery(ClientConfiguration::enableEndpointDiscovery)
{
    enableEndpointDiscovery = IsEndpointDiscoveryEnabled(this->endpointOverride, this->profileName);
    enableHostPrefixInjection = false; // disabled by default in the SDK
}

GenericClientConfiguration<true>::GenericClientConfiguration(bool useSmartDefaults, const char* inputDefaultMode, bool shouldDisableIMDS)
    : ClientConfiguration(useSmartDefaults, inputDefaultMode, shouldDisableIMDS),
      enableHostPrefixInjection(ClientConfiguration::enableHostPrefixInjection),
      enableEndpointDiscovery(ClientConfiguration::enableEndpointDiscovery)
{
    enableEndpointDiscovery = IsEndpointDiscoveryEnabled(this->endpointOverride, this->profileName);
    enableHostPrefixInjection = false; // disabled by default in the SDK
}

GenericClientConfiguration<true>::GenericClientConfiguration(const ClientConfiguration& config)
    : ClientConfiguration(config),
      enableHostPrefixInjection(ClientConfiguration::enableHostPrefixInjection),
      enableEndpointDiscovery(ClientConfiguration::enableEndpointDiscovery)
{
    enableEndpointDiscovery = IsEndpointDiscoveryEnabled(this->endpointOverride, this->profileName);
    enableHostPrefixInjection = false; // disabled by default in the SDK
}

GenericClientConfiguration<true>::GenericClientConfiguration(const GenericClientConfiguration<true>& other)
    : ClientConfiguration(static_cast<ClientConfiguration>(other)),
      enableHostPrefixInjection(ClientConfiguration::enableHostPrefixInjection),
      enableEndpointDiscovery(ClientConfiguration::enableEndpointDiscovery)
{
    if (other.enableEndpointDiscovery) {
        enableEndpointDiscovery = other.enableEndpointDiscovery.value();
    }
    enableHostPrefixInjection = other.enableHostPrefixInjection;
}

GenericClientConfiguration<true>& GenericClientConfiguration<true>::operator=(const GenericClientConfiguration<true>& other)
{
  if (this != &other) {
      *static_cast<ClientConfiguration*>(this) = static_cast<ClientConfiguration>(other);
  }
  return *this;
}
#endif

} // namespace Client
} // namespace Aws
