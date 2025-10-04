/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

/* Generic header includes */
#include <aws/lambda/LambdaErrors.h>
#include <aws/core/client/GenericClientConfiguration.h>
#include <aws/core/client/AWSError.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/client/AsyncCallerContext.h>
#include <aws/core/http/HttpTypes.h>
#include <aws/lambda/LambdaEndpointProvider.h>
#include <future>
#include <functional>
/* End of generic header includes */

/* Service model headers required in LambdaClient header */
#include <aws/lambda/model/AddLayerVersionPermissionResult.h>
#include <aws/lambda/model/AddPermissionResult.h>
#include <aws/lambda/model/CreateAliasResult.h>
#include <aws/lambda/model/CreateCodeSigningConfigResult.h>
#include <aws/lambda/model/CreateEventSourceMappingResult.h>
#include <aws/lambda/model/CreateFunctionResult.h>
#include <aws/lambda/model/CreateFunctionUrlConfigResult.h>
#include <aws/lambda/model/DeleteCodeSigningConfigResult.h>
#include <aws/lambda/model/DeleteEventSourceMappingResult.h>
#include <aws/lambda/model/GetAccountSettingsResult.h>
#include <aws/lambda/model/GetAliasResult.h>
#include <aws/lambda/model/GetCodeSigningConfigResult.h>
#include <aws/lambda/model/GetEventSourceMappingResult.h>
#include <aws/lambda/model/GetFunctionResult.h>
#include <aws/lambda/model/GetFunctionCodeSigningConfigResult.h>
#include <aws/lambda/model/GetFunctionConcurrencyResult.h>
#include <aws/lambda/model/GetFunctionConfigurationResult.h>
#include <aws/lambda/model/GetFunctionEventInvokeConfigResult.h>
#include <aws/lambda/model/GetFunctionRecursionConfigResult.h>
#include <aws/lambda/model/GetFunctionUrlConfigResult.h>
#include <aws/lambda/model/GetLayerVersionResult.h>
#include <aws/lambda/model/GetLayerVersionByArnResult.h>
#include <aws/lambda/model/GetLayerVersionPolicyResult.h>
#include <aws/lambda/model/GetPolicyResult.h>
#include <aws/lambda/model/GetProvisionedConcurrencyConfigResult.h>
#include <aws/lambda/model/GetRuntimeManagementConfigResult.h>
#include <aws/lambda/model/InvokeResult.h>
#include <aws/lambda/model/ListAliasesResult.h>
#include <aws/lambda/model/ListCodeSigningConfigsResult.h>
#include <aws/lambda/model/ListEventSourceMappingsResult.h>
#include <aws/lambda/model/ListFunctionEventInvokeConfigsResult.h>
#include <aws/lambda/model/ListFunctionUrlConfigsResult.h>
#include <aws/lambda/model/ListFunctionsResult.h>
#include <aws/lambda/model/ListFunctionsByCodeSigningConfigResult.h>
#include <aws/lambda/model/ListLayerVersionsResult.h>
#include <aws/lambda/model/ListLayersResult.h>
#include <aws/lambda/model/ListProvisionedConcurrencyConfigsResult.h>
#include <aws/lambda/model/ListTagsResult.h>
#include <aws/lambda/model/ListVersionsByFunctionResult.h>
#include <aws/lambda/model/PublishLayerVersionResult.h>
#include <aws/lambda/model/PublishVersionResult.h>
#include <aws/lambda/model/PutFunctionCodeSigningConfigResult.h>
#include <aws/lambda/model/PutFunctionConcurrencyResult.h>
#include <aws/lambda/model/PutFunctionEventInvokeConfigResult.h>
#include <aws/lambda/model/PutFunctionRecursionConfigResult.h>
#include <aws/lambda/model/PutProvisionedConcurrencyConfigResult.h>
#include <aws/lambda/model/PutRuntimeManagementConfigResult.h>
#include <aws/lambda/model/UpdateAliasResult.h>
#include <aws/lambda/model/UpdateCodeSigningConfigResult.h>
#include <aws/lambda/model/UpdateEventSourceMappingResult.h>
#include <aws/lambda/model/UpdateFunctionCodeResult.h>
#include <aws/lambda/model/UpdateFunctionConfigurationResult.h>
#include <aws/lambda/model/UpdateFunctionEventInvokeConfigResult.h>
#include <aws/lambda/model/UpdateFunctionUrlConfigResult.h>
#include <aws/lambda/model/ListCodeSigningConfigsRequest.h>
#include <aws/lambda/model/ListEventSourceMappingsRequest.h>
#include <aws/lambda/model/ListLayersRequest.h>
#include <aws/lambda/model/GetAccountSettingsRequest.h>
#include <aws/lambda/model/ListFunctionsRequest.h>
#include <aws/core/NoResult.h>
/* End of service model headers required in LambdaClient header */

