/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

namespace Aws
{
    namespace Crt
    {
        class ApiHandle;

        namespace Io
        {
            class ClientBootstrap;
            class TlsConnectionOptions;

        }
    }

    /**
     * Like we need to call InitAPI() to initialize aws-sdk-cpp, we need ApiHandle to initialize aws-crt-cpp, which is a wrapper of a collection of common runtime libraries.
     * We will wrap the memory management system and pass it to common runtime libraries via ApiHandle.
     */
    AWS_CORE_API Aws::Crt::ApiHandle* GetApiHandle();

    /**
     * Set the default ClientBootStrap for AWS common runtime libraries in the global scope.
     */
    AWS_CORE_API void SetDefaultClientBootstrap(const std::shared_ptr<Aws::Crt::Io::ClientBootstrap>& clientBootstrap);

    /**
     * Get the default ClientBootStrap for AWS common runtime libraries in the global scope.
     */
    AWS_CORE_API Aws::Crt::Io::ClientBootstrap* GetDefaultClientBootstrap();

    /**
     * Set the default TlsConnectionOptions for AWS common runtime libraries in the global scope.
     */
    AWS_CORE_API void SetDefaultTlsConnectionOptions(const std::shared_ptr<Aws::Crt::Io::TlsConnectionOptions>& tlsConnectionOptions);

    /**
     * Get the default TlsConnectionOptions for AWS common runtime libraries in the global scope.
     */
    AWS_CORE_API Aws::Crt::Io::TlsConnectionOptions* GetDefaultTlsConnectionOptions();

    /**
     * Initialize ApiHandle in aws-crt-cpp.
     */
    void InitializeCrt();

    /**
     * Clean up ApiHandle in aws-crt-cpp.
     */
    void CleanupCrt();

    namespace Utils
    {
        class EnumParseOverflowContainer;
    }
    /**
     * This is used to handle the Enum round tripping problem
     * for when a service updates their enumerations, but the user does not
     * have an up to date client. This container will be initialized during Aws::InitAPI
     * and will be cleaned on Aws::ShutdownAPI.
     */
    AWS_CORE_API Utils::EnumParseOverflowContainer* GetEnumOverflowContainer();

    /**
     * Initializes a global overflow container to a new instance.
     * This should only be called once from within Aws::InitAPI
     */
    void InitializeEnumOverflowContainer();

    /**
     * Destroys the global overflow container instance.
     * This should only be called once from within Aws::ShutdownAPI
     */
    void CleanupEnumOverflowContainer();
}
