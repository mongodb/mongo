/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/Outcome.h>
#include <aws/core/auth/AWSAuthSigner.h>
#include <aws/core/client/CoreErrors.h>
#include <aws/core/client/RetryStrategy.h>
#include <aws/core/http/HttpClient.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/http/HttpClientFactory.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/core/utils/threading/Executor.h>
#include <aws/core/utils/DNS.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/logging/ErrorMacros.h>
#include <aws/core/utils/event/EventStream.h>

#include <aws/lambda/LambdaClient.h>
#include <aws/lambda/LambdaErrorMarshaller.h>
#include <aws/lambda/LambdaEndpointProvider.h>
#include <aws/lambda/model/AddLayerVersionPermissionRequest.h>
#include <aws/lambda/model/AddPermissionRequest.h>
#include <aws/lambda/model/CreateAliasRequest.h>
#include <aws/lambda/model/CreateCodeSigningConfigRequest.h>
#include <aws/lambda/model/CreateEventSourceMappingRequest.h>
#include <aws/lambda/model/CreateFunctionRequest.h>
#include <aws/lambda/model/CreateFunctionUrlConfigRequest.h>
#include <aws/lambda/model/DeleteAliasRequest.h>
#include <aws/lambda/model/DeleteCodeSigningConfigRequest.h>
#include <aws/lambda/model/DeleteEventSourceMappingRequest.h>
#include <aws/lambda/model/DeleteFunctionRequest.h>
#include <aws/lambda/model/DeleteFunctionCodeSigningConfigRequest.h>
#include <aws/lambda/model/DeleteFunctionConcurrencyRequest.h>
#include <aws/lambda/model/DeleteFunctionEventInvokeConfigRequest.h>
#include <aws/lambda/model/DeleteFunctionUrlConfigRequest.h>
#include <aws/lambda/model/DeleteLayerVersionRequest.h>
#include <aws/lambda/model/DeleteProvisionedConcurrencyConfigRequest.h>
#include <aws/lambda/model/GetAccountSettingsRequest.h>
#include <aws/lambda/model/GetAliasRequest.h>
#include <aws/lambda/model/GetCodeSigningConfigRequest.h>
#include <aws/lambda/model/GetEventSourceMappingRequest.h>
#include <aws/lambda/model/GetFunctionRequest.h>
#include <aws/lambda/model/GetFunctionCodeSigningConfigRequest.h>
#include <aws/lambda/model/GetFunctionConcurrencyRequest.h>
#include <aws/lambda/model/GetFunctionConfigurationRequest.h>
#include <aws/lambda/model/GetFunctionEventInvokeConfigRequest.h>
#include <aws/lambda/model/GetFunctionRecursionConfigRequest.h>
#include <aws/lambda/model/GetFunctionUrlConfigRequest.h>
#include <aws/lambda/model/GetLayerVersionRequest.h>
#include <aws/lambda/model/GetLayerVersionByArnRequest.h>
#include <aws/lambda/model/GetLayerVersionPolicyRequest.h>
#include <aws/lambda/model/GetPolicyRequest.h>
#include <aws/lambda/model/GetProvisionedConcurrencyConfigRequest.h>
#include <aws/lambda/model/GetRuntimeManagementConfigRequest.h>
#include <aws/lambda/model/InvokeRequest.h>
#include <aws/lambda/model/InvokeWithResponseStreamRequest.h>
#include <aws/lambda/model/ListAliasesRequest.h>
#include <aws/lambda/model/ListCodeSigningConfigsRequest.h>
#include <aws/lambda/model/ListEventSourceMappingsRequest.h>
#include <aws/lambda/model/ListFunctionEventInvokeConfigsRequest.h>
#include <aws/lambda/model/ListFunctionUrlConfigsRequest.h>
#include <aws/lambda/model/ListFunctionsRequest.h>
#include <aws/lambda/model/ListFunctionsByCodeSigningConfigRequest.h>
#include <aws/lambda/model/ListLayerVersionsRequest.h>
#include <aws/lambda/model/ListLayersRequest.h>
#include <aws/lambda/model/ListProvisionedConcurrencyConfigsRequest.h>
#include <aws/lambda/model/ListTagsRequest.h>
#include <aws/lambda/model/ListVersionsByFunctionRequest.h>
#include <aws/lambda/model/PublishLayerVersionRequest.h>
#include <aws/lambda/model/PublishVersionRequest.h>
#include <aws/lambda/model/PutFunctionCodeSigningConfigRequest.h>
#include <aws/lambda/model/PutFunctionConcurrencyRequest.h>
#include <aws/lambda/model/PutFunctionEventInvokeConfigRequest.h>
#include <aws/lambda/model/PutFunctionRecursionConfigRequest.h>
#include <aws/lambda/model/PutProvisionedConcurrencyConfigRequest.h>
#include <aws/lambda/model/PutRuntimeManagementConfigRequest.h>
#include <aws/lambda/model/RemoveLayerVersionPermissionRequest.h>
#include <aws/lambda/model/RemovePermissionRequest.h>
#include <aws/lambda/model/TagResourceRequest.h>
#include <aws/lambda/model/UntagResourceRequest.h>
#include <aws/lambda/model/UpdateAliasRequest.h>
#include <aws/lambda/model/UpdateCodeSigningConfigRequest.h>
#include <aws/lambda/model/UpdateEventSourceMappingRequest.h>
#include <aws/lambda/model/UpdateFunctionCodeRequest.h>
#include <aws/lambda/model/UpdateFunctionConfigurationRequest.h>
#include <aws/lambda/model/UpdateFunctionEventInvokeConfigRequest.h>
#include <aws/lambda/model/UpdateFunctionUrlConfigRequest.h>

#include <smithy/tracing/TracingUtils.h>


using namespace Aws;
using namespace Aws::Auth;
using namespace Aws::Client;
using namespace Aws::Lambda;
using namespace Aws::Lambda::Model;
using namespace Aws::Http;
using namespace Aws::Utils::Json;
using namespace smithy::components::tracing;
using ResolveEndpointOutcome = Aws::Endpoint::ResolveEndpointOutcome;

namespace Aws
{
  namespace Lambda
  {
    const char SERVICE_NAME[] = "lambda";
    const char ALLOCATION_TAG[] = "LambdaClient";
  }
}
const char* LambdaClient::GetServiceName() {return SERVICE_NAME;}
const char* LambdaClient::GetAllocationTag() {return ALLOCATION_TAG;}

LambdaClient::LambdaClient(const Lambda::LambdaClientConfiguration& clientConfiguration,
                           std::shared_ptr<LambdaEndpointProviderBase> endpointProvider) :
  BASECLASS(clientConfiguration,
            Aws::MakeShared<Aws::Auth::DefaultAuthSignerProvider>(ALLOCATION_TAG,
                                                                  Aws::MakeShared<DefaultAWSCredentialsProviderChain>(ALLOCATION_TAG),
                                                                  SERVICE_NAME,
                                                                  Aws::Region::ComputeSignerRegion(clientConfiguration.region)),
            Aws::MakeShared<LambdaErrorMarshaller>(ALLOCATION_TAG)),
  m_clientConfiguration(clientConfiguration),
  m_endpointProvider(endpointProvider ? std::move(endpointProvider) : Aws::MakeShared<LambdaEndpointProvider>(ALLOCATION_TAG))
{
  init(m_clientConfiguration);
}