namespace Aws
{
  namespace Http
  {
    class HttpClient;
    class HttpClientFactory;
  } // namespace Http

  namespace Utils
  {
    template< typename R, typename E> class Outcome;

    namespace Threading
    {
      class Executor;
    } // namespace Threading
  } // namespace Utils

  namespace Auth
  {
    class AWSCredentials;
    class AWSCredentialsProvider;
  } // namespace Auth

  namespace Client
  {
    class RetryStrategy;
  } // namespace Client

  namespace Lambda
  {
    using LambdaClientConfiguration = Aws::Client::GenericClientConfiguration;
    using LambdaEndpointProviderBase = Aws::Lambda::Endpoint::LambdaEndpointProviderBase;
    using LambdaEndpointProvider = Aws::Lambda::Endpoint::LambdaEndpointProvider;

    namespace Model
    {
      /* Service model forward declarations required in LambdaClient header */
      class AddLayerVersionPermissionRequest;
      class AddPermissionRequest;
      class CreateAliasRequest;
      class CreateCodeSigningConfigRequest;
      class CreateEventSourceMappingRequest;
      class CreateFunctionRequest;
      class CreateFunctionUrlConfigRequest;
      class DeleteAliasRequest;
      class DeleteCodeSigningConfigRequest;
      class DeleteEventSourceMappingRequest;
      class DeleteFunctionRequest;
      class DeleteFunctionCodeSigningConfigRequest;
      class DeleteFunctionConcurrencyRequest;
      class DeleteFunctionEventInvokeConfigRequest;
      class DeleteFunctionUrlConfigRequest;
      class DeleteLayerVersionRequest;
      class DeleteProvisionedConcurrencyConfigRequest;
      class GetAccountSettingsRequest;
      class GetAliasRequest;
      class GetCodeSigningConfigRequest;
      class GetEventSourceMappingRequest;
      class GetFunctionRequest;
      class GetFunctionCodeSigningConfigRequest;
      class GetFunctionConcurrencyRequest;
      class GetFunctionConfigurationRequest;
      class GetFunctionEventInvokeConfigRequest;
      class GetFunctionRecursionConfigRequest;
      class GetFunctionUrlConfigRequest;
      class GetLayerVersionRequest;
      class GetLayerVersionByArnRequest;
      class GetLayerVersionPolicyRequest;
      class GetPolicyRequest;
      class GetProvisionedConcurrencyConfigRequest;
      class GetRuntimeManagementConfigRequest;
      class InvokeRequest;
      class InvokeWithResponseStreamRequest;
      class ListAliasesRequest;
      class ListCodeSigningConfigsRequest;
      class ListEventSourceMappingsRequest;
      class ListFunctionEventInvokeConfigsRequest;
      class ListFunctionUrlConfigsRequest;
      class ListFunctionsRequest;
      class ListFunctionsByCodeSigningConfigRequest;
      class ListLayerVersionsRequest;
      class ListLayersRequest;
      class ListProvisionedConcurrencyConfigsRequest;
      class ListTagsRequest;
      class ListVersionsByFunctionRequest;
      class PublishLayerVersionRequest;
      class PublishVersionRequest;
      class PutFunctionCodeSigningConfigRequest;
      class PutFunctionConcurrencyRequest;
      class PutFunctionEventInvokeConfigRequest;
      class PutFunctionRecursionConfigRequest;
      class PutProvisionedConcurrencyConfigRequest;
      class PutRuntimeManagementConfigRequest;
      class RemoveLayerVersionPermissionRequest;
      class RemovePermissionRequest;
      class TagResourceRequest;
      class UntagResourceRequest;
      class UpdateAliasRequest;
      class UpdateCodeSigningConfigRequest;
      class UpdateEventSourceMappingRequest;
      class UpdateFunctionCodeRequest;
      class UpdateFunctionConfigurationRequest;
      class UpdateFunctionEventInvokeConfigRequest;
      class UpdateFunctionUrlConfigRequest;
      /* End of service model forward declarations required in LambdaClient header */

