/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

/* Generic header includes */
#include <aws/cognito-identity/CognitoIdentityErrors.h>
#include <aws/core/client/GenericClientConfiguration.h>
#include <aws/core/client/AWSError.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/client/AsyncCallerContext.h>
#include <aws/core/http/HttpTypes.h>
#include <aws/cognito-identity/CognitoIdentityEndpointProvider.h>
#include <future>
#include <functional>
/* End of generic header includes */

/* Service model headers required in CognitoIdentityClient header */
#include <aws/cognito-identity/model/CreateIdentityPoolResult.h>
#include <aws/cognito-identity/model/DeleteIdentitiesResult.h>
#include <aws/cognito-identity/model/DescribeIdentityResult.h>
#include <aws/cognito-identity/model/DescribeIdentityPoolResult.h>
#include <aws/cognito-identity/model/GetCredentialsForIdentityResult.h>
#include <aws/cognito-identity/model/GetIdResult.h>
#include <aws/cognito-identity/model/GetIdentityPoolRolesResult.h>
#include <aws/cognito-identity/model/GetOpenIdTokenResult.h>
#include <aws/cognito-identity/model/GetOpenIdTokenForDeveloperIdentityResult.h>
#include <aws/cognito-identity/model/GetPrincipalTagAttributeMapResult.h>
#include <aws/cognito-identity/model/ListIdentitiesResult.h>
#include <aws/cognito-identity/model/ListIdentityPoolsResult.h>
#include <aws/cognito-identity/model/ListTagsForResourceResult.h>
#include <aws/cognito-identity/model/LookupDeveloperIdentityResult.h>
#include <aws/cognito-identity/model/MergeDeveloperIdentitiesResult.h>
#include <aws/cognito-identity/model/SetPrincipalTagAttributeMapResult.h>
#include <aws/cognito-identity/model/TagResourceResult.h>
#include <aws/cognito-identity/model/UntagResourceResult.h>
#include <aws/cognito-identity/model/UpdateIdentityPoolResult.h>
#include <aws/core/NoResult.h>
/* End of service model headers required in CognitoIdentityClient header */

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

  namespace CognitoIdentity
  {
    using CognitoIdentityClientConfiguration = Aws::Client::GenericClientConfiguration;
    using CognitoIdentityEndpointProviderBase = Aws::CognitoIdentity::Endpoint::CognitoIdentityEndpointProviderBase;
    using CognitoIdentityEndpointProvider = Aws::CognitoIdentity::Endpoint::CognitoIdentityEndpointProvider;

    namespace Model
    {
      /* Service model forward declarations required in CognitoIdentityClient header */
      class CreateIdentityPoolRequest;
      class DeleteIdentitiesRequest;
      class DeleteIdentityPoolRequest;
      class DescribeIdentityRequest;
      class DescribeIdentityPoolRequest;
      class GetCredentialsForIdentityRequest;
      class GetIdRequest;
      class GetIdentityPoolRolesRequest;
      class GetOpenIdTokenRequest;
      class GetOpenIdTokenForDeveloperIdentityRequest;
      class GetPrincipalTagAttributeMapRequest;
      class ListIdentitiesRequest;
      class ListIdentityPoolsRequest;
      class ListTagsForResourceRequest;
      class LookupDeveloperIdentityRequest;
      class MergeDeveloperIdentitiesRequest;
      class SetIdentityPoolRolesRequest;
      class SetPrincipalTagAttributeMapRequest;
      class TagResourceRequest;
      class UnlinkDeveloperIdentityRequest;
      class UnlinkIdentityRequest;
      class UntagResourceRequest;
      class UpdateIdentityPoolRequest;
      /* End of service model forward declarations required in CognitoIdentityClient header */

      /* Service model Outcome class definitions */
      typedef Aws::Utils::Outcome<CreateIdentityPoolResult, CognitoIdentityError> CreateIdentityPoolOutcome;
      typedef Aws::Utils::Outcome<DeleteIdentitiesResult, CognitoIdentityError> DeleteIdentitiesOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, CognitoIdentityError> DeleteIdentityPoolOutcome;
      typedef Aws::Utils::Outcome<DescribeIdentityResult, CognitoIdentityError> DescribeIdentityOutcome;
      typedef Aws::Utils::Outcome<DescribeIdentityPoolResult, CognitoIdentityError> DescribeIdentityPoolOutcome;
      typedef Aws::Utils::Outcome<GetCredentialsForIdentityResult, CognitoIdentityError> GetCredentialsForIdentityOutcome;
      typedef Aws::Utils::Outcome<GetIdResult, CognitoIdentityError> GetIdOutcome;
      typedef Aws::Utils::Outcome<GetIdentityPoolRolesResult, CognitoIdentityError> GetIdentityPoolRolesOutcome;
      typedef Aws::Utils::Outcome<GetOpenIdTokenResult, CognitoIdentityError> GetOpenIdTokenOutcome;
      typedef Aws::Utils::Outcome<GetOpenIdTokenForDeveloperIdentityResult, CognitoIdentityError> GetOpenIdTokenForDeveloperIdentityOutcome;
      typedef Aws::Utils::Outcome<GetPrincipalTagAttributeMapResult, CognitoIdentityError> GetPrincipalTagAttributeMapOutcome;
      typedef Aws::Utils::Outcome<ListIdentitiesResult, CognitoIdentityError> ListIdentitiesOutcome;
      typedef Aws::Utils::Outcome<ListIdentityPoolsResult, CognitoIdentityError> ListIdentityPoolsOutcome;
      typedef Aws::Utils::Outcome<ListTagsForResourceResult, CognitoIdentityError> ListTagsForResourceOutcome;
      typedef Aws::Utils::Outcome<LookupDeveloperIdentityResult, CognitoIdentityError> LookupDeveloperIdentityOutcome;
      typedef Aws::Utils::Outcome<MergeDeveloperIdentitiesResult, CognitoIdentityError> MergeDeveloperIdentitiesOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, CognitoIdentityError> SetIdentityPoolRolesOutcome;
      typedef Aws::Utils::Outcome<SetPrincipalTagAttributeMapResult, CognitoIdentityError> SetPrincipalTagAttributeMapOutcome;
      typedef Aws::Utils::Outcome<TagResourceResult, CognitoIdentityError> TagResourceOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, CognitoIdentityError> UnlinkDeveloperIdentityOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, CognitoIdentityError> UnlinkIdentityOutcome;
      typedef Aws::Utils::Outcome<UntagResourceResult, CognitoIdentityError> UntagResourceOutcome;
      typedef Aws::Utils::Outcome<UpdateIdentityPoolResult, CognitoIdentityError> UpdateIdentityPoolOutcome;
      /* End of service model Outcome class definitions */

      /* Service model Outcome callable definitions */
      typedef std::future<CreateIdentityPoolOutcome> CreateIdentityPoolOutcomeCallable;
      typedef std::future<DeleteIdentitiesOutcome> DeleteIdentitiesOutcomeCallable;
      typedef std::future<DeleteIdentityPoolOutcome> DeleteIdentityPoolOutcomeCallable;
      typedef std::future<DescribeIdentityOutcome> DescribeIdentityOutcomeCallable;
      typedef std::future<DescribeIdentityPoolOutcome> DescribeIdentityPoolOutcomeCallable;
      typedef std::future<GetCredentialsForIdentityOutcome> GetCredentialsForIdentityOutcomeCallable;
      typedef std::future<GetIdOutcome> GetIdOutcomeCallable;
      typedef std::future<GetIdentityPoolRolesOutcome> GetIdentityPoolRolesOutcomeCallable;
      typedef std::future<GetOpenIdTokenOutcome> GetOpenIdTokenOutcomeCallable;
      typedef std::future<GetOpenIdTokenForDeveloperIdentityOutcome> GetOpenIdTokenForDeveloperIdentityOutcomeCallable;
      typedef std::future<GetPrincipalTagAttributeMapOutcome> GetPrincipalTagAttributeMapOutcomeCallable;
      typedef std::future<ListIdentitiesOutcome> ListIdentitiesOutcomeCallable;
      typedef std::future<ListIdentityPoolsOutcome> ListIdentityPoolsOutcomeCallable;
      typedef std::future<ListTagsForResourceOutcome> ListTagsForResourceOutcomeCallable;
      typedef std::future<LookupDeveloperIdentityOutcome> LookupDeveloperIdentityOutcomeCallable;
      typedef std::future<MergeDeveloperIdentitiesOutcome> MergeDeveloperIdentitiesOutcomeCallable;
      typedef std::future<SetIdentityPoolRolesOutcome> SetIdentityPoolRolesOutcomeCallable;
      typedef std::future<SetPrincipalTagAttributeMapOutcome> SetPrincipalTagAttributeMapOutcomeCallable;
      typedef std::future<TagResourceOutcome> TagResourceOutcomeCallable;
      typedef std::future<UnlinkDeveloperIdentityOutcome> UnlinkDeveloperIdentityOutcomeCallable;
      typedef std::future<UnlinkIdentityOutcome> UnlinkIdentityOutcomeCallable;
      typedef std::future<UntagResourceOutcome> UntagResourceOutcomeCallable;
      typedef std::future<UpdateIdentityPoolOutcome> UpdateIdentityPoolOutcomeCallable;
      /* End of service model Outcome callable definitions */
    } // namespace Model

    class CognitoIdentityClient;

    /* Service model async handlers definitions */
    typedef std::function<void(const CognitoIdentityClient*, const Model::CreateIdentityPoolRequest&, const Model::CreateIdentityPoolOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateIdentityPoolResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::DeleteIdentitiesRequest&, const Model::DeleteIdentitiesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteIdentitiesResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::DeleteIdentityPoolRequest&, const Model::DeleteIdentityPoolOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteIdentityPoolResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::DescribeIdentityRequest&, const Model::DescribeIdentityOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DescribeIdentityResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::DescribeIdentityPoolRequest&, const Model::DescribeIdentityPoolOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DescribeIdentityPoolResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::GetCredentialsForIdentityRequest&, const Model::GetCredentialsForIdentityOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetCredentialsForIdentityResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::GetIdRequest&, const Model::GetIdOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetIdResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::GetIdentityPoolRolesRequest&, const Model::GetIdentityPoolRolesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetIdentityPoolRolesResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::GetOpenIdTokenRequest&, const Model::GetOpenIdTokenOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetOpenIdTokenResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::GetOpenIdTokenForDeveloperIdentityRequest&, const Model::GetOpenIdTokenForDeveloperIdentityOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetOpenIdTokenForDeveloperIdentityResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::GetPrincipalTagAttributeMapRequest&, const Model::GetPrincipalTagAttributeMapOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetPrincipalTagAttributeMapResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::ListIdentitiesRequest&, const Model::ListIdentitiesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListIdentitiesResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::ListIdentityPoolsRequest&, const Model::ListIdentityPoolsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListIdentityPoolsResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::ListTagsForResourceRequest&, const Model::ListTagsForResourceOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListTagsForResourceResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::LookupDeveloperIdentityRequest&, const Model::LookupDeveloperIdentityOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > LookupDeveloperIdentityResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::MergeDeveloperIdentitiesRequest&, const Model::MergeDeveloperIdentitiesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > MergeDeveloperIdentitiesResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::SetIdentityPoolRolesRequest&, const Model::SetIdentityPoolRolesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > SetIdentityPoolRolesResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::SetPrincipalTagAttributeMapRequest&, const Model::SetPrincipalTagAttributeMapOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > SetPrincipalTagAttributeMapResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::TagResourceRequest&, const Model::TagResourceOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > TagResourceResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::UnlinkDeveloperIdentityRequest&, const Model::UnlinkDeveloperIdentityOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UnlinkDeveloperIdentityResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::UnlinkIdentityRequest&, const Model::UnlinkIdentityOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UnlinkIdentityResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::UntagResourceRequest&, const Model::UntagResourceOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UntagResourceResponseReceivedHandler;
    typedef std::function<void(const CognitoIdentityClient*, const Model::UpdateIdentityPoolRequest&, const Model::UpdateIdentityPoolOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateIdentityPoolResponseReceivedHandler;
    /* End of service model async handlers definitions */
  } // namespace CognitoIdentity
} // namespace Aws