LambdaClient::LambdaClient(const AWSCredentials& credentials,
                           std::shared_ptr<LambdaEndpointProviderBase> endpointProvider,
                           const Lambda::LambdaClientConfiguration& clientConfiguration) :
  BASECLASS(clientConfiguration,
            Aws::MakeShared<Aws::Auth::DefaultAuthSignerProvider>(ALLOCATION_TAG,
                                                                  Aws::MakeShared<SimpleAWSCredentialsProvider>(ALLOCATION_TAG, credentials),
                                                                  SERVICE_NAME,
                                                                  Aws::Region::ComputeSignerRegion(clientConfiguration.region)),
            Aws::MakeShared<LambdaErrorMarshaller>(ALLOCATION_TAG)),
    m_clientConfiguration(clientConfiguration),
    m_endpointProvider(endpointProvider ? std::move(endpointProvider) : Aws::MakeShared<LambdaEndpointProvider>(ALLOCATION_TAG))
{
  init(m_clientConfiguration);
}

LambdaClient::LambdaClient(const std::shared_ptr<AWSCredentialsProvider>& credentialsProvider,
                           std::shared_ptr<LambdaEndpointProviderBase> endpointProvider,
                           const Lambda::LambdaClientConfiguration& clientConfiguration) :
  BASECLASS(clientConfiguration,
            Aws::MakeShared<Aws::Auth::DefaultAuthSignerProvider>(ALLOCATION_TAG,
                                                                  credentialsProvider,
                                                                  SERVICE_NAME,
                                                                  Aws::Region::ComputeSignerRegion(clientConfiguration.region)),
            Aws::MakeShared<LambdaErrorMarshaller>(ALLOCATION_TAG)),
    m_clientConfiguration(clientConfiguration),
    m_endpointProvider(endpointProvider ? std::move(endpointProvider) : Aws::MakeShared<LambdaEndpointProvider>(ALLOCATION_TAG))
{
  init(m_clientConfiguration);
}

    /* Legacy constructors due deprecation */
  LambdaClient::LambdaClient(const Client::ClientConfiguration& clientConfiguration) :
  BASECLASS(clientConfiguration,
            Aws::MakeShared<Aws::Auth::DefaultAuthSignerProvider>(ALLOCATION_TAG,
                                                                  Aws::MakeShared<DefaultAWSCredentialsProviderChain>(ALLOCATION_TAG),
                                                                  SERVICE_NAME,
                                                                  Aws::Region::ComputeSignerRegion(clientConfiguration.region)),
            Aws::MakeShared<LambdaErrorMarshaller>(ALLOCATION_TAG)),
  m_clientConfiguration(clientConfiguration),
  m_endpointProvider(Aws::MakeShared<LambdaEndpointProvider>(ALLOCATION_TAG))
{
  init(m_clientConfiguration);
}

LambdaClient::LambdaClient(const AWSCredentials& credentials,
                           const Client::ClientConfiguration& clientConfiguration) :
  BASECLASS(clientConfiguration,
            Aws::MakeShared<Aws::Auth::DefaultAuthSignerProvider>(ALLOCATION_TAG,
                                                                  Aws::MakeShared<SimpleAWSCredentialsProvider>(ALLOCATION_TAG, credentials),
                                                                  SERVICE_NAME,
                                                                  Aws::Region::ComputeSignerRegion(clientConfiguration.region)),
            Aws::MakeShared<LambdaErrorMarshaller>(ALLOCATION_TAG)),
    m_clientConfiguration(clientConfiguration),
    m_endpointProvider(Aws::MakeShared<LambdaEndpointProvider>(ALLOCATION_TAG))
{
  init(m_clientConfiguration);
}

LambdaClient::LambdaClient(const std::shared_ptr<AWSCredentialsProvider>& credentialsProvider,
                           const Client::ClientConfiguration& clientConfiguration) :
  BASECLASS(clientConfiguration,
            Aws::MakeShared<Aws::Auth::DefaultAuthSignerProvider>(ALLOCATION_TAG,
                                                                  credentialsProvider,
                                                                  SERVICE_NAME,
                                                                  Aws::Region::ComputeSignerRegion(clientConfiguration.region)),
            Aws::MakeShared<LambdaErrorMarshaller>(ALLOCATION_TAG)),
    m_clientConfiguration(clientConfiguration),
    m_endpointProvider(Aws::MakeShared<LambdaEndpointProvider>(ALLOCATION_TAG))
{
  init(m_clientConfiguration);
}

    /* End of legacy constructors due deprecation */
LambdaClient::~LambdaClient()
{
  ShutdownSdkClient(this, -1);
}

std::shared_ptr<LambdaEndpointProviderBase>& LambdaClient::accessEndpointProvider()
{
  return m_endpointProvider;
}

void LambdaClient::init(const Lambda::LambdaClientConfiguration& config)
{
  AWSClient::SetServiceClientName("Lambda");
  if (!m_clientConfiguration.executor) {
    if (!m_clientConfiguration.configFactories.executorCreateFn()) {
      AWS_LOGSTREAM_FATAL(ALLOCATION_TAG, "Failed to initialize client: config is missing Executor or executorCreateFn");
      m_isInitialized = false;
      return;
    }
    m_clientConfiguration.executor = m_clientConfiguration.configFactories.executorCreateFn();
  }
  AWS_CHECK_PTR(SERVICE_NAME, m_endpointProvider);
  m_endpointProvider->InitBuiltInParameters(config);
}

void LambdaClient::OverrideEndpoint(const Aws::String& endpoint)
{
  AWS_CHECK_PTR(SERVICE_NAME, m_endpointProvider);
  m_endpointProvider->OverrideEndpoint(endpoint);
}