      /* Service model Outcome class definitions */
      typedef Aws::Utils::Outcome<AddLayerVersionPermissionResult, LambdaError> AddLayerVersionPermissionOutcome;
      typedef Aws::Utils::Outcome<AddPermissionResult, LambdaError> AddPermissionOutcome;
      typedef Aws::Utils::Outcome<CreateAliasResult, LambdaError> CreateAliasOutcome;
      typedef Aws::Utils::Outcome<CreateCodeSigningConfigResult, LambdaError> CreateCodeSigningConfigOutcome;
      typedef Aws::Utils::Outcome<CreateEventSourceMappingResult, LambdaError> CreateEventSourceMappingOutcome;
      typedef Aws::Utils::Outcome<CreateFunctionResult, LambdaError> CreateFunctionOutcome;
      typedef Aws::Utils::Outcome<CreateFunctionUrlConfigResult, LambdaError> CreateFunctionUrlConfigOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, LambdaError> DeleteAliasOutcome;
      typedef Aws::Utils::Outcome<DeleteCodeSigningConfigResult, LambdaError> DeleteCodeSigningConfigOutcome;
      typedef Aws::Utils::Outcome<DeleteEventSourceMappingResult, LambdaError> DeleteEventSourceMappingOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, LambdaError> DeleteFunctionOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, LambdaError> DeleteFunctionCodeSigningConfigOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, LambdaError> DeleteFunctionConcurrencyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, LambdaError> DeleteFunctionEventInvokeConfigOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, LambdaError> DeleteFunctionUrlConfigOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, LambdaError> DeleteLayerVersionOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, LambdaError> DeleteProvisionedConcurrencyConfigOutcome;
      typedef Aws::Utils::Outcome<GetAccountSettingsResult, LambdaError> GetAccountSettingsOutcome;
      typedef Aws::Utils::Outcome<GetAliasResult, LambdaError> GetAliasOutcome;
      typedef Aws::Utils::Outcome<GetCodeSigningConfigResult, LambdaError> GetCodeSigningConfigOutcome;
      typedef Aws::Utils::Outcome<GetEventSourceMappingResult, LambdaError> GetEventSourceMappingOutcome;
      typedef Aws::Utils::Outcome<GetFunctionResult, LambdaError> GetFunctionOutcome;
      typedef Aws::Utils::Outcome<GetFunctionCodeSigningConfigResult, LambdaError> GetFunctionCodeSigningConfigOutcome;
      typedef Aws::Utils::Outcome<GetFunctionConcurrencyResult, LambdaError> GetFunctionConcurrencyOutcome;
      typedef Aws::Utils::Outcome<GetFunctionConfigurationResult, LambdaError> GetFunctionConfigurationOutcome;
      typedef Aws::Utils::Outcome<GetFunctionEventInvokeConfigResult, LambdaError> GetFunctionEventInvokeConfigOutcome;
      typedef Aws::Utils::Outcome<GetFunctionRecursionConfigResult, LambdaError> GetFunctionRecursionConfigOutcome;
      typedef Aws::Utils::Outcome<GetFunctionUrlConfigResult, LambdaError> GetFunctionUrlConfigOutcome;
      typedef Aws::Utils::Outcome<GetLayerVersionResult, LambdaError> GetLayerVersionOutcome;
      typedef Aws::Utils::Outcome<GetLayerVersionByArnResult, LambdaError> GetLayerVersionByArnOutcome;
      typedef Aws::Utils::Outcome<GetLayerVersionPolicyResult, LambdaError> GetLayerVersionPolicyOutcome;
      typedef Aws::Utils::Outcome<GetPolicyResult, LambdaError> GetPolicyOutcome;
      typedef Aws::Utils::Outcome<GetProvisionedConcurrencyConfigResult, LambdaError> GetProvisionedConcurrencyConfigOutcome;
      typedef Aws::Utils::Outcome<GetRuntimeManagementConfigResult, LambdaError> GetRuntimeManagementConfigOutcome;
      typedef Aws::Utils::Outcome<InvokeResult, LambdaError> InvokeOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, LambdaError> InvokeWithResponseStreamOutcome;
      typedef Aws::Utils::Outcome<ListAliasesResult, LambdaError> ListAliasesOutcome;
      typedef Aws::Utils::Outcome<ListCodeSigningConfigsResult, LambdaError> ListCodeSigningConfigsOutcome;
      typedef Aws::Utils::Outcome<ListEventSourceMappingsResult, LambdaError> ListEventSourceMappingsOutcome;
      typedef Aws::Utils::Outcome<ListFunctionEventInvokeConfigsResult, LambdaError> ListFunctionEventInvokeConfigsOutcome;
      typedef Aws::Utils::Outcome<ListFunctionUrlConfigsResult, LambdaError> ListFunctionUrlConfigsOutcome;
      typedef Aws::Utils::Outcome<ListFunctionsResult, LambdaError> ListFunctionsOutcome;
      typedef Aws::Utils::Outcome<ListFunctionsByCodeSigningConfigResult, LambdaError> ListFunctionsByCodeSigningConfigOutcome;
      typedef Aws::Utils::Outcome<ListLayerVersionsResult, LambdaError> ListLayerVersionsOutcome;
      typedef Aws::Utils::Outcome<ListLayersResult, LambdaError> ListLayersOutcome;
      typedef Aws::Utils::Outcome<ListProvisionedConcurrencyConfigsResult, LambdaError> ListProvisionedConcurrencyConfigsOutcome;
      typedef Aws::Utils::Outcome<ListTagsResult, LambdaError> ListTagsOutcome;
      typedef Aws::Utils::Outcome<ListVersionsByFunctionResult, LambdaError> ListVersionsByFunctionOutcome;
      typedef Aws::Utils::Outcome<PublishLayerVersionResult, LambdaError> PublishLayerVersionOutcome;
      typedef Aws::Utils::Outcome<PublishVersionResult, LambdaError> PublishVersionOutcome;
      typedef Aws::Utils::Outcome<PutFunctionCodeSigningConfigResult, LambdaError> PutFunctionCodeSigningConfigOutcome;
      typedef Aws::Utils::Outcome<PutFunctionConcurrencyResult, LambdaError> PutFunctionConcurrencyOutcome;
      typedef Aws::Utils::Outcome<PutFunctionEventInvokeConfigResult, LambdaError> PutFunctionEventInvokeConfigOutcome;
      typedef Aws::Utils::Outcome<PutFunctionRecursionConfigResult, LambdaError> PutFunctionRecursionConfigOutcome;
      typedef Aws::Utils::Outcome<PutProvisionedConcurrencyConfigResult, LambdaError> PutProvisionedConcurrencyConfigOutcome;
      typedef Aws::Utils::Outcome<PutRuntimeManagementConfigResult, LambdaError> PutRuntimeManagementConfigOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, LambdaError> RemoveLayerVersionPermissionOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, LambdaError> RemovePermissionOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, LambdaError> TagResourceOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, LambdaError> UntagResourceOutcome;
      typedef Aws::Utils::Outcome<UpdateAliasResult, LambdaError> UpdateAliasOutcome;
      typedef Aws::Utils::Outcome<UpdateCodeSigningConfigResult, LambdaError> UpdateCodeSigningConfigOutcome;
      typedef Aws::Utils::Outcome<UpdateEventSourceMappingResult, LambdaError> UpdateEventSourceMappingOutcome;
      typedef Aws::Utils::Outcome<UpdateFunctionCodeResult, LambdaError> UpdateFunctionCodeOutcome;
      typedef Aws::Utils::Outcome<UpdateFunctionConfigurationResult, LambdaError> UpdateFunctionConfigurationOutcome;
      typedef Aws::Utils::Outcome<UpdateFunctionEventInvokeConfigResult, LambdaError> UpdateFunctionEventInvokeConfigOutcome;
      typedef Aws::Utils::Outcome<UpdateFunctionUrlConfigResult, LambdaError> UpdateFunctionUrlConfigOutcome;
      /* End of service model Outcome class definitions */

