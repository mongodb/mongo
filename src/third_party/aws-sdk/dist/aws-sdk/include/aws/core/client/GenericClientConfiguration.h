/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/client/ClientConfiguration.h>

namespace Aws
{
    namespace Client
    {
        /**
         * This mutable structure is used to configure a regular AWS client.
         */
        struct AWS_CORE_API GenericClientConfiguration : public ClientConfiguration
        {
            GenericClientConfiguration(const ClientConfigurationInitValues &configuration = {})
              : ClientConfiguration(configuration)
            {}

            /**
             * Create a configuration based on settings in the aws configuration file for the given profile name.
             * The configuration file location can be set via the environment variable AWS_CONFIG_FILE
             * @param profileName the aws profile name.
             * @param shouldDisableIMDS whether or not to disable IMDS calls.
             */
            GenericClientConfiguration(const char* inputProfileName, bool shouldDisableIMDS = false)
              : ClientConfiguration(inputProfileName, shouldDisableIMDS)
            {}

            /**
             * Create a configuration with a predefined smart defaults
             * @param useSmartDefaults, required to differentiate c-tors
             * @param defaultMode, default mode to use
             * @param shouldDisableIMDS whether or not to disable IMDS calls.
             */
            explicit GenericClientConfiguration(bool useSmartDefaults, const char* defaultMode = "legacy", bool shouldDisableIMDS = false)
              : ClientConfiguration(useSmartDefaults, defaultMode, shouldDisableIMDS)
            {}

            GenericClientConfiguration(const ClientConfiguration& config)
                    : ClientConfiguration(config)
            {}
        };

#if 0
        /**
         * This mutable structure is used to configure a regular AWS client that supports endpoint discovery.
         */
        template <bool EndpointDiscoveryDefaultValT = false>
        struct AWS_CORE_API GenericClientConfiguration<EndpointDiscoveryDefaultValT, true> : public ClientConfiguration
        {
            static const bool EndpointDiscoverySupported = true;
            static const bool EndpointDiscoveryDefaultValue = EndpointDiscoveryDefaultValT;

            GenericClientConfiguration(const ClientConfigurationInitValues &configuration = {});
            GenericClientConfiguration(const char* profileName, bool shouldDisableIMDS = false);
            explicit GenericClientConfiguration(bool useSmartDefaults, const char* defaultMode = "legacy", bool shouldDisableIMDS = false);
            GenericClientConfiguration(const ClientConfiguration& config);
            GenericClientConfiguration(const GenericClientConfiguration&);
            GenericClientConfiguration& operator=(const GenericClientConfiguration&);


            /**
             * Enable host prefix injection.
             * For services whose endpoint is injectable. e.g. servicediscovery, you can modify the http host's prefix so as to add "data-" prefix for DiscoverInstances request.
             * Default to true, enabled. You can disable it for testing purpose.
             */
            bool& enableHostPrefixInjection;

            /**
             * Enable endpoint discovery
             * For some services to dynamically set up their endpoints for different requests.
             * By default, service clients will decide if endpoint discovery is enabled or not.
             * If disabled, regional or overridden endpoint will be used instead.
             * If a request requires endpoint discovery but you disabled it. The request will never succeed.
             * A boolean value is either true of false, use Optional here to have an instance does not contain a value,
             * such that SDK will decide the default behavior as stated before, if no value specified.
             */
            Aws::Crt::Optional<bool>& enableEndpointDiscovery;
        };
#endif
    } // namespace Client
} // namespace Aws
