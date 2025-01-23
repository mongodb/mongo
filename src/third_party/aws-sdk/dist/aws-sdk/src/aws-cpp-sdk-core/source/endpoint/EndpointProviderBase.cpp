/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/endpoint/EndpointProviderBase.h>

namespace Aws
{
namespace Endpoint
{
#ifndef AWS_CORE_EXPORTS // Except for Windows DLL
/**
 * Instantiate endpoint providers
 */
template class EndpointProviderBase<Aws::Client::GenericClientConfiguration,
            Aws::Endpoint::BuiltInParameters,
            Aws::Endpoint::ClientContextParameters>;
#endif
} // namespace Endpoint
} // namespace Aws