      /* Service model Outcome callable definitions */
      typedef std::future<AddLayerVersionPermissionOutcome> AddLayerVersionPermissionOutcomeCallable;
      typedef std::future<AddPermissionOutcome> AddPermissionOutcomeCallable;
      typedef std::future<CreateAliasOutcome> CreateAliasOutcomeCallable;
      typedef std::future<CreateCodeSigningConfigOutcome> CreateCodeSigningConfigOutcomeCallable;
      typedef std::future<CreateEventSourceMappingOutcome> CreateEventSourceMappingOutcomeCallable;
      typedef std::future<CreateFunctionOutcome> CreateFunctionOutcomeCallable;
      typedef std::future<CreateFunctionUrlConfigOutcome> CreateFunctionUrlConfigOutcomeCallable;
      typedef std::future<DeleteAliasOutcome> DeleteAliasOutcomeCallable;
      typedef std::future<DeleteCodeSigningConfigOutcome> DeleteCodeSigningConfigOutcomeCallable;
      typedef std::future<DeleteEventSourceMappingOutcome> DeleteEventSourceMappingOutcomeCallable;
      typedef std::future<DeleteFunctionOutcome> DeleteFunctionOutcomeCallable;
      typedef std::future<DeleteFunctionCodeSigningConfigOutcome> DeleteFunctionCodeSigningConfigOutcomeCallable;
      typedef std::future<DeleteFunctionConcurrencyOutcome> DeleteFunctionConcurrencyOutcomeCallable;
      typedef std::future<DeleteFunctionEventInvokeConfigOutcome> DeleteFunctionEventInvokeConfigOutcomeCallable;
      typedef std::future<DeleteFunctionUrlConfigOutcome> DeleteFunctionUrlConfigOutcomeCallable;
      typedef std::future<DeleteLayerVersionOutcome> DeleteLayerVersionOutcomeCallable;
      typedef std::future<DeleteProvisionedConcurrencyConfigOutcome> DeleteProvisionedConcurrencyConfigOutcomeCallable;
      typedef std::future<GetAccountSettingsOutcome> GetAccountSettingsOutcomeCallable;
      typedef std::future<GetAliasOutcome> GetAliasOutcomeCallable;
      typedef std::future<GetCodeSigningConfigOutcome> GetCodeSigningConfigOutcomeCallable;
      typedef std::future<GetEventSourceMappingOutcome> GetEventSourceMappingOutcomeCallable;
      typedef std::future<GetFunctionOutcome> GetFunctionOutcomeCallable;
      typedef std::future<GetFunctionCodeSigningConfigOutcome> GetFunctionCodeSigningConfigOutcomeCallable;
      typedef std::future<GetFunctionConcurrencyOutcome> GetFunctionConcurrencyOutcomeCallable;
      typedef std::future<GetFunctionConfigurationOutcome> GetFunctionConfigurationOutcomeCallable;
      typedef std::future<GetFunctionEventInvokeConfigOutcome> GetFunctionEventInvokeConfigOutcomeCallable;
      typedef std::future<GetFunctionRecursionConfigOutcome> GetFunctionRecursionConfigOutcomeCallable;
      typedef std::future<GetFunctionUrlConfigOutcome> GetFunctionUrlConfigOutcomeCallable;
      typedef std::future<GetLayerVersionOutcome> GetLayerVersionOutcomeCallable;
      typedef std::future<GetLayerVersionByArnOutcome> GetLayerVersionByArnOutcomeCallable;
      typedef std::future<GetLayerVersionPolicyOutcome> GetLayerVersionPolicyOutcomeCallable;
      typedef std::future<GetPolicyOutcome> GetPolicyOutcomeCallable;
      typedef std::future<GetProvisionedConcurrencyConfigOutcome> GetProvisionedConcurrencyConfigOutcomeCallable;
      typedef std::future<GetRuntimeManagementConfigOutcome> GetRuntimeManagementConfigOutcomeCallable;
      typedef std::future<InvokeOutcome> InvokeOutcomeCallable;
      typedef std::future<InvokeWithResponseStreamOutcome> InvokeWithResponseStreamOutcomeCallable;
      typedef std::future<ListAliasesOutcome> ListAliasesOutcomeCallable;
      typedef std::future<ListCodeSigningConfigsOutcome> ListCodeSigningConfigsOutcomeCallable;
      typedef std::future<ListEventSourceMappingsOutcome> ListEventSourceMappingsOutcomeCallable;
      typedef std::future<ListFunctionEventInvokeConfigsOutcome> ListFunctionEventInvokeConfigsOutcomeCallable;
      typedef std::future<ListFunctionUrlConfigsOutcome> ListFunctionUrlConfigsOutcomeCallable;
      typedef std::future<ListFunctionsOutcome> ListFunctionsOutcomeCallable;
      typedef std::future<ListFunctionsByCodeSigningConfigOutcome> ListFunctionsByCodeSigningConfigOutcomeCallable;
      typedef std::future<ListLayerVersionsOutcome> ListLayerVersionsOutcomeCallable;
      typedef std::future<ListLayersOutcome> ListLayersOutcomeCallable;
      typedef std::future<ListProvisionedConcurrencyConfigsOutcome> ListProvisionedConcurrencyConfigsOutcomeCallable;
      typedef std::future<ListTagsOutcome> ListTagsOutcomeCallable;
      typedef std::future<ListVersionsByFunctionOutcome> ListVersionsByFunctionOutcomeCallable;
      typedef std::future<PublishLayerVersionOutcome> PublishLayerVersionOutcomeCallable;
      typedef std::future<PublishVersionOutcome> PublishVersionOutcomeCallable;
      typedef std::future<PutFunctionCodeSigningConfigOutcome> PutFunctionCodeSigningConfigOutcomeCallable;
      typedef std::future<PutFunctionConcurrencyOutcome> PutFunctionConcurrencyOutcomeCallable;
      typedef std::future<PutFunctionEventInvokeConfigOutcome> PutFunctionEventInvokeConfigOutcomeCallable;
      typedef std::future<PutFunctionRecursionConfigOutcome> PutFunctionRecursionConfigOutcomeCallable;
      typedef std::future<PutProvisionedConcurrencyConfigOutcome> PutProvisionedConcurrencyConfigOutcomeCallable;
      typedef std::future<PutRuntimeManagementConfigOutcome> PutRuntimeManagementConfigOutcomeCallable;
      typedef std::future<RemoveLayerVersionPermissionOutcome> RemoveLayerVersionPermissionOutcomeCallable;
      typedef std::future<RemovePermissionOutcome> RemovePermissionOutcomeCallable;
      typedef std::future<TagResourceOutcome> TagResourceOutcomeCallable;
      typedef std::future<UntagResourceOutcome> UntagResourceOutcomeCallable;
      typedef std::future<UpdateAliasOutcome> UpdateAliasOutcomeCallable;
      typedef std::future<UpdateCodeSigningConfigOutcome> UpdateCodeSigningConfigOutcomeCallable;
      typedef std::future<UpdateEventSourceMappingOutcome> UpdateEventSourceMappingOutcomeCallable;
      typedef std::future<UpdateFunctionCodeOutcome> UpdateFunctionCodeOutcomeCallable;
      typedef std::future<UpdateFunctionConfigurationOutcome> UpdateFunctionConfigurationOutcomeCallable;
      typedef std::future<UpdateFunctionEventInvokeConfigOutcome> UpdateFunctionEventInvokeConfigOutcomeCallable;
      typedef std::future<UpdateFunctionUrlConfigOutcome> UpdateFunctionUrlConfigOutcomeCallable;
      /* End of service model Outcome callable definitions */
    } // namespace Model