AddLayerVersionPermissionOutcome LambdaClient::AddLayerVersionPermission(const AddLayerVersionPermissionRequest& request) const
{
  AWS_OPERATION_GUARD(AddLayerVersionPermission);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, AddLayerVersionPermission, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.LayerNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("AddLayerVersionPermission", "Required field: LayerName, is not set");
    return AddLayerVersionPermissionOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [LayerName]", false));
  }
  if (!request.VersionNumberHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("AddLayerVersionPermission", "Required field: VersionNumber, is not set");
    return AddLayerVersionPermissionOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [VersionNumber]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, AddLayerVersionPermission, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, AddLayerVersionPermission, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".AddLayerVersionPermission",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<AddLayerVersionPermissionOutcome>(
    [&]()-> AddLayerVersionPermissionOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, AddLayerVersionPermission, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2018-10-31/layers/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetLayerName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/versions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetVersionNumber());
      endpointResolutionOutcome.GetResult().AddPathSegments("/policy");
      return AddLayerVersionPermissionOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_POST, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

AddPermissionOutcome LambdaClient::AddPermission(const AddPermissionRequest& request) const
{
  AWS_OPERATION_GUARD(AddPermission);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, AddPermission, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("AddPermission", "Required field: FunctionName, is not set");
    return AddPermissionOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, AddPermission, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, AddPermission, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".AddPermission",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<AddPermissionOutcome>(
    [&]()-> AddPermissionOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, AddPermission, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/policy");
      return AddPermissionOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_POST, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

CreateAliasOutcome LambdaClient::CreateAlias(const CreateAliasRequest& request) const
{
  AWS_OPERATION_GUARD(CreateAlias);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, CreateAlias, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("CreateAlias", "Required field: FunctionName, is not set");
    return CreateAliasOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, CreateAlias, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, CreateAlias, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".CreateAlias",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<CreateAliasOutcome>(
    [&]()-> CreateAliasOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, CreateAlias, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/aliases");
      return CreateAliasOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_POST, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

CreateCodeSigningConfigOutcome LambdaClient::CreateCodeSigningConfig(const CreateCodeSigningConfigRequest& request) const
{
  AWS_OPERATION_GUARD(CreateCodeSigningConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, CreateCodeSigningConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, CreateCodeSigningConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, CreateCodeSigningConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".CreateCodeSigningConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<CreateCodeSigningConfigOutcome>(
    [&]()-> CreateCodeSigningConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, CreateCodeSigningConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2020-04-22/code-signing-configs/");
      return CreateCodeSigningConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_POST, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

CreateEventSourceMappingOutcome LambdaClient::CreateEventSourceMapping(const CreateEventSourceMappingRequest& request) const
{
  AWS_OPERATION_GUARD(CreateEventSourceMapping);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, CreateEventSourceMapping, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, CreateEventSourceMapping, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, CreateEventSourceMapping, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".CreateEventSourceMapping",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<CreateEventSourceMappingOutcome>(
    [&]()-> CreateEventSourceMappingOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, CreateEventSourceMapping, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/event-source-mappings/");
      return CreateEventSourceMappingOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_POST, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

CreateFunctionOutcome LambdaClient::CreateFunction(const CreateFunctionRequest& request) const
{
  AWS_OPERATION_GUARD(CreateFunction);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, CreateFunction, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, CreateFunction, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, CreateFunction, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".CreateFunction",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<CreateFunctionOutcome>(
    [&]()-> CreateFunctionOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, CreateFunction, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/functions");
      return CreateFunctionOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_POST, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

CreateFunctionUrlConfigOutcome LambdaClient::CreateFunctionUrlConfig(const CreateFunctionUrlConfigRequest& request) const
{
  AWS_OPERATION_GUARD(CreateFunctionUrlConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, CreateFunctionUrlConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("CreateFunctionUrlConfig", "Required field: FunctionName, is not set");
    return CreateFunctionUrlConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, CreateFunctionUrlConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, CreateFunctionUrlConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".CreateFunctionUrlConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<CreateFunctionUrlConfigOutcome>(
    [&]()-> CreateFunctionUrlConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, CreateFunctionUrlConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2021-10-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/url");
      return CreateFunctionUrlConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_POST, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

DeleteAliasOutcome LambdaClient::DeleteAlias(const DeleteAliasRequest& request) const
{
  AWS_OPERATION_GUARD(DeleteAlias);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, DeleteAlias, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("DeleteAlias", "Required field: FunctionName, is not set");
    return DeleteAliasOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  if (!request.NameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("DeleteAlias", "Required field: Name, is not set");
    return DeleteAliasOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [Name]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, DeleteAlias, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, DeleteAlias, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".DeleteAlias",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<DeleteAliasOutcome>(
    [&]()-> DeleteAliasOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, DeleteAlias, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/aliases/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetName());
      return DeleteAliasOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_DELETE, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

DeleteCodeSigningConfigOutcome LambdaClient::DeleteCodeSigningConfig(const DeleteCodeSigningConfigRequest& request) const
{
  AWS_OPERATION_GUARD(DeleteCodeSigningConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, DeleteCodeSigningConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.CodeSigningConfigArnHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("DeleteCodeSigningConfig", "Required field: CodeSigningConfigArn, is not set");
    return DeleteCodeSigningConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [CodeSigningConfigArn]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, DeleteCodeSigningConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, DeleteCodeSigningConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".DeleteCodeSigningConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<DeleteCodeSigningConfigOutcome>(
    [&]()-> DeleteCodeSigningConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, DeleteCodeSigningConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2020-04-22/code-signing-configs/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetCodeSigningConfigArn());
      return DeleteCodeSigningConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_DELETE, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

DeleteEventSourceMappingOutcome LambdaClient::DeleteEventSourceMapping(const DeleteEventSourceMappingRequest& request) const
{
  AWS_OPERATION_GUARD(DeleteEventSourceMapping);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, DeleteEventSourceMapping, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.UUIDHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("DeleteEventSourceMapping", "Required field: UUID, is not set");
    return DeleteEventSourceMappingOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [UUID]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, DeleteEventSourceMapping, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, DeleteEventSourceMapping, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".DeleteEventSourceMapping",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<DeleteEventSourceMappingOutcome>(
    [&]()-> DeleteEventSourceMappingOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, DeleteEventSourceMapping, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/event-source-mappings/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetUUID());
      return DeleteEventSourceMappingOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_DELETE, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

DeleteFunctionOutcome LambdaClient::DeleteFunction(const DeleteFunctionRequest& request) const
{
  AWS_OPERATION_GUARD(DeleteFunction);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, DeleteFunction, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("DeleteFunction", "Required field: FunctionName, is not set");
    return DeleteFunctionOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, DeleteFunction, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, DeleteFunction, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".DeleteFunction",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<DeleteFunctionOutcome>(
    [&]()-> DeleteFunctionOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, DeleteFunction, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      return DeleteFunctionOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_DELETE, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

DeleteFunctionCodeSigningConfigOutcome LambdaClient::DeleteFunctionCodeSigningConfig(const DeleteFunctionCodeSigningConfigRequest& request) const
{
  AWS_OPERATION_GUARD(DeleteFunctionCodeSigningConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, DeleteFunctionCodeSigningConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("DeleteFunctionCodeSigningConfig", "Required field: FunctionName, is not set");
    return DeleteFunctionCodeSigningConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, DeleteFunctionCodeSigningConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, DeleteFunctionCodeSigningConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".DeleteFunctionCodeSigningConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<DeleteFunctionCodeSigningConfigOutcome>(
    [&]()-> DeleteFunctionCodeSigningConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, DeleteFunctionCodeSigningConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2020-06-30/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/code-signing-config");
      return DeleteFunctionCodeSigningConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_DELETE, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

DeleteFunctionConcurrencyOutcome LambdaClient::DeleteFunctionConcurrency(const DeleteFunctionConcurrencyRequest& request) const
{
  AWS_OPERATION_GUARD(DeleteFunctionConcurrency);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, DeleteFunctionConcurrency, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("DeleteFunctionConcurrency", "Required field: FunctionName, is not set");
    return DeleteFunctionConcurrencyOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, DeleteFunctionConcurrency, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, DeleteFunctionConcurrency, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".DeleteFunctionConcurrency",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<DeleteFunctionConcurrencyOutcome>(
    [&]()-> DeleteFunctionConcurrencyOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, DeleteFunctionConcurrency, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2017-10-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/concurrency");
      return DeleteFunctionConcurrencyOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_DELETE, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

DeleteFunctionEventInvokeConfigOutcome LambdaClient::DeleteFunctionEventInvokeConfig(const DeleteFunctionEventInvokeConfigRequest& request) const
{
  AWS_OPERATION_GUARD(DeleteFunctionEventInvokeConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, DeleteFunctionEventInvokeConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("DeleteFunctionEventInvokeConfig", "Required field: FunctionName, is not set");
    return DeleteFunctionEventInvokeConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, DeleteFunctionEventInvokeConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, DeleteFunctionEventInvokeConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".DeleteFunctionEventInvokeConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<DeleteFunctionEventInvokeConfigOutcome>(
    [&]()-> DeleteFunctionEventInvokeConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, DeleteFunctionEventInvokeConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2019-09-25/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/event-invoke-config");
      return DeleteFunctionEventInvokeConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_DELETE, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

DeleteFunctionUrlConfigOutcome LambdaClient::DeleteFunctionUrlConfig(const DeleteFunctionUrlConfigRequest& request) const
{
  AWS_OPERATION_GUARD(DeleteFunctionUrlConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, DeleteFunctionUrlConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("DeleteFunctionUrlConfig", "Required field: FunctionName, is not set");
    return DeleteFunctionUrlConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, DeleteFunctionUrlConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, DeleteFunctionUrlConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".DeleteFunctionUrlConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<DeleteFunctionUrlConfigOutcome>(
    [&]()-> DeleteFunctionUrlConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, DeleteFunctionUrlConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2021-10-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/url");
      return DeleteFunctionUrlConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_DELETE, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

DeleteLayerVersionOutcome LambdaClient::DeleteLayerVersion(const DeleteLayerVersionRequest& request) const
{
  AWS_OPERATION_GUARD(DeleteLayerVersion);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, DeleteLayerVersion, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.LayerNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("DeleteLayerVersion", "Required field: LayerName, is not set");
    return DeleteLayerVersionOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [LayerName]", false));
  }
  if (!request.VersionNumberHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("DeleteLayerVersion", "Required field: VersionNumber, is not set");
    return DeleteLayerVersionOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [VersionNumber]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, DeleteLayerVersion, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, DeleteLayerVersion, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".DeleteLayerVersion",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<DeleteLayerVersionOutcome>(
    [&]()-> DeleteLayerVersionOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, DeleteLayerVersion, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2018-10-31/layers/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetLayerName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/versions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetVersionNumber());
      return DeleteLayerVersionOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_DELETE, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

DeleteProvisionedConcurrencyConfigOutcome LambdaClient::DeleteProvisionedConcurrencyConfig(const DeleteProvisionedConcurrencyConfigRequest& request) const
{
  AWS_OPERATION_GUARD(DeleteProvisionedConcurrencyConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, DeleteProvisionedConcurrencyConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("DeleteProvisionedConcurrencyConfig", "Required field: FunctionName, is not set");
    return DeleteProvisionedConcurrencyConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  if (!request.QualifierHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("DeleteProvisionedConcurrencyConfig", "Required field: Qualifier, is not set");
    return DeleteProvisionedConcurrencyConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [Qualifier]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, DeleteProvisionedConcurrencyConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, DeleteProvisionedConcurrencyConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".DeleteProvisionedConcurrencyConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<DeleteProvisionedConcurrencyConfigOutcome>(
    [&]()-> DeleteProvisionedConcurrencyConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, DeleteProvisionedConcurrencyConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2019-09-30/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/provisioned-concurrency");
      return DeleteProvisionedConcurrencyConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_DELETE, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

GetAccountSettingsOutcome LambdaClient::GetAccountSettings(const GetAccountSettingsRequest& request) const
{
  AWS_OPERATION_GUARD(GetAccountSettings);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, GetAccountSettings, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, GetAccountSettings, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, GetAccountSettings, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".GetAccountSettings",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<GetAccountSettingsOutcome>(
    [&]()-> GetAccountSettingsOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, GetAccountSettings, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2016-08-19/account-settings/");
      return GetAccountSettingsOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

GetAliasOutcome LambdaClient::GetAlias(const GetAliasRequest& request) const
{
  AWS_OPERATION_GUARD(GetAlias);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, GetAlias, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("GetAlias", "Required field: FunctionName, is not set");
    return GetAliasOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  if (!request.NameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("GetAlias", "Required field: Name, is not set");
    return GetAliasOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [Name]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, GetAlias, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, GetAlias, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".GetAlias",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<GetAliasOutcome>(
    [&]()-> GetAliasOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, GetAlias, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/aliases/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetName());
      return GetAliasOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

GetCodeSigningConfigOutcome LambdaClient::GetCodeSigningConfig(const GetCodeSigningConfigRequest& request) const
{
  AWS_OPERATION_GUARD(GetCodeSigningConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, GetCodeSigningConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.CodeSigningConfigArnHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("GetCodeSigningConfig", "Required field: CodeSigningConfigArn, is not set");
    return GetCodeSigningConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [CodeSigningConfigArn]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, GetCodeSigningConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, GetCodeSigningConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".GetCodeSigningConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<GetCodeSigningConfigOutcome>(
    [&]()-> GetCodeSigningConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, GetCodeSigningConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2020-04-22/code-signing-configs/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetCodeSigningConfigArn());
      return GetCodeSigningConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

GetEventSourceMappingOutcome LambdaClient::GetEventSourceMapping(const GetEventSourceMappingRequest& request) const
{
  AWS_OPERATION_GUARD(GetEventSourceMapping);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, GetEventSourceMapping, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.UUIDHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("GetEventSourceMapping", "Required field: UUID, is not set");
    return GetEventSourceMappingOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [UUID]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, GetEventSourceMapping, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, GetEventSourceMapping, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".GetEventSourceMapping",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<GetEventSourceMappingOutcome>(
    [&]()-> GetEventSourceMappingOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, GetEventSourceMapping, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/event-source-mappings/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetUUID());
      return GetEventSourceMappingOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

GetFunctionOutcome LambdaClient::GetFunction(const GetFunctionRequest& request) const
{
  AWS_OPERATION_GUARD(GetFunction);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, GetFunction, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("GetFunction", "Required field: FunctionName, is not set");
    return GetFunctionOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, GetFunction, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, GetFunction, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".GetFunction",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<GetFunctionOutcome>(
    [&]()-> GetFunctionOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, GetFunction, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      return GetFunctionOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

GetFunctionCodeSigningConfigOutcome LambdaClient::GetFunctionCodeSigningConfig(const GetFunctionCodeSigningConfigRequest& request) const
{
  AWS_OPERATION_GUARD(GetFunctionCodeSigningConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, GetFunctionCodeSigningConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("GetFunctionCodeSigningConfig", "Required field: FunctionName, is not set");
    return GetFunctionCodeSigningConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, GetFunctionCodeSigningConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, GetFunctionCodeSigningConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".GetFunctionCodeSigningConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<GetFunctionCodeSigningConfigOutcome>(
    [&]()-> GetFunctionCodeSigningConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, GetFunctionCodeSigningConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2020-06-30/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/code-signing-config");
      return GetFunctionCodeSigningConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

GetFunctionConcurrencyOutcome LambdaClient::GetFunctionConcurrency(const GetFunctionConcurrencyRequest& request) const
{
  AWS_OPERATION_GUARD(GetFunctionConcurrency);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, GetFunctionConcurrency, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("GetFunctionConcurrency", "Required field: FunctionName, is not set");
    return GetFunctionConcurrencyOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, GetFunctionConcurrency, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, GetFunctionConcurrency, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".GetFunctionConcurrency",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<GetFunctionConcurrencyOutcome>(
    [&]()-> GetFunctionConcurrencyOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, GetFunctionConcurrency, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2019-09-30/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/concurrency");
      return GetFunctionConcurrencyOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

GetFunctionConfigurationOutcome LambdaClient::GetFunctionConfiguration(const GetFunctionConfigurationRequest& request) const
{
  AWS_OPERATION_GUARD(GetFunctionConfiguration);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, GetFunctionConfiguration, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("GetFunctionConfiguration", "Required field: FunctionName, is not set");
    return GetFunctionConfigurationOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, GetFunctionConfiguration, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, GetFunctionConfiguration, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".GetFunctionConfiguration",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<GetFunctionConfigurationOutcome>(
    [&]()-> GetFunctionConfigurationOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, GetFunctionConfiguration, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/configuration");
      return GetFunctionConfigurationOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

GetFunctionEventInvokeConfigOutcome LambdaClient::GetFunctionEventInvokeConfig(const GetFunctionEventInvokeConfigRequest& request) const
{
  AWS_OPERATION_GUARD(GetFunctionEventInvokeConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, GetFunctionEventInvokeConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("GetFunctionEventInvokeConfig", "Required field: FunctionName, is not set");
    return GetFunctionEventInvokeConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, GetFunctionEventInvokeConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, GetFunctionEventInvokeConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".GetFunctionEventInvokeConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<GetFunctionEventInvokeConfigOutcome>(
    [&]()-> GetFunctionEventInvokeConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, GetFunctionEventInvokeConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2019-09-25/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/event-invoke-config");
      return GetFunctionEventInvokeConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

GetFunctionRecursionConfigOutcome LambdaClient::GetFunctionRecursionConfig(const GetFunctionRecursionConfigRequest& request) const
{
  AWS_OPERATION_GUARD(GetFunctionRecursionConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, GetFunctionRecursionConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("GetFunctionRecursionConfig", "Required field: FunctionName, is not set");
    return GetFunctionRecursionConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, GetFunctionRecursionConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, GetFunctionRecursionConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".GetFunctionRecursionConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<GetFunctionRecursionConfigOutcome>(
    [&]()-> GetFunctionRecursionConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, GetFunctionRecursionConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2024-08-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/recursion-config");
      return GetFunctionRecursionConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

GetFunctionUrlConfigOutcome LambdaClient::GetFunctionUrlConfig(const GetFunctionUrlConfigRequest& request) const
{
  AWS_OPERATION_GUARD(GetFunctionUrlConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, GetFunctionUrlConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("GetFunctionUrlConfig", "Required field: FunctionName, is not set");
    return GetFunctionUrlConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, GetFunctionUrlConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, GetFunctionUrlConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".GetFunctionUrlConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<GetFunctionUrlConfigOutcome>(
    [&]()-> GetFunctionUrlConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, GetFunctionUrlConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2021-10-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/url");
      return GetFunctionUrlConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

GetLayerVersionOutcome LambdaClient::GetLayerVersion(const GetLayerVersionRequest& request) const
{
  AWS_OPERATION_GUARD(GetLayerVersion);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, GetLayerVersion, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.LayerNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("GetLayerVersion", "Required field: LayerName, is not set");
    return GetLayerVersionOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [LayerName]", false));
  }
  if (!request.VersionNumberHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("GetLayerVersion", "Required field: VersionNumber, is not set");
    return GetLayerVersionOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [VersionNumber]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, GetLayerVersion, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, GetLayerVersion, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".GetLayerVersion",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<GetLayerVersionOutcome>(
    [&]()-> GetLayerVersionOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, GetLayerVersion, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2018-10-31/layers/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetLayerName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/versions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetVersionNumber());
      return GetLayerVersionOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

GetLayerVersionByArnOutcome LambdaClient::GetLayerVersionByArn(const GetLayerVersionByArnRequest& request) const
{
  AWS_OPERATION_GUARD(GetLayerVersionByArn);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, GetLayerVersionByArn, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.ArnHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("GetLayerVersionByArn", "Required field: Arn, is not set");
    return GetLayerVersionByArnOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [Arn]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, GetLayerVersionByArn, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, GetLayerVersionByArn, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".GetLayerVersionByArn",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<GetLayerVersionByArnOutcome>(
    [&]()-> GetLayerVersionByArnOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, GetLayerVersionByArn, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      Aws::StringStream ss;
      endpointResolutionOutcome.GetResult().AddPathSegments("/2018-10-31/layers");
      ss.str("?find=LayerVersion");
      endpointResolutionOutcome.GetResult().SetQueryString(ss.str());
      return GetLayerVersionByArnOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

GetLayerVersionPolicyOutcome LambdaClient::GetLayerVersionPolicy(const GetLayerVersionPolicyRequest& request) const
{
  AWS_OPERATION_GUARD(GetLayerVersionPolicy);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, GetLayerVersionPolicy, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.LayerNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("GetLayerVersionPolicy", "Required field: LayerName, is not set");
    return GetLayerVersionPolicyOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [LayerName]", false));
  }
  if (!request.VersionNumberHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("GetLayerVersionPolicy", "Required field: VersionNumber, is not set");
    return GetLayerVersionPolicyOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [VersionNumber]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, GetLayerVersionPolicy, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, GetLayerVersionPolicy, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".GetLayerVersionPolicy",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<GetLayerVersionPolicyOutcome>(
    [&]()-> GetLayerVersionPolicyOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, GetLayerVersionPolicy, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2018-10-31/layers/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetLayerName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/versions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetVersionNumber());
      endpointResolutionOutcome.GetResult().AddPathSegments("/policy");
      return GetLayerVersionPolicyOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

GetPolicyOutcome LambdaClient::GetPolicy(const GetPolicyRequest& request) const
{
  AWS_OPERATION_GUARD(GetPolicy);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, GetPolicy, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("GetPolicy", "Required field: FunctionName, is not set");
    return GetPolicyOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, GetPolicy, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, GetPolicy, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".GetPolicy",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<GetPolicyOutcome>(
    [&]()-> GetPolicyOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, GetPolicy, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/policy");
      return GetPolicyOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

GetProvisionedConcurrencyConfigOutcome LambdaClient::GetProvisionedConcurrencyConfig(const GetProvisionedConcurrencyConfigRequest& request) const
{
  AWS_OPERATION_GUARD(GetProvisionedConcurrencyConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, GetProvisionedConcurrencyConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("GetProvisionedConcurrencyConfig", "Required field: FunctionName, is not set");
    return GetProvisionedConcurrencyConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  if (!request.QualifierHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("GetProvisionedConcurrencyConfig", "Required field: Qualifier, is not set");
    return GetProvisionedConcurrencyConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [Qualifier]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, GetProvisionedConcurrencyConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, GetProvisionedConcurrencyConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".GetProvisionedConcurrencyConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<GetProvisionedConcurrencyConfigOutcome>(
    [&]()-> GetProvisionedConcurrencyConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, GetProvisionedConcurrencyConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2019-09-30/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/provisioned-concurrency");
      return GetProvisionedConcurrencyConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

GetRuntimeManagementConfigOutcome LambdaClient::GetRuntimeManagementConfig(const GetRuntimeManagementConfigRequest& request) const
{
  AWS_OPERATION_GUARD(GetRuntimeManagementConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, GetRuntimeManagementConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("GetRuntimeManagementConfig", "Required field: FunctionName, is not set");
    return GetRuntimeManagementConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, GetRuntimeManagementConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, GetRuntimeManagementConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".GetRuntimeManagementConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<GetRuntimeManagementConfigOutcome>(
    [&]()-> GetRuntimeManagementConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, GetRuntimeManagementConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2021-07-20/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/runtime-management-config");
      return GetRuntimeManagementConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

InvokeOutcome LambdaClient::Invoke(const InvokeRequest& request) const
{
  AWS_OPERATION_GUARD(Invoke);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, Invoke, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("Invoke", "Required field: FunctionName, is not set");
    return InvokeOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, Invoke, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, Invoke, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".Invoke",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<InvokeOutcome>(
    [&]()-> InvokeOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, Invoke, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/invocations");
      return InvokeOutcome(MakeRequestWithUnparsedResponse(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_POST));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

InvokeWithResponseStreamOutcome LambdaClient::InvokeWithResponseStream(InvokeWithResponseStreamRequest& request) const
{
  AWS_OPERATION_GUARD(InvokeWithResponseStream);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, InvokeWithResponseStream, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("InvokeWithResponseStream", "Required field: FunctionName, is not set");
    return InvokeWithResponseStreamOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, InvokeWithResponseStream, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, InvokeWithResponseStream, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".InvokeWithResponseStream",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<InvokeWithResponseStreamOutcome>(
    [&]()-> InvokeWithResponseStreamOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, InvokeWithResponseStream, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2021-11-15/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/response-streaming-invocations");
      request.SetResponseStreamFactory(
          [&] { request.GetEventStreamDecoder().Reset(); return Aws::New<Aws::Utils::Event::EventDecoderStream>(ALLOCATION_TAG, request.GetEventStreamDecoder()); }
      );
      return InvokeWithResponseStreamOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_POST));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

ListAliasesOutcome LambdaClient::ListAliases(const ListAliasesRequest& request) const
{
  AWS_OPERATION_GUARD(ListAliases);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, ListAliases, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("ListAliases", "Required field: FunctionName, is not set");
    return ListAliasesOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, ListAliases, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, ListAliases, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".ListAliases",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<ListAliasesOutcome>(
    [&]()-> ListAliasesOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, ListAliases, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/aliases");
      return ListAliasesOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

ListCodeSigningConfigsOutcome LambdaClient::ListCodeSigningConfigs(const ListCodeSigningConfigsRequest& request) const
{
  AWS_OPERATION_GUARD(ListCodeSigningConfigs);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, ListCodeSigningConfigs, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, ListCodeSigningConfigs, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, ListCodeSigningConfigs, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".ListCodeSigningConfigs",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<ListCodeSigningConfigsOutcome>(
    [&]()-> ListCodeSigningConfigsOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, ListCodeSigningConfigs, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2020-04-22/code-signing-configs/");
      return ListCodeSigningConfigsOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

ListEventSourceMappingsOutcome LambdaClient::ListEventSourceMappings(const ListEventSourceMappingsRequest& request) const
{
  AWS_OPERATION_GUARD(ListEventSourceMappings);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, ListEventSourceMappings, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, ListEventSourceMappings, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, ListEventSourceMappings, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".ListEventSourceMappings",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<ListEventSourceMappingsOutcome>(
    [&]()-> ListEventSourceMappingsOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, ListEventSourceMappings, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/event-source-mappings/");
      return ListEventSourceMappingsOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

ListFunctionEventInvokeConfigsOutcome LambdaClient::ListFunctionEventInvokeConfigs(const ListFunctionEventInvokeConfigsRequest& request) const
{
  AWS_OPERATION_GUARD(ListFunctionEventInvokeConfigs);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, ListFunctionEventInvokeConfigs, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("ListFunctionEventInvokeConfigs", "Required field: FunctionName, is not set");
    return ListFunctionEventInvokeConfigsOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, ListFunctionEventInvokeConfigs, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, ListFunctionEventInvokeConfigs, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".ListFunctionEventInvokeConfigs",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<ListFunctionEventInvokeConfigsOutcome>(
    [&]()-> ListFunctionEventInvokeConfigsOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, ListFunctionEventInvokeConfigs, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2019-09-25/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/event-invoke-config/list");
      return ListFunctionEventInvokeConfigsOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

ListFunctionUrlConfigsOutcome LambdaClient::ListFunctionUrlConfigs(const ListFunctionUrlConfigsRequest& request) const
{
  AWS_OPERATION_GUARD(ListFunctionUrlConfigs);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, ListFunctionUrlConfigs, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("ListFunctionUrlConfigs", "Required field: FunctionName, is not set");
    return ListFunctionUrlConfigsOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, ListFunctionUrlConfigs, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, ListFunctionUrlConfigs, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".ListFunctionUrlConfigs",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<ListFunctionUrlConfigsOutcome>(
    [&]()-> ListFunctionUrlConfigsOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, ListFunctionUrlConfigs, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2021-10-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/urls");
      return ListFunctionUrlConfigsOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

ListFunctionsOutcome LambdaClient::ListFunctions(const ListFunctionsRequest& request) const
{
  AWS_OPERATION_GUARD(ListFunctions);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, ListFunctions, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, ListFunctions, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, ListFunctions, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".ListFunctions",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<ListFunctionsOutcome>(
    [&]()-> ListFunctionsOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, ListFunctions, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/functions/");
      return ListFunctionsOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

ListFunctionsByCodeSigningConfigOutcome LambdaClient::ListFunctionsByCodeSigningConfig(const ListFunctionsByCodeSigningConfigRequest& request) const
{
  AWS_OPERATION_GUARD(ListFunctionsByCodeSigningConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, ListFunctionsByCodeSigningConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.CodeSigningConfigArnHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("ListFunctionsByCodeSigningConfig", "Required field: CodeSigningConfigArn, is not set");
    return ListFunctionsByCodeSigningConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [CodeSigningConfigArn]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, ListFunctionsByCodeSigningConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, ListFunctionsByCodeSigningConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".ListFunctionsByCodeSigningConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<ListFunctionsByCodeSigningConfigOutcome>(
    [&]()-> ListFunctionsByCodeSigningConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, ListFunctionsByCodeSigningConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2020-04-22/code-signing-configs/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetCodeSigningConfigArn());
      endpointResolutionOutcome.GetResult().AddPathSegments("/functions");
      return ListFunctionsByCodeSigningConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

ListLayerVersionsOutcome LambdaClient::ListLayerVersions(const ListLayerVersionsRequest& request) const
{
  AWS_OPERATION_GUARD(ListLayerVersions);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, ListLayerVersions, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.LayerNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("ListLayerVersions", "Required field: LayerName, is not set");
    return ListLayerVersionsOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [LayerName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, ListLayerVersions, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, ListLayerVersions, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".ListLayerVersions",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<ListLayerVersionsOutcome>(
    [&]()-> ListLayerVersionsOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, ListLayerVersions, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2018-10-31/layers/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetLayerName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/versions");
      return ListLayerVersionsOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

ListLayersOutcome LambdaClient::ListLayers(const ListLayersRequest& request) const
{
  AWS_OPERATION_GUARD(ListLayers);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, ListLayers, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, ListLayers, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, ListLayers, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".ListLayers",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<ListLayersOutcome>(
    [&]()-> ListLayersOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, ListLayers, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2018-10-31/layers");
      return ListLayersOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

ListProvisionedConcurrencyConfigsOutcome LambdaClient::ListProvisionedConcurrencyConfigs(const ListProvisionedConcurrencyConfigsRequest& request) const
{
  AWS_OPERATION_GUARD(ListProvisionedConcurrencyConfigs);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, ListProvisionedConcurrencyConfigs, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("ListProvisionedConcurrencyConfigs", "Required field: FunctionName, is not set");
    return ListProvisionedConcurrencyConfigsOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, ListProvisionedConcurrencyConfigs, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, ListProvisionedConcurrencyConfigs, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".ListProvisionedConcurrencyConfigs",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<ListProvisionedConcurrencyConfigsOutcome>(
    [&]()-> ListProvisionedConcurrencyConfigsOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, ListProvisionedConcurrencyConfigs, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      Aws::StringStream ss;
      endpointResolutionOutcome.GetResult().AddPathSegments("/2019-09-30/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/provisioned-concurrency");
      ss.str("?List=ALL");
      endpointResolutionOutcome.GetResult().SetQueryString(ss.str());
      return ListProvisionedConcurrencyConfigsOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

ListTagsOutcome LambdaClient::ListTags(const ListTagsRequest& request) const
{
  AWS_OPERATION_GUARD(ListTags);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, ListTags, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.ResourceHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("ListTags", "Required field: Resource, is not set");
    return ListTagsOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [Resource]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, ListTags, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, ListTags, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".ListTags",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<ListTagsOutcome>(
    [&]()-> ListTagsOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, ListTags, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2017-03-31/tags/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetResource());
      return ListTagsOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

ListVersionsByFunctionOutcome LambdaClient::ListVersionsByFunction(const ListVersionsByFunctionRequest& request) const
{
  AWS_OPERATION_GUARD(ListVersionsByFunction);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, ListVersionsByFunction, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("ListVersionsByFunction", "Required field: FunctionName, is not set");
    return ListVersionsByFunctionOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, ListVersionsByFunction, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, ListVersionsByFunction, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".ListVersionsByFunction",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<ListVersionsByFunctionOutcome>(
    [&]()-> ListVersionsByFunctionOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, ListVersionsByFunction, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/versions");
      return ListVersionsByFunctionOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_GET, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

PublishLayerVersionOutcome LambdaClient::PublishLayerVersion(const PublishLayerVersionRequest& request) const
{
  AWS_OPERATION_GUARD(PublishLayerVersion);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, PublishLayerVersion, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.LayerNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("PublishLayerVersion", "Required field: LayerName, is not set");
    return PublishLayerVersionOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [LayerName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, PublishLayerVersion, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, PublishLayerVersion, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".PublishLayerVersion",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<PublishLayerVersionOutcome>(
    [&]()-> PublishLayerVersionOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, PublishLayerVersion, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2018-10-31/layers/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetLayerName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/versions");
      return PublishLayerVersionOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_POST, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

PublishVersionOutcome LambdaClient::PublishVersion(const PublishVersionRequest& request) const
{
  AWS_OPERATION_GUARD(PublishVersion);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, PublishVersion, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("PublishVersion", "Required field: FunctionName, is not set");
    return PublishVersionOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, PublishVersion, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, PublishVersion, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".PublishVersion",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<PublishVersionOutcome>(
    [&]()-> PublishVersionOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, PublishVersion, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/versions");
      return PublishVersionOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_POST, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

PutFunctionCodeSigningConfigOutcome LambdaClient::PutFunctionCodeSigningConfig(const PutFunctionCodeSigningConfigRequest& request) const
{
  AWS_OPERATION_GUARD(PutFunctionCodeSigningConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, PutFunctionCodeSigningConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("PutFunctionCodeSigningConfig", "Required field: FunctionName, is not set");
    return PutFunctionCodeSigningConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, PutFunctionCodeSigningConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, PutFunctionCodeSigningConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".PutFunctionCodeSigningConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<PutFunctionCodeSigningConfigOutcome>(
    [&]()-> PutFunctionCodeSigningConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, PutFunctionCodeSigningConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2020-06-30/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/code-signing-config");
      return PutFunctionCodeSigningConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_PUT, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

PutFunctionConcurrencyOutcome LambdaClient::PutFunctionConcurrency(const PutFunctionConcurrencyRequest& request) const
{
  AWS_OPERATION_GUARD(PutFunctionConcurrency);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, PutFunctionConcurrency, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("PutFunctionConcurrency", "Required field: FunctionName, is not set");
    return PutFunctionConcurrencyOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, PutFunctionConcurrency, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, PutFunctionConcurrency, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".PutFunctionConcurrency",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<PutFunctionConcurrencyOutcome>(
    [&]()-> PutFunctionConcurrencyOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, PutFunctionConcurrency, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2017-10-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/concurrency");
      return PutFunctionConcurrencyOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_PUT, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

PutFunctionEventInvokeConfigOutcome LambdaClient::PutFunctionEventInvokeConfig(const PutFunctionEventInvokeConfigRequest& request) const
{
  AWS_OPERATION_GUARD(PutFunctionEventInvokeConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, PutFunctionEventInvokeConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("PutFunctionEventInvokeConfig", "Required field: FunctionName, is not set");
    return PutFunctionEventInvokeConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, PutFunctionEventInvokeConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, PutFunctionEventInvokeConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".PutFunctionEventInvokeConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<PutFunctionEventInvokeConfigOutcome>(
    [&]()-> PutFunctionEventInvokeConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, PutFunctionEventInvokeConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2019-09-25/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/event-invoke-config");
      return PutFunctionEventInvokeConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_PUT, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

PutFunctionRecursionConfigOutcome LambdaClient::PutFunctionRecursionConfig(const PutFunctionRecursionConfigRequest& request) const
{
  AWS_OPERATION_GUARD(PutFunctionRecursionConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, PutFunctionRecursionConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("PutFunctionRecursionConfig", "Required field: FunctionName, is not set");
    return PutFunctionRecursionConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, PutFunctionRecursionConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, PutFunctionRecursionConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".PutFunctionRecursionConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<PutFunctionRecursionConfigOutcome>(
    [&]()-> PutFunctionRecursionConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, PutFunctionRecursionConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2024-08-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/recursion-config");
      return PutFunctionRecursionConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_PUT, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

PutProvisionedConcurrencyConfigOutcome LambdaClient::PutProvisionedConcurrencyConfig(const PutProvisionedConcurrencyConfigRequest& request) const
{
  AWS_OPERATION_GUARD(PutProvisionedConcurrencyConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, PutProvisionedConcurrencyConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("PutProvisionedConcurrencyConfig", "Required field: FunctionName, is not set");
    return PutProvisionedConcurrencyConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  if (!request.QualifierHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("PutProvisionedConcurrencyConfig", "Required field: Qualifier, is not set");
    return PutProvisionedConcurrencyConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [Qualifier]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, PutProvisionedConcurrencyConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, PutProvisionedConcurrencyConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".PutProvisionedConcurrencyConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<PutProvisionedConcurrencyConfigOutcome>(
    [&]()-> PutProvisionedConcurrencyConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, PutProvisionedConcurrencyConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2019-09-30/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/provisioned-concurrency");
      return PutProvisionedConcurrencyConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_PUT, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

PutRuntimeManagementConfigOutcome LambdaClient::PutRuntimeManagementConfig(const PutRuntimeManagementConfigRequest& request) const
{
  AWS_OPERATION_GUARD(PutRuntimeManagementConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, PutRuntimeManagementConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("PutRuntimeManagementConfig", "Required field: FunctionName, is not set");
    return PutRuntimeManagementConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, PutRuntimeManagementConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, PutRuntimeManagementConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".PutRuntimeManagementConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<PutRuntimeManagementConfigOutcome>(
    [&]()-> PutRuntimeManagementConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, PutRuntimeManagementConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2021-07-20/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/runtime-management-config");
      return PutRuntimeManagementConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_PUT, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

RemoveLayerVersionPermissionOutcome LambdaClient::RemoveLayerVersionPermission(const RemoveLayerVersionPermissionRequest& request) const
{
  AWS_OPERATION_GUARD(RemoveLayerVersionPermission);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, RemoveLayerVersionPermission, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.LayerNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("RemoveLayerVersionPermission", "Required field: LayerName, is not set");
    return RemoveLayerVersionPermissionOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [LayerName]", false));
  }
  if (!request.VersionNumberHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("RemoveLayerVersionPermission", "Required field: VersionNumber, is not set");
    return RemoveLayerVersionPermissionOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [VersionNumber]", false));
  }
  if (!request.StatementIdHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("RemoveLayerVersionPermission", "Required field: StatementId, is not set");
    return RemoveLayerVersionPermissionOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [StatementId]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, RemoveLayerVersionPermission, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, RemoveLayerVersionPermission, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".RemoveLayerVersionPermission",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<RemoveLayerVersionPermissionOutcome>(
    [&]()-> RemoveLayerVersionPermissionOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, RemoveLayerVersionPermission, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2018-10-31/layers/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetLayerName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/versions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetVersionNumber());
      endpointResolutionOutcome.GetResult().AddPathSegments("/policy/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetStatementId());
      return RemoveLayerVersionPermissionOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_DELETE, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

RemovePermissionOutcome LambdaClient::RemovePermission(const RemovePermissionRequest& request) const
{
  AWS_OPERATION_GUARD(RemovePermission);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, RemovePermission, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("RemovePermission", "Required field: FunctionName, is not set");
    return RemovePermissionOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  if (!request.StatementIdHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("RemovePermission", "Required field: StatementId, is not set");
    return RemovePermissionOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [StatementId]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, RemovePermission, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, RemovePermission, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".RemovePermission",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<RemovePermissionOutcome>(
    [&]()-> RemovePermissionOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, RemovePermission, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/policy/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetStatementId());
      return RemovePermissionOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_DELETE, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

TagResourceOutcome LambdaClient::TagResource(const TagResourceRequest& request) const
{
  AWS_OPERATION_GUARD(TagResource);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, TagResource, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.ResourceHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("TagResource", "Required field: Resource, is not set");
    return TagResourceOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [Resource]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, TagResource, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, TagResource, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".TagResource",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<TagResourceOutcome>(
    [&]()-> TagResourceOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, TagResource, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2017-03-31/tags/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetResource());
      return TagResourceOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_POST, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

UntagResourceOutcome LambdaClient::UntagResource(const UntagResourceRequest& request) const
{
  AWS_OPERATION_GUARD(UntagResource);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, UntagResource, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.ResourceHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("UntagResource", "Required field: Resource, is not set");
    return UntagResourceOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [Resource]", false));
  }
  if (!request.TagKeysHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("UntagResource", "Required field: TagKeys, is not set");
    return UntagResourceOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [TagKeys]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, UntagResource, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, UntagResource, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".UntagResource",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<UntagResourceOutcome>(
    [&]()-> UntagResourceOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, UntagResource, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2017-03-31/tags/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetResource());
      return UntagResourceOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_DELETE, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

UpdateAliasOutcome LambdaClient::UpdateAlias(const UpdateAliasRequest& request) const
{
  AWS_OPERATION_GUARD(UpdateAlias);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, UpdateAlias, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("UpdateAlias", "Required field: FunctionName, is not set");
    return UpdateAliasOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  if (!request.NameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("UpdateAlias", "Required field: Name, is not set");
    return UpdateAliasOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [Name]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, UpdateAlias, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, UpdateAlias, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".UpdateAlias",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<UpdateAliasOutcome>(
    [&]()-> UpdateAliasOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, UpdateAlias, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/aliases/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetName());
      return UpdateAliasOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_PUT, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

UpdateCodeSigningConfigOutcome LambdaClient::UpdateCodeSigningConfig(const UpdateCodeSigningConfigRequest& request) const
{
  AWS_OPERATION_GUARD(UpdateCodeSigningConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, UpdateCodeSigningConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.CodeSigningConfigArnHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("UpdateCodeSigningConfig", "Required field: CodeSigningConfigArn, is not set");
    return UpdateCodeSigningConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [CodeSigningConfigArn]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, UpdateCodeSigningConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, UpdateCodeSigningConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".UpdateCodeSigningConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<UpdateCodeSigningConfigOutcome>(
    [&]()-> UpdateCodeSigningConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, UpdateCodeSigningConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2020-04-22/code-signing-configs/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetCodeSigningConfigArn());
      return UpdateCodeSigningConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_PUT, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

UpdateEventSourceMappingOutcome LambdaClient::UpdateEventSourceMapping(const UpdateEventSourceMappingRequest& request) const
{
  AWS_OPERATION_GUARD(UpdateEventSourceMapping);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, UpdateEventSourceMapping, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.UUIDHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("UpdateEventSourceMapping", "Required field: UUID, is not set");
    return UpdateEventSourceMappingOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [UUID]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, UpdateEventSourceMapping, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, UpdateEventSourceMapping, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".UpdateEventSourceMapping",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<UpdateEventSourceMappingOutcome>(
    [&]()-> UpdateEventSourceMappingOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, UpdateEventSourceMapping, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/event-source-mappings/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetUUID());
      return UpdateEventSourceMappingOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_PUT, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

UpdateFunctionCodeOutcome LambdaClient::UpdateFunctionCode(const UpdateFunctionCodeRequest& request) const
{
  AWS_OPERATION_GUARD(UpdateFunctionCode);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, UpdateFunctionCode, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("UpdateFunctionCode", "Required field: FunctionName, is not set");
    return UpdateFunctionCodeOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, UpdateFunctionCode, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, UpdateFunctionCode, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".UpdateFunctionCode",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<UpdateFunctionCodeOutcome>(
    [&]()-> UpdateFunctionCodeOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, UpdateFunctionCode, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/code");
      return UpdateFunctionCodeOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_PUT, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

UpdateFunctionConfigurationOutcome LambdaClient::UpdateFunctionConfiguration(const UpdateFunctionConfigurationRequest& request) const
{
  AWS_OPERATION_GUARD(UpdateFunctionConfiguration);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, UpdateFunctionConfiguration, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("UpdateFunctionConfiguration", "Required field: FunctionName, is not set");
    return UpdateFunctionConfigurationOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, UpdateFunctionConfiguration, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, UpdateFunctionConfiguration, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".UpdateFunctionConfiguration",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<UpdateFunctionConfigurationOutcome>(
    [&]()-> UpdateFunctionConfigurationOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, UpdateFunctionConfiguration, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2015-03-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/configuration");
      return UpdateFunctionConfigurationOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_PUT, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

UpdateFunctionEventInvokeConfigOutcome LambdaClient::UpdateFunctionEventInvokeConfig(const UpdateFunctionEventInvokeConfigRequest& request) const
{
  AWS_OPERATION_GUARD(UpdateFunctionEventInvokeConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, UpdateFunctionEventInvokeConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("UpdateFunctionEventInvokeConfig", "Required field: FunctionName, is not set");
    return UpdateFunctionEventInvokeConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, UpdateFunctionEventInvokeConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, UpdateFunctionEventInvokeConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".UpdateFunctionEventInvokeConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<UpdateFunctionEventInvokeConfigOutcome>(
    [&]()-> UpdateFunctionEventInvokeConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, UpdateFunctionEventInvokeConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2019-09-25/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/event-invoke-config");
      return UpdateFunctionEventInvokeConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_POST, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

UpdateFunctionUrlConfigOutcome LambdaClient::UpdateFunctionUrlConfig(const UpdateFunctionUrlConfigRequest& request) const
{
  AWS_OPERATION_GUARD(UpdateFunctionUrlConfig);
  AWS_OPERATION_CHECK_PTR(m_endpointProvider, UpdateFunctionUrlConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE);
  if (!request.FunctionNameHasBeenSet())
  {
    AWS_LOGSTREAM_ERROR("UpdateFunctionUrlConfig", "Required field: FunctionName, is not set");
    return UpdateFunctionUrlConfigOutcome(Aws::Client::AWSError<LambdaErrors>(LambdaErrors::MISSING_PARAMETER, "MISSING_PARAMETER", "Missing required field [FunctionName]", false));
  }
  AWS_OPERATION_CHECK_PTR(m_telemetryProvider, UpdateFunctionUrlConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto tracer = m_telemetryProvider->getTracer(this->GetServiceClientName(), {});
  auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
  AWS_OPERATION_CHECK_PTR(meter, UpdateFunctionUrlConfig, CoreErrors, CoreErrors::NOT_INITIALIZED);
  auto span = tracer->CreateSpan(Aws::String(this->GetServiceClientName()) + ".UpdateFunctionUrlConfig",
    {{ TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName() }, { TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName() }, { TracingUtils::SMITHY_SYSTEM_DIMENSION, TracingUtils::SMITHY_METHOD_AWS_VALUE }},
    smithy::components::tracing::SpanKind::CLIENT);
  return TracingUtils::MakeCallWithTiming<UpdateFunctionUrlConfigOutcome>(
    [&]()-> UpdateFunctionUrlConfigOutcome {
      auto endpointResolutionOutcome = TracingUtils::MakeCallWithTiming<ResolveEndpointOutcome>(
          [&]() -> ResolveEndpointOutcome { return m_endpointProvider->ResolveEndpoint(request.GetEndpointContextParams()); },
          TracingUtils::SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC,
          *meter,
          {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
      AWS_OPERATION_CHECK_SUCCESS(endpointResolutionOutcome, UpdateFunctionUrlConfig, CoreErrors, CoreErrors::ENDPOINT_RESOLUTION_FAILURE, endpointResolutionOutcome.GetError().GetMessage());
      endpointResolutionOutcome.GetResult().AddPathSegments("/2021-10-31/functions/");
      endpointResolutionOutcome.GetResult().AddPathSegment(request.GetFunctionName());
      endpointResolutionOutcome.GetResult().AddPathSegments("/url");
      return UpdateFunctionUrlConfigOutcome(MakeRequest(request, endpointResolutionOutcome.GetResult(), Aws::Http::HttpMethod::HTTP_PUT, Aws::Auth::SIGV4_SIGNER));
    },
    TracingUtils::SMITHY_CLIENT_DURATION_METRIC,
    *meter,
    {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