    class LambdaClient;

    /* Service model async handlers definitions */
    typedef std::function<void(const LambdaClient*, const Model::AddLayerVersionPermissionRequest&, const Model::AddLayerVersionPermissionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > AddLayerVersionPermissionResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::AddPermissionRequest&, const Model::AddPermissionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > AddPermissionResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::CreateAliasRequest&, const Model::CreateAliasOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateAliasResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::CreateCodeSigningConfigRequest&, const Model::CreateCodeSigningConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateCodeSigningConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::CreateEventSourceMappingRequest&, const Model::CreateEventSourceMappingOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateEventSourceMappingResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::CreateFunctionRequest&, const Model::CreateFunctionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateFunctionResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::CreateFunctionUrlConfigRequest&, const Model::CreateFunctionUrlConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateFunctionUrlConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::DeleteAliasRequest&, const Model::DeleteAliasOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteAliasResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::DeleteCodeSigningConfigRequest&, const Model::DeleteCodeSigningConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteCodeSigningConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::DeleteEventSourceMappingRequest&, const Model::DeleteEventSourceMappingOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteEventSourceMappingResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::DeleteFunctionRequest&, const Model::DeleteFunctionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteFunctionResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::DeleteFunctionCodeSigningConfigRequest&, const Model::DeleteFunctionCodeSigningConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteFunctionCodeSigningConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::DeleteFunctionConcurrencyRequest&, const Model::DeleteFunctionConcurrencyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteFunctionConcurrencyResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::DeleteFunctionEventInvokeConfigRequest&, const Model::DeleteFunctionEventInvokeConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteFunctionEventInvokeConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::DeleteFunctionUrlConfigRequest&, const Model::DeleteFunctionUrlConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteFunctionUrlConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::DeleteLayerVersionRequest&, const Model::DeleteLayerVersionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteLayerVersionResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::DeleteProvisionedConcurrencyConfigRequest&, const Model::DeleteProvisionedConcurrencyConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteProvisionedConcurrencyConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::GetAccountSettingsRequest&, const Model::GetAccountSettingsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetAccountSettingsResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::GetAliasRequest&, const Model::GetAliasOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetAliasResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::GetCodeSigningConfigRequest&, const Model::GetCodeSigningConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetCodeSigningConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::GetEventSourceMappingRequest&, const Model::GetEventSourceMappingOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetEventSourceMappingResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::GetFunctionRequest&, const Model::GetFunctionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetFunctionResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::GetFunctionCodeSigningConfigRequest&, const Model::GetFunctionCodeSigningConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetFunctionCodeSigningConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::GetFunctionConcurrencyRequest&, const Model::GetFunctionConcurrencyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetFunctionConcurrencyResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::GetFunctionConfigurationRequest&, const Model::GetFunctionConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetFunctionConfigurationResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::GetFunctionEventInvokeConfigRequest&, const Model::GetFunctionEventInvokeConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetFunctionEventInvokeConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::GetFunctionRecursionConfigRequest&, const Model::GetFunctionRecursionConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetFunctionRecursionConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::GetFunctionUrlConfigRequest&, const Model::GetFunctionUrlConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetFunctionUrlConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::GetLayerVersionRequest&, const Model::GetLayerVersionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetLayerVersionResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::GetLayerVersionByArnRequest&, const Model::GetLayerVersionByArnOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetLayerVersionByArnResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::GetLayerVersionPolicyRequest&, const Model::GetLayerVersionPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetLayerVersionPolicyResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::GetPolicyRequest&, const Model::GetPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetPolicyResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::GetProvisionedConcurrencyConfigRequest&, const Model::GetProvisionedConcurrencyConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetProvisionedConcurrencyConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::GetRuntimeManagementConfigRequest&, const Model::GetRuntimeManagementConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetRuntimeManagementConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::InvokeRequest&, Model::InvokeOutcome, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > InvokeResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::InvokeWithResponseStreamRequest&, const Model::InvokeWithResponseStreamOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > InvokeWithResponseStreamResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::ListAliasesRequest&, const Model::ListAliasesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListAliasesResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::ListCodeSigningConfigsRequest&, const Model::ListCodeSigningConfigsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListCodeSigningConfigsResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::ListEventSourceMappingsRequest&, const Model::ListEventSourceMappingsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListEventSourceMappingsResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::ListFunctionEventInvokeConfigsRequest&, const Model::ListFunctionEventInvokeConfigsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListFunctionEventInvokeConfigsResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::ListFunctionUrlConfigsRequest&, const Model::ListFunctionUrlConfigsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListFunctionUrlConfigsResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::ListFunctionsRequest&, const Model::ListFunctionsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListFunctionsResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::ListFunctionsByCodeSigningConfigRequest&, const Model::ListFunctionsByCodeSigningConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListFunctionsByCodeSigningConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::ListLayerVersionsRequest&, const Model::ListLayerVersionsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListLayerVersionsResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::ListLayersRequest&, const Model::ListLayersOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListLayersResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::ListProvisionedConcurrencyConfigsRequest&, const Model::ListProvisionedConcurrencyConfigsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListProvisionedConcurrencyConfigsResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::ListTagsRequest&, const Model::ListTagsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListTagsResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::ListVersionsByFunctionRequest&, const Model::ListVersionsByFunctionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListVersionsByFunctionResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::PublishLayerVersionRequest&, const Model::PublishLayerVersionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PublishLayerVersionResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::PublishVersionRequest&, const Model::PublishVersionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PublishVersionResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::PutFunctionCodeSigningConfigRequest&, const Model::PutFunctionCodeSigningConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutFunctionCodeSigningConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::PutFunctionConcurrencyRequest&, const Model::PutFunctionConcurrencyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutFunctionConcurrencyResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::PutFunctionEventInvokeConfigRequest&, const Model::PutFunctionEventInvokeConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutFunctionEventInvokeConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::PutFunctionRecursionConfigRequest&, const Model::PutFunctionRecursionConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutFunctionRecursionConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::PutProvisionedConcurrencyConfigRequest&, const Model::PutProvisionedConcurrencyConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutProvisionedConcurrencyConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::PutRuntimeManagementConfigRequest&, const Model::PutRuntimeManagementConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutRuntimeManagementConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::RemoveLayerVersionPermissionRequest&, const Model::RemoveLayerVersionPermissionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > RemoveLayerVersionPermissionResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::RemovePermissionRequest&, const Model::RemovePermissionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > RemovePermissionResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::TagResourceRequest&, const Model::TagResourceOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > TagResourceResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::UntagResourceRequest&, const Model::UntagResourceOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UntagResourceResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::UpdateAliasRequest&, const Model::UpdateAliasOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateAliasResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::UpdateCodeSigningConfigRequest&, const Model::UpdateCodeSigningConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateCodeSigningConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::UpdateEventSourceMappingRequest&, const Model::UpdateEventSourceMappingOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateEventSourceMappingResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::UpdateFunctionCodeRequest&, const Model::UpdateFunctionCodeOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateFunctionCodeResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::UpdateFunctionConfigurationRequest&, const Model::UpdateFunctionConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateFunctionConfigurationResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::UpdateFunctionEventInvokeConfigRequest&, const Model::UpdateFunctionEventInvokeConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateFunctionEventInvokeConfigResponseReceivedHandler;
    typedef std::function<void(const LambdaClient*, const Model::UpdateFunctionUrlConfigRequest&, const Model::UpdateFunctionUrlConfigOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateFunctionUrlConfigResponseReceivedHandler;
    /* End of service model async handlers definitions */
  } // namespace Lambda
} // namespace Aws
