/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

/* Generic header includes */
#include <aws/iam/IAMErrors.h>
#include <aws/core/client/GenericClientConfiguration.h>
#include <aws/core/client/AWSError.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/client/AsyncCallerContext.h>
#include <aws/core/http/HttpTypes.h>
#include <aws/iam/IAMEndpointProvider.h>
#include <future>
#include <functional>
/* End of generic header includes */

/* Service model headers required in IAMClient header */
#include <aws/iam/model/CreateAccessKeyResult.h>
#include <aws/iam/model/CreateGroupResult.h>
#include <aws/iam/model/CreateInstanceProfileResult.h>
#include <aws/iam/model/CreateLoginProfileResult.h>
#include <aws/iam/model/CreateOpenIDConnectProviderResult.h>
#include <aws/iam/model/CreatePolicyResult.h>
#include <aws/iam/model/CreatePolicyVersionResult.h>
#include <aws/iam/model/CreateRoleResult.h>
#include <aws/iam/model/CreateSAMLProviderResult.h>
#include <aws/iam/model/CreateServiceLinkedRoleResult.h>
#include <aws/iam/model/CreateServiceSpecificCredentialResult.h>
#include <aws/iam/model/CreateUserResult.h>
#include <aws/iam/model/CreateVirtualMFADeviceResult.h>
#include <aws/iam/model/DeleteServiceLinkedRoleResult.h>
#include <aws/iam/model/DisableOrganizationsRootCredentialsManagementResult.h>
#include <aws/iam/model/DisableOrganizationsRootSessionsResult.h>
#include <aws/iam/model/EnableOrganizationsRootCredentialsManagementResult.h>
#include <aws/iam/model/EnableOrganizationsRootSessionsResult.h>
#include <aws/iam/model/GenerateCredentialReportResult.h>
#include <aws/iam/model/GenerateOrganizationsAccessReportResult.h>
#include <aws/iam/model/GenerateServiceLastAccessedDetailsResult.h>
#include <aws/iam/model/GetAccessKeyLastUsedResult.h>
#include <aws/iam/model/GetAccountAuthorizationDetailsResult.h>
#include <aws/iam/model/GetAccountPasswordPolicyResult.h>
#include <aws/iam/model/GetAccountSummaryResult.h>
#include <aws/iam/model/GetContextKeysForCustomPolicyResult.h>
#include <aws/iam/model/GetContextKeysForPrincipalPolicyResult.h>
#include <aws/iam/model/GetCredentialReportResult.h>
#include <aws/iam/model/GetGroupResult.h>
#include <aws/iam/model/GetGroupPolicyResult.h>
#include <aws/iam/model/GetInstanceProfileResult.h>
#include <aws/iam/model/GetLoginProfileResult.h>
#include <aws/iam/model/GetMFADeviceResult.h>
#include <aws/iam/model/GetOpenIDConnectProviderResult.h>
#include <aws/iam/model/GetOrganizationsAccessReportResult.h>
#include <aws/iam/model/GetPolicyResult.h>
#include <aws/iam/model/GetPolicyVersionResult.h>
#include <aws/iam/model/GetRoleResult.h>
#include <aws/iam/model/GetRolePolicyResult.h>
#include <aws/iam/model/GetSAMLProviderResult.h>
#include <aws/iam/model/GetSSHPublicKeyResult.h>
#include <aws/iam/model/GetServerCertificateResult.h>
#include <aws/iam/model/GetServiceLastAccessedDetailsResult.h>
#include <aws/iam/model/GetServiceLastAccessedDetailsWithEntitiesResult.h>
#include <aws/iam/model/GetServiceLinkedRoleDeletionStatusResult.h>
#include <aws/iam/model/GetUserResult.h>
#include <aws/iam/model/GetUserPolicyResult.h>
#include <aws/iam/model/ListAccessKeysResult.h>
#include <aws/iam/model/ListAccountAliasesResult.h>
#include <aws/iam/model/ListAttachedGroupPoliciesResult.h>
#include <aws/iam/model/ListAttachedRolePoliciesResult.h>
#include <aws/iam/model/ListAttachedUserPoliciesResult.h>
#include <aws/iam/model/ListEntitiesForPolicyResult.h>
#include <aws/iam/model/ListGroupPoliciesResult.h>
#include <aws/iam/model/ListGroupsResult.h>
#include <aws/iam/model/ListGroupsForUserResult.h>
#include <aws/iam/model/ListInstanceProfileTagsResult.h>
#include <aws/iam/model/ListInstanceProfilesResult.h>
#include <aws/iam/model/ListInstanceProfilesForRoleResult.h>
#include <aws/iam/model/ListMFADeviceTagsResult.h>
#include <aws/iam/model/ListMFADevicesResult.h>
#include <aws/iam/model/ListOpenIDConnectProviderTagsResult.h>
#include <aws/iam/model/ListOpenIDConnectProvidersResult.h>
#include <aws/iam/model/ListOrganizationsFeaturesResult.h>
#include <aws/iam/model/ListPoliciesResult.h>
#include <aws/iam/model/ListPoliciesGrantingServiceAccessResult.h>
#include <aws/iam/model/ListPolicyTagsResult.h>
#include <aws/iam/model/ListPolicyVersionsResult.h>
#include <aws/iam/model/ListRolePoliciesResult.h>
#include <aws/iam/model/ListRoleTagsResult.h>
#include <aws/iam/model/ListRolesResult.h>
#include <aws/iam/model/ListSAMLProviderTagsResult.h>
#include <aws/iam/model/ListSAMLProvidersResult.h>
#include <aws/iam/model/ListSSHPublicKeysResult.h>
#include <aws/iam/model/ListServerCertificateTagsResult.h>
#include <aws/iam/model/ListServerCertificatesResult.h>
#include <aws/iam/model/ListServiceSpecificCredentialsResult.h>
#include <aws/iam/model/ListSigningCertificatesResult.h>
#include <aws/iam/model/ListUserPoliciesResult.h>
#include <aws/iam/model/ListUserTagsResult.h>
#include <aws/iam/model/ListUsersResult.h>
#include <aws/iam/model/ListVirtualMFADevicesResult.h>
#include <aws/iam/model/ResetServiceSpecificCredentialResult.h>
#include <aws/iam/model/SimulateCustomPolicyResult.h>
#include <aws/iam/model/SimulatePrincipalPolicyResult.h>
#include <aws/iam/model/UpdateRoleResult.h>
#include <aws/iam/model/UpdateRoleDescriptionResult.h>
#include <aws/iam/model/UpdateSAMLProviderResult.h>
#include <aws/iam/model/UploadSSHPublicKeyResult.h>
#include <aws/iam/model/UploadServerCertificateResult.h>
#include <aws/iam/model/UploadSigningCertificateResult.h>
#include <aws/iam/model/UpdateAccountPasswordPolicyRequest.h>
#include <aws/iam/model/ListOpenIDConnectProvidersRequest.h>
#include <aws/iam/model/ListUsersRequest.h>
#include <aws/iam/model/ListOrganizationsFeaturesRequest.h>
#include <aws/iam/model/ListGroupsRequest.h>
#include <aws/iam/model/GetAccountAuthorizationDetailsRequest.h>
#include <aws/iam/model/DeleteAccountPasswordPolicyRequest.h>
#include <aws/iam/model/ListSigningCertificatesRequest.h>
#include <aws/iam/model/ListSSHPublicKeysRequest.h>
#include <aws/iam/model/GetLoginProfileRequest.h>
#include <aws/iam/model/ListAccountAliasesRequest.h>
#include <aws/iam/model/ListPoliciesRequest.h>
#include <aws/iam/model/ListServerCertificatesRequest.h>
#include <aws/iam/model/ListServiceSpecificCredentialsRequest.h>
#include <aws/iam/model/GenerateCredentialReportRequest.h>
#include <aws/iam/model/ListInstanceProfilesRequest.h>
#include <aws/iam/model/EnableOrganizationsRootCredentialsManagementRequest.h>
#include <aws/iam/model/ListMFADevicesRequest.h>
#include <aws/iam/model/ListRolesRequest.h>
#include <aws/iam/model/GetAccountPasswordPolicyRequest.h>
#include <aws/iam/model/GetUserRequest.h>
#include <aws/iam/model/GetAccountSummaryRequest.h>
#include <aws/iam/model/EnableOrganizationsRootSessionsRequest.h>
#include <aws/iam/model/ListAccessKeysRequest.h>
#include <aws/iam/model/GetCredentialReportRequest.h>
#include <aws/iam/model/CreateAccessKeyRequest.h>
#include <aws/iam/model/DisableOrganizationsRootCredentialsManagementRequest.h>
#include <aws/iam/model/DisableOrganizationsRootSessionsRequest.h>
#include <aws/iam/model/DeleteLoginProfileRequest.h>
#include <aws/iam/model/ListSAMLProvidersRequest.h>
#include <aws/iam/model/CreateLoginProfileRequest.h>
#include <aws/iam/model/ListVirtualMFADevicesRequest.h>
#include <aws/core/NoResult.h>
/* End of service model headers required in IAMClient header */

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

  namespace IAM
  {
    using IAMClientConfiguration = Aws::Client::GenericClientConfiguration;
    using IAMEndpointProviderBase = Aws::IAM::Endpoint::IAMEndpointProviderBase;
    using IAMEndpointProvider = Aws::IAM::Endpoint::IAMEndpointProvider;

    namespace Model
    {
      /* Service model forward declarations required in IAMClient header */
      class AddClientIDToOpenIDConnectProviderRequest;
      class AddRoleToInstanceProfileRequest;
      class AddUserToGroupRequest;
      class AttachGroupPolicyRequest;
      class AttachRolePolicyRequest;
      class AttachUserPolicyRequest;
      class ChangePasswordRequest;
      class CreateAccessKeyRequest;
      class CreateAccountAliasRequest;
      class CreateGroupRequest;
      class CreateInstanceProfileRequest;
      class CreateLoginProfileRequest;
      class CreateOpenIDConnectProviderRequest;
      class CreatePolicyRequest;
      class CreatePolicyVersionRequest;
      class CreateRoleRequest;
      class CreateSAMLProviderRequest;
      class CreateServiceLinkedRoleRequest;
      class CreateServiceSpecificCredentialRequest;
      class CreateUserRequest;
      class CreateVirtualMFADeviceRequest;
      class DeactivateMFADeviceRequest;
      class DeleteAccessKeyRequest;
      class DeleteAccountAliasRequest;
      class DeleteAccountPasswordPolicyRequest;
      class DeleteGroupRequest;
      class DeleteGroupPolicyRequest;
      class DeleteInstanceProfileRequest;
      class DeleteLoginProfileRequest;
      class DeleteOpenIDConnectProviderRequest;
      class DeletePolicyRequest;
      class DeletePolicyVersionRequest;
      class DeleteRoleRequest;
      class DeleteRolePermissionsBoundaryRequest;
      class DeleteRolePolicyRequest;
      class DeleteSAMLProviderRequest;
      class DeleteSSHPublicKeyRequest;
      class DeleteServerCertificateRequest;
      class DeleteServiceLinkedRoleRequest;
      class DeleteServiceSpecificCredentialRequest;
      class DeleteSigningCertificateRequest;
      class DeleteUserRequest;
      class DeleteUserPermissionsBoundaryRequest;
      class DeleteUserPolicyRequest;
      class DeleteVirtualMFADeviceRequest;
      class DetachGroupPolicyRequest;
      class DetachRolePolicyRequest;
      class DetachUserPolicyRequest;
      class DisableOrganizationsRootCredentialsManagementRequest;
      class DisableOrganizationsRootSessionsRequest;
      class EnableMFADeviceRequest;
      class EnableOrganizationsRootCredentialsManagementRequest;
      class EnableOrganizationsRootSessionsRequest;
      class GenerateCredentialReportRequest;
      class GenerateOrganizationsAccessReportRequest;
      class GenerateServiceLastAccessedDetailsRequest;
      class GetAccessKeyLastUsedRequest;
      class GetAccountAuthorizationDetailsRequest;
      class GetAccountPasswordPolicyRequest;
      class GetAccountSummaryRequest;
      class GetContextKeysForCustomPolicyRequest;
      class GetContextKeysForPrincipalPolicyRequest;
      class GetCredentialReportRequest;
      class GetGroupRequest;
      class GetGroupPolicyRequest;
      class GetInstanceProfileRequest;
      class GetLoginProfileRequest;
      class GetMFADeviceRequest;
      class GetOpenIDConnectProviderRequest;
      class GetOrganizationsAccessReportRequest;
      class GetPolicyRequest;
      class GetPolicyVersionRequest;
      class GetRoleRequest;
      class GetRolePolicyRequest;
      class GetSAMLProviderRequest;
      class GetSSHPublicKeyRequest;
      class GetServerCertificateRequest;
      class GetServiceLastAccessedDetailsRequest;
      class GetServiceLastAccessedDetailsWithEntitiesRequest;
      class GetServiceLinkedRoleDeletionStatusRequest;
      class GetUserRequest;
      class GetUserPolicyRequest;
      class ListAccessKeysRequest;
      class ListAccountAliasesRequest;
      class ListAttachedGroupPoliciesRequest;
      class ListAttachedRolePoliciesRequest;
      class ListAttachedUserPoliciesRequest;
      class ListEntitiesForPolicyRequest;
      class ListGroupPoliciesRequest;
      class ListGroupsRequest;
      class ListGroupsForUserRequest;
      class ListInstanceProfileTagsRequest;
      class ListInstanceProfilesRequest;
      class ListInstanceProfilesForRoleRequest;
      class ListMFADeviceTagsRequest;
      class ListMFADevicesRequest;
      class ListOpenIDConnectProviderTagsRequest;
      class ListOpenIDConnectProvidersRequest;
      class ListOrganizationsFeaturesRequest;
      class ListPoliciesRequest;
      class ListPoliciesGrantingServiceAccessRequest;
      class ListPolicyTagsRequest;
      class ListPolicyVersionsRequest;
      class ListRolePoliciesRequest;
      class ListRoleTagsRequest;
      class ListRolesRequest;
      class ListSAMLProviderTagsRequest;
      class ListSAMLProvidersRequest;
      class ListSSHPublicKeysRequest;
      class ListServerCertificateTagsRequest;
      class ListServerCertificatesRequest;
      class ListServiceSpecificCredentialsRequest;
      class ListSigningCertificatesRequest;
      class ListUserPoliciesRequest;
      class ListUserTagsRequest;
      class ListUsersRequest;
      class ListVirtualMFADevicesRequest;
      class PutGroupPolicyRequest;
      class PutRolePermissionsBoundaryRequest;
      class PutRolePolicyRequest;
      class PutUserPermissionsBoundaryRequest;
      class PutUserPolicyRequest;
      class RemoveClientIDFromOpenIDConnectProviderRequest;
      class RemoveRoleFromInstanceProfileRequest;
      class RemoveUserFromGroupRequest;
      class ResetServiceSpecificCredentialRequest;
      class ResyncMFADeviceRequest;
      class SetDefaultPolicyVersionRequest;
      class SetSecurityTokenServicePreferencesRequest;
      class SimulateCustomPolicyRequest;
      class SimulatePrincipalPolicyRequest;
      class TagInstanceProfileRequest;
      class TagMFADeviceRequest;
      class TagOpenIDConnectProviderRequest;
      class TagPolicyRequest;
      class TagRoleRequest;
      class TagSAMLProviderRequest;
      class TagServerCertificateRequest;
      class TagUserRequest;
      class UntagInstanceProfileRequest;
      class UntagMFADeviceRequest;
      class UntagOpenIDConnectProviderRequest;
      class UntagPolicyRequest;
      class UntagRoleRequest;
      class UntagSAMLProviderRequest;
      class UntagServerCertificateRequest;
      class UntagUserRequest;
      class UpdateAccessKeyRequest;
      class UpdateAccountPasswordPolicyRequest;
      class UpdateAssumeRolePolicyRequest;
      class UpdateGroupRequest;
      class UpdateLoginProfileRequest;
      class UpdateOpenIDConnectProviderThumbprintRequest;
      class UpdateRoleRequest;
      class UpdateRoleDescriptionRequest;
      class UpdateSAMLProviderRequest;
      class UpdateSSHPublicKeyRequest;
      class UpdateServerCertificateRequest;
      class UpdateServiceSpecificCredentialRequest;
      class UpdateSigningCertificateRequest;
      class UpdateUserRequest;
      class UploadSSHPublicKeyRequest;
      class UploadServerCertificateRequest;
      class UploadSigningCertificateRequest;
      /* End of service model forward declarations required in IAMClient header */

      /* Service model Outcome class definitions */
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> AddClientIDToOpenIDConnectProviderOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> AddRoleToInstanceProfileOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> AddUserToGroupOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> AttachGroupPolicyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> AttachRolePolicyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> AttachUserPolicyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> ChangePasswordOutcome;
      typedef Aws::Utils::Outcome<CreateAccessKeyResult, IAMError> CreateAccessKeyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> CreateAccountAliasOutcome;
      typedef Aws::Utils::Outcome<CreateGroupResult, IAMError> CreateGroupOutcome;
      typedef Aws::Utils::Outcome<CreateInstanceProfileResult, IAMError> CreateInstanceProfileOutcome;
      typedef Aws::Utils::Outcome<CreateLoginProfileResult, IAMError> CreateLoginProfileOutcome;
      typedef Aws::Utils::Outcome<CreateOpenIDConnectProviderResult, IAMError> CreateOpenIDConnectProviderOutcome;
      typedef Aws::Utils::Outcome<CreatePolicyResult, IAMError> CreatePolicyOutcome;
      typedef Aws::Utils::Outcome<CreatePolicyVersionResult, IAMError> CreatePolicyVersionOutcome;
      typedef Aws::Utils::Outcome<CreateRoleResult, IAMError> CreateRoleOutcome;
      typedef Aws::Utils::Outcome<CreateSAMLProviderResult, IAMError> CreateSAMLProviderOutcome;
      typedef Aws::Utils::Outcome<CreateServiceLinkedRoleResult, IAMError> CreateServiceLinkedRoleOutcome;
      typedef Aws::Utils::Outcome<CreateServiceSpecificCredentialResult, IAMError> CreateServiceSpecificCredentialOutcome;
      typedef Aws::Utils::Outcome<CreateUserResult, IAMError> CreateUserOutcome;
      typedef Aws::Utils::Outcome<CreateVirtualMFADeviceResult, IAMError> CreateVirtualMFADeviceOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeactivateMFADeviceOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeleteAccessKeyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeleteAccountAliasOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeleteAccountPasswordPolicyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeleteGroupOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeleteGroupPolicyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeleteInstanceProfileOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeleteLoginProfileOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeleteOpenIDConnectProviderOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeletePolicyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeletePolicyVersionOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeleteRoleOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeleteRolePermissionsBoundaryOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeleteRolePolicyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeleteSAMLProviderOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeleteSSHPublicKeyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeleteServerCertificateOutcome;
      typedef Aws::Utils::Outcome<DeleteServiceLinkedRoleResult, IAMError> DeleteServiceLinkedRoleOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeleteServiceSpecificCredentialOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeleteSigningCertificateOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeleteUserOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeleteUserPermissionsBoundaryOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeleteUserPolicyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DeleteVirtualMFADeviceOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DetachGroupPolicyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DetachRolePolicyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> DetachUserPolicyOutcome;
      typedef Aws::Utils::Outcome<DisableOrganizationsRootCredentialsManagementResult, IAMError> DisableOrganizationsRootCredentialsManagementOutcome;
      typedef Aws::Utils::Outcome<DisableOrganizationsRootSessionsResult, IAMError> DisableOrganizationsRootSessionsOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> EnableMFADeviceOutcome;
      typedef Aws::Utils::Outcome<EnableOrganizationsRootCredentialsManagementResult, IAMError> EnableOrganizationsRootCredentialsManagementOutcome;
      typedef Aws::Utils::Outcome<EnableOrganizationsRootSessionsResult, IAMError> EnableOrganizationsRootSessionsOutcome;
      typedef Aws::Utils::Outcome<GenerateCredentialReportResult, IAMError> GenerateCredentialReportOutcome;
      typedef Aws::Utils::Outcome<GenerateOrganizationsAccessReportResult, IAMError> GenerateOrganizationsAccessReportOutcome;
      typedef Aws::Utils::Outcome<GenerateServiceLastAccessedDetailsResult, IAMError> GenerateServiceLastAccessedDetailsOutcome;
      typedef Aws::Utils::Outcome<GetAccessKeyLastUsedResult, IAMError> GetAccessKeyLastUsedOutcome;
      typedef Aws::Utils::Outcome<GetAccountAuthorizationDetailsResult, IAMError> GetAccountAuthorizationDetailsOutcome;
      typedef Aws::Utils::Outcome<GetAccountPasswordPolicyResult, IAMError> GetAccountPasswordPolicyOutcome;
      typedef Aws::Utils::Outcome<GetAccountSummaryResult, IAMError> GetAccountSummaryOutcome;
      typedef Aws::Utils::Outcome<GetContextKeysForCustomPolicyResult, IAMError> GetContextKeysForCustomPolicyOutcome;
      typedef Aws::Utils::Outcome<GetContextKeysForPrincipalPolicyResult, IAMError> GetContextKeysForPrincipalPolicyOutcome;
      typedef Aws::Utils::Outcome<GetCredentialReportResult, IAMError> GetCredentialReportOutcome;
      typedef Aws::Utils::Outcome<GetGroupResult, IAMError> GetGroupOutcome;
      typedef Aws::Utils::Outcome<GetGroupPolicyResult, IAMError> GetGroupPolicyOutcome;
      typedef Aws::Utils::Outcome<GetInstanceProfileResult, IAMError> GetInstanceProfileOutcome;
      typedef Aws::Utils::Outcome<GetLoginProfileResult, IAMError> GetLoginProfileOutcome;
      typedef Aws::Utils::Outcome<GetMFADeviceResult, IAMError> GetMFADeviceOutcome;
      typedef Aws::Utils::Outcome<GetOpenIDConnectProviderResult, IAMError> GetOpenIDConnectProviderOutcome;
      typedef Aws::Utils::Outcome<GetOrganizationsAccessReportResult, IAMError> GetOrganizationsAccessReportOutcome;
      typedef Aws::Utils::Outcome<GetPolicyResult, IAMError> GetPolicyOutcome;
      typedef Aws::Utils::Outcome<GetPolicyVersionResult, IAMError> GetPolicyVersionOutcome;
      typedef Aws::Utils::Outcome<GetRoleResult, IAMError> GetRoleOutcome;
      typedef Aws::Utils::Outcome<GetRolePolicyResult, IAMError> GetRolePolicyOutcome;
      typedef Aws::Utils::Outcome<GetSAMLProviderResult, IAMError> GetSAMLProviderOutcome;
      typedef Aws::Utils::Outcome<GetSSHPublicKeyResult, IAMError> GetSSHPublicKeyOutcome;
      typedef Aws::Utils::Outcome<GetServerCertificateResult, IAMError> GetServerCertificateOutcome;
      typedef Aws::Utils::Outcome<GetServiceLastAccessedDetailsResult, IAMError> GetServiceLastAccessedDetailsOutcome;
      typedef Aws::Utils::Outcome<GetServiceLastAccessedDetailsWithEntitiesResult, IAMError> GetServiceLastAccessedDetailsWithEntitiesOutcome;
      typedef Aws::Utils::Outcome<GetServiceLinkedRoleDeletionStatusResult, IAMError> GetServiceLinkedRoleDeletionStatusOutcome;
      typedef Aws::Utils::Outcome<GetUserResult, IAMError> GetUserOutcome;
      typedef Aws::Utils::Outcome<GetUserPolicyResult, IAMError> GetUserPolicyOutcome;
      typedef Aws::Utils::Outcome<ListAccessKeysResult, IAMError> ListAccessKeysOutcome;
      typedef Aws::Utils::Outcome<ListAccountAliasesResult, IAMError> ListAccountAliasesOutcome;
      typedef Aws::Utils::Outcome<ListAttachedGroupPoliciesResult, IAMError> ListAttachedGroupPoliciesOutcome;
      typedef Aws::Utils::Outcome<ListAttachedRolePoliciesResult, IAMError> ListAttachedRolePoliciesOutcome;
      typedef Aws::Utils::Outcome<ListAttachedUserPoliciesResult, IAMError> ListAttachedUserPoliciesOutcome;
      typedef Aws::Utils::Outcome<ListEntitiesForPolicyResult, IAMError> ListEntitiesForPolicyOutcome;
      typedef Aws::Utils::Outcome<ListGroupPoliciesResult, IAMError> ListGroupPoliciesOutcome;
      typedef Aws::Utils::Outcome<ListGroupsResult, IAMError> ListGroupsOutcome;
      typedef Aws::Utils::Outcome<ListGroupsForUserResult, IAMError> ListGroupsForUserOutcome;
      typedef Aws::Utils::Outcome<ListInstanceProfileTagsResult, IAMError> ListInstanceProfileTagsOutcome;
      typedef Aws::Utils::Outcome<ListInstanceProfilesResult, IAMError> ListInstanceProfilesOutcome;
      typedef Aws::Utils::Outcome<ListInstanceProfilesForRoleResult, IAMError> ListInstanceProfilesForRoleOutcome;
      typedef Aws::Utils::Outcome<ListMFADeviceTagsResult, IAMError> ListMFADeviceTagsOutcome;
      typedef Aws::Utils::Outcome<ListMFADevicesResult, IAMError> ListMFADevicesOutcome;
      typedef Aws::Utils::Outcome<ListOpenIDConnectProviderTagsResult, IAMError> ListOpenIDConnectProviderTagsOutcome;
      typedef Aws::Utils::Outcome<ListOpenIDConnectProvidersResult, IAMError> ListOpenIDConnectProvidersOutcome;
      typedef Aws::Utils::Outcome<ListOrganizationsFeaturesResult, IAMError> ListOrganizationsFeaturesOutcome;
      typedef Aws::Utils::Outcome<ListPoliciesResult, IAMError> ListPoliciesOutcome;
      typedef Aws::Utils::Outcome<ListPoliciesGrantingServiceAccessResult, IAMError> ListPoliciesGrantingServiceAccessOutcome;
      typedef Aws::Utils::Outcome<ListPolicyTagsResult, IAMError> ListPolicyTagsOutcome;
      typedef Aws::Utils::Outcome<ListPolicyVersionsResult, IAMError> ListPolicyVersionsOutcome;
      typedef Aws::Utils::Outcome<ListRolePoliciesResult, IAMError> ListRolePoliciesOutcome;
      typedef Aws::Utils::Outcome<ListRoleTagsResult, IAMError> ListRoleTagsOutcome;
      typedef Aws::Utils::Outcome<ListRolesResult, IAMError> ListRolesOutcome;
      typedef Aws::Utils::Outcome<ListSAMLProviderTagsResult, IAMError> ListSAMLProviderTagsOutcome;
      typedef Aws::Utils::Outcome<ListSAMLProvidersResult, IAMError> ListSAMLProvidersOutcome;
      typedef Aws::Utils::Outcome<ListSSHPublicKeysResult, IAMError> ListSSHPublicKeysOutcome;
      typedef Aws::Utils::Outcome<ListServerCertificateTagsResult, IAMError> ListServerCertificateTagsOutcome;
      typedef Aws::Utils::Outcome<ListServerCertificatesResult, IAMError> ListServerCertificatesOutcome;
      typedef Aws::Utils::Outcome<ListServiceSpecificCredentialsResult, IAMError> ListServiceSpecificCredentialsOutcome;
      typedef Aws::Utils::Outcome<ListSigningCertificatesResult, IAMError> ListSigningCertificatesOutcome;
      typedef Aws::Utils::Outcome<ListUserPoliciesResult, IAMError> ListUserPoliciesOutcome;
      typedef Aws::Utils::Outcome<ListUserTagsResult, IAMError> ListUserTagsOutcome;
      typedef Aws::Utils::Outcome<ListUsersResult, IAMError> ListUsersOutcome;
      typedef Aws::Utils::Outcome<ListVirtualMFADevicesResult, IAMError> ListVirtualMFADevicesOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> PutGroupPolicyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> PutRolePermissionsBoundaryOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> PutRolePolicyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> PutUserPermissionsBoundaryOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> PutUserPolicyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> RemoveClientIDFromOpenIDConnectProviderOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> RemoveRoleFromInstanceProfileOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> RemoveUserFromGroupOutcome;
      typedef Aws::Utils::Outcome<ResetServiceSpecificCredentialResult, IAMError> ResetServiceSpecificCredentialOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> ResyncMFADeviceOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> SetDefaultPolicyVersionOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> SetSecurityTokenServicePreferencesOutcome;
      typedef Aws::Utils::Outcome<SimulateCustomPolicyResult, IAMError> SimulateCustomPolicyOutcome;
      typedef Aws::Utils::Outcome<SimulatePrincipalPolicyResult, IAMError> SimulatePrincipalPolicyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> TagInstanceProfileOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> TagMFADeviceOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> TagOpenIDConnectProviderOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> TagPolicyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> TagRoleOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> TagSAMLProviderOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> TagServerCertificateOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> TagUserOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> UntagInstanceProfileOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> UntagMFADeviceOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> UntagOpenIDConnectProviderOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> UntagPolicyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> UntagRoleOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> UntagSAMLProviderOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> UntagServerCertificateOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> UntagUserOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> UpdateAccessKeyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> UpdateAccountPasswordPolicyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> UpdateAssumeRolePolicyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> UpdateGroupOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> UpdateLoginProfileOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> UpdateOpenIDConnectProviderThumbprintOutcome;
      typedef Aws::Utils::Outcome<UpdateRoleResult, IAMError> UpdateRoleOutcome;
      typedef Aws::Utils::Outcome<UpdateRoleDescriptionResult, IAMError> UpdateRoleDescriptionOutcome;
      typedef Aws::Utils::Outcome<UpdateSAMLProviderResult, IAMError> UpdateSAMLProviderOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> UpdateSSHPublicKeyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> UpdateServerCertificateOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> UpdateServiceSpecificCredentialOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> UpdateSigningCertificateOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, IAMError> UpdateUserOutcome;
      typedef Aws::Utils::Outcome<UploadSSHPublicKeyResult, IAMError> UploadSSHPublicKeyOutcome;
      typedef Aws::Utils::Outcome<UploadServerCertificateResult, IAMError> UploadServerCertificateOutcome;
      typedef Aws::Utils::Outcome<UploadSigningCertificateResult, IAMError> UploadSigningCertificateOutcome;
      /* End of service model Outcome class definitions */

      /* Service model Outcome callable definitions */
      typedef std::future<AddClientIDToOpenIDConnectProviderOutcome> AddClientIDToOpenIDConnectProviderOutcomeCallable;
      typedef std::future<AddRoleToInstanceProfileOutcome> AddRoleToInstanceProfileOutcomeCallable;
      typedef std::future<AddUserToGroupOutcome> AddUserToGroupOutcomeCallable;
      typedef std::future<AttachGroupPolicyOutcome> AttachGroupPolicyOutcomeCallable;
      typedef std::future<AttachRolePolicyOutcome> AttachRolePolicyOutcomeCallable;
      typedef std::future<AttachUserPolicyOutcome> AttachUserPolicyOutcomeCallable;
      typedef std::future<ChangePasswordOutcome> ChangePasswordOutcomeCallable;
      typedef std::future<CreateAccessKeyOutcome> CreateAccessKeyOutcomeCallable;
      typedef std::future<CreateAccountAliasOutcome> CreateAccountAliasOutcomeCallable;
      typedef std::future<CreateGroupOutcome> CreateGroupOutcomeCallable;
      typedef std::future<CreateInstanceProfileOutcome> CreateInstanceProfileOutcomeCallable;
      typedef std::future<CreateLoginProfileOutcome> CreateLoginProfileOutcomeCallable;
      typedef std::future<CreateOpenIDConnectProviderOutcome> CreateOpenIDConnectProviderOutcomeCallable;
      typedef std::future<CreatePolicyOutcome> CreatePolicyOutcomeCallable;
      typedef std::future<CreatePolicyVersionOutcome> CreatePolicyVersionOutcomeCallable;
      typedef std::future<CreateRoleOutcome> CreateRoleOutcomeCallable;
      typedef std::future<CreateSAMLProviderOutcome> CreateSAMLProviderOutcomeCallable;
      typedef std::future<CreateServiceLinkedRoleOutcome> CreateServiceLinkedRoleOutcomeCallable;
      typedef std::future<CreateServiceSpecificCredentialOutcome> CreateServiceSpecificCredentialOutcomeCallable;
      typedef std::future<CreateUserOutcome> CreateUserOutcomeCallable;
      typedef std::future<CreateVirtualMFADeviceOutcome> CreateVirtualMFADeviceOutcomeCallable;
      typedef std::future<DeactivateMFADeviceOutcome> DeactivateMFADeviceOutcomeCallable;
      typedef std::future<DeleteAccessKeyOutcome> DeleteAccessKeyOutcomeCallable;
      typedef std::future<DeleteAccountAliasOutcome> DeleteAccountAliasOutcomeCallable;
      typedef std::future<DeleteAccountPasswordPolicyOutcome> DeleteAccountPasswordPolicyOutcomeCallable;
      typedef std::future<DeleteGroupOutcome> DeleteGroupOutcomeCallable;
      typedef std::future<DeleteGroupPolicyOutcome> DeleteGroupPolicyOutcomeCallable;
      typedef std::future<DeleteInstanceProfileOutcome> DeleteInstanceProfileOutcomeCallable;
      typedef std::future<DeleteLoginProfileOutcome> DeleteLoginProfileOutcomeCallable;
      typedef std::future<DeleteOpenIDConnectProviderOutcome> DeleteOpenIDConnectProviderOutcomeCallable;
      typedef std::future<DeletePolicyOutcome> DeletePolicyOutcomeCallable;
      typedef std::future<DeletePolicyVersionOutcome> DeletePolicyVersionOutcomeCallable;
      typedef std::future<DeleteRoleOutcome> DeleteRoleOutcomeCallable;
      typedef std::future<DeleteRolePermissionsBoundaryOutcome> DeleteRolePermissionsBoundaryOutcomeCallable;
      typedef std::future<DeleteRolePolicyOutcome> DeleteRolePolicyOutcomeCallable;
      typedef std::future<DeleteSAMLProviderOutcome> DeleteSAMLProviderOutcomeCallable;
      typedef std::future<DeleteSSHPublicKeyOutcome> DeleteSSHPublicKeyOutcomeCallable;
      typedef std::future<DeleteServerCertificateOutcome> DeleteServerCertificateOutcomeCallable;
      typedef std::future<DeleteServiceLinkedRoleOutcome> DeleteServiceLinkedRoleOutcomeCallable;
      typedef std::future<DeleteServiceSpecificCredentialOutcome> DeleteServiceSpecificCredentialOutcomeCallable;
      typedef std::future<DeleteSigningCertificateOutcome> DeleteSigningCertificateOutcomeCallable;
      typedef std::future<DeleteUserOutcome> DeleteUserOutcomeCallable;
      typedef std::future<DeleteUserPermissionsBoundaryOutcome> DeleteUserPermissionsBoundaryOutcomeCallable;
      typedef std::future<DeleteUserPolicyOutcome> DeleteUserPolicyOutcomeCallable;
      typedef std::future<DeleteVirtualMFADeviceOutcome> DeleteVirtualMFADeviceOutcomeCallable;
      typedef std::future<DetachGroupPolicyOutcome> DetachGroupPolicyOutcomeCallable;
      typedef std::future<DetachRolePolicyOutcome> DetachRolePolicyOutcomeCallable;
      typedef std::future<DetachUserPolicyOutcome> DetachUserPolicyOutcomeCallable;
      typedef std::future<DisableOrganizationsRootCredentialsManagementOutcome> DisableOrganizationsRootCredentialsManagementOutcomeCallable;
      typedef std::future<DisableOrganizationsRootSessionsOutcome> DisableOrganizationsRootSessionsOutcomeCallable;
      typedef std::future<EnableMFADeviceOutcome> EnableMFADeviceOutcomeCallable;
      typedef std::future<EnableOrganizationsRootCredentialsManagementOutcome> EnableOrganizationsRootCredentialsManagementOutcomeCallable;
      typedef std::future<EnableOrganizationsRootSessionsOutcome> EnableOrganizationsRootSessionsOutcomeCallable;
      typedef std::future<GenerateCredentialReportOutcome> GenerateCredentialReportOutcomeCallable;
      typedef std::future<GenerateOrganizationsAccessReportOutcome> GenerateOrganizationsAccessReportOutcomeCallable;
      typedef std::future<GenerateServiceLastAccessedDetailsOutcome> GenerateServiceLastAccessedDetailsOutcomeCallable;
      typedef std::future<GetAccessKeyLastUsedOutcome> GetAccessKeyLastUsedOutcomeCallable;
      typedef std::future<GetAccountAuthorizationDetailsOutcome> GetAccountAuthorizationDetailsOutcomeCallable;
      typedef std::future<GetAccountPasswordPolicyOutcome> GetAccountPasswordPolicyOutcomeCallable;
      typedef std::future<GetAccountSummaryOutcome> GetAccountSummaryOutcomeCallable;
      typedef std::future<GetContextKeysForCustomPolicyOutcome> GetContextKeysForCustomPolicyOutcomeCallable;
      typedef std::future<GetContextKeysForPrincipalPolicyOutcome> GetContextKeysForPrincipalPolicyOutcomeCallable;
      typedef std::future<GetCredentialReportOutcome> GetCredentialReportOutcomeCallable;
      typedef std::future<GetGroupOutcome> GetGroupOutcomeCallable;
      typedef std::future<GetGroupPolicyOutcome> GetGroupPolicyOutcomeCallable;
      typedef std::future<GetInstanceProfileOutcome> GetInstanceProfileOutcomeCallable;
      typedef std::future<GetLoginProfileOutcome> GetLoginProfileOutcomeCallable;
      typedef std::future<GetMFADeviceOutcome> GetMFADeviceOutcomeCallable;
      typedef std::future<GetOpenIDConnectProviderOutcome> GetOpenIDConnectProviderOutcomeCallable;
      typedef std::future<GetOrganizationsAccessReportOutcome> GetOrganizationsAccessReportOutcomeCallable;
      typedef std::future<GetPolicyOutcome> GetPolicyOutcomeCallable;
      typedef std::future<GetPolicyVersionOutcome> GetPolicyVersionOutcomeCallable;
      typedef std::future<GetRoleOutcome> GetRoleOutcomeCallable;
      typedef std::future<GetRolePolicyOutcome> GetRolePolicyOutcomeCallable;
      typedef std::future<GetSAMLProviderOutcome> GetSAMLProviderOutcomeCallable;
      typedef std::future<GetSSHPublicKeyOutcome> GetSSHPublicKeyOutcomeCallable;
      typedef std::future<GetServerCertificateOutcome> GetServerCertificateOutcomeCallable;
      typedef std::future<GetServiceLastAccessedDetailsOutcome> GetServiceLastAccessedDetailsOutcomeCallable;
      typedef std::future<GetServiceLastAccessedDetailsWithEntitiesOutcome> GetServiceLastAccessedDetailsWithEntitiesOutcomeCallable;
      typedef std::future<GetServiceLinkedRoleDeletionStatusOutcome> GetServiceLinkedRoleDeletionStatusOutcomeCallable;
      typedef std::future<GetUserOutcome> GetUserOutcomeCallable;
      typedef std::future<GetUserPolicyOutcome> GetUserPolicyOutcomeCallable;
      typedef std::future<ListAccessKeysOutcome> ListAccessKeysOutcomeCallable;
      typedef std::future<ListAccountAliasesOutcome> ListAccountAliasesOutcomeCallable;
      typedef std::future<ListAttachedGroupPoliciesOutcome> ListAttachedGroupPoliciesOutcomeCallable;
      typedef std::future<ListAttachedRolePoliciesOutcome> ListAttachedRolePoliciesOutcomeCallable;
      typedef std::future<ListAttachedUserPoliciesOutcome> ListAttachedUserPoliciesOutcomeCallable;
      typedef std::future<ListEntitiesForPolicyOutcome> ListEntitiesForPolicyOutcomeCallable;
      typedef std::future<ListGroupPoliciesOutcome> ListGroupPoliciesOutcomeCallable;
      typedef std::future<ListGroupsOutcome> ListGroupsOutcomeCallable;
      typedef std::future<ListGroupsForUserOutcome> ListGroupsForUserOutcomeCallable;
      typedef std::future<ListInstanceProfileTagsOutcome> ListInstanceProfileTagsOutcomeCallable;
      typedef std::future<ListInstanceProfilesOutcome> ListInstanceProfilesOutcomeCallable;
      typedef std::future<ListInstanceProfilesForRoleOutcome> ListInstanceProfilesForRoleOutcomeCallable;
      typedef std::future<ListMFADeviceTagsOutcome> ListMFADeviceTagsOutcomeCallable;
      typedef std::future<ListMFADevicesOutcome> ListMFADevicesOutcomeCallable;
      typedef std::future<ListOpenIDConnectProviderTagsOutcome> ListOpenIDConnectProviderTagsOutcomeCallable;
      typedef std::future<ListOpenIDConnectProvidersOutcome> ListOpenIDConnectProvidersOutcomeCallable;
      typedef std::future<ListOrganizationsFeaturesOutcome> ListOrganizationsFeaturesOutcomeCallable;
      typedef std::future<ListPoliciesOutcome> ListPoliciesOutcomeCallable;
      typedef std::future<ListPoliciesGrantingServiceAccessOutcome> ListPoliciesGrantingServiceAccessOutcomeCallable;
      typedef std::future<ListPolicyTagsOutcome> ListPolicyTagsOutcomeCallable;
      typedef std::future<ListPolicyVersionsOutcome> ListPolicyVersionsOutcomeCallable;
      typedef std::future<ListRolePoliciesOutcome> ListRolePoliciesOutcomeCallable;
      typedef std::future<ListRoleTagsOutcome> ListRoleTagsOutcomeCallable;
      typedef std::future<ListRolesOutcome> ListRolesOutcomeCallable;
      typedef std::future<ListSAMLProviderTagsOutcome> ListSAMLProviderTagsOutcomeCallable;
      typedef std::future<ListSAMLProvidersOutcome> ListSAMLProvidersOutcomeCallable;
      typedef std::future<ListSSHPublicKeysOutcome> ListSSHPublicKeysOutcomeCallable;
      typedef std::future<ListServerCertificateTagsOutcome> ListServerCertificateTagsOutcomeCallable;
      typedef std::future<ListServerCertificatesOutcome> ListServerCertificatesOutcomeCallable;
      typedef std::future<ListServiceSpecificCredentialsOutcome> ListServiceSpecificCredentialsOutcomeCallable;
      typedef std::future<ListSigningCertificatesOutcome> ListSigningCertificatesOutcomeCallable;
      typedef std::future<ListUserPoliciesOutcome> ListUserPoliciesOutcomeCallable;
      typedef std::future<ListUserTagsOutcome> ListUserTagsOutcomeCallable;
      typedef std::future<ListUsersOutcome> ListUsersOutcomeCallable;
      typedef std::future<ListVirtualMFADevicesOutcome> ListVirtualMFADevicesOutcomeCallable;
      typedef std::future<PutGroupPolicyOutcome> PutGroupPolicyOutcomeCallable;
      typedef std::future<PutRolePermissionsBoundaryOutcome> PutRolePermissionsBoundaryOutcomeCallable;
      typedef std::future<PutRolePolicyOutcome> PutRolePolicyOutcomeCallable;
      typedef std::future<PutUserPermissionsBoundaryOutcome> PutUserPermissionsBoundaryOutcomeCallable;
      typedef std::future<PutUserPolicyOutcome> PutUserPolicyOutcomeCallable;
      typedef std::future<RemoveClientIDFromOpenIDConnectProviderOutcome> RemoveClientIDFromOpenIDConnectProviderOutcomeCallable;
      typedef std::future<RemoveRoleFromInstanceProfileOutcome> RemoveRoleFromInstanceProfileOutcomeCallable;
      typedef std::future<RemoveUserFromGroupOutcome> RemoveUserFromGroupOutcomeCallable;
      typedef std::future<ResetServiceSpecificCredentialOutcome> ResetServiceSpecificCredentialOutcomeCallable;
      typedef std::future<ResyncMFADeviceOutcome> ResyncMFADeviceOutcomeCallable;
      typedef std::future<SetDefaultPolicyVersionOutcome> SetDefaultPolicyVersionOutcomeCallable;
      typedef std::future<SetSecurityTokenServicePreferencesOutcome> SetSecurityTokenServicePreferencesOutcomeCallable;
      typedef std::future<SimulateCustomPolicyOutcome> SimulateCustomPolicyOutcomeCallable;
      typedef std::future<SimulatePrincipalPolicyOutcome> SimulatePrincipalPolicyOutcomeCallable;
      typedef std::future<TagInstanceProfileOutcome> TagInstanceProfileOutcomeCallable;
      typedef std::future<TagMFADeviceOutcome> TagMFADeviceOutcomeCallable;
      typedef std::future<TagOpenIDConnectProviderOutcome> TagOpenIDConnectProviderOutcomeCallable;
      typedef std::future<TagPolicyOutcome> TagPolicyOutcomeCallable;
      typedef std::future<TagRoleOutcome> TagRoleOutcomeCallable;
      typedef std::future<TagSAMLProviderOutcome> TagSAMLProviderOutcomeCallable;
      typedef std::future<TagServerCertificateOutcome> TagServerCertificateOutcomeCallable;
      typedef std::future<TagUserOutcome> TagUserOutcomeCallable;
      typedef std::future<UntagInstanceProfileOutcome> UntagInstanceProfileOutcomeCallable;
      typedef std::future<UntagMFADeviceOutcome> UntagMFADeviceOutcomeCallable;
      typedef std::future<UntagOpenIDConnectProviderOutcome> UntagOpenIDConnectProviderOutcomeCallable;
      typedef std::future<UntagPolicyOutcome> UntagPolicyOutcomeCallable;
      typedef std::future<UntagRoleOutcome> UntagRoleOutcomeCallable;
      typedef std::future<UntagSAMLProviderOutcome> UntagSAMLProviderOutcomeCallable;
      typedef std::future<UntagServerCertificateOutcome> UntagServerCertificateOutcomeCallable;
      typedef std::future<UntagUserOutcome> UntagUserOutcomeCallable;
      typedef std::future<UpdateAccessKeyOutcome> UpdateAccessKeyOutcomeCallable;
      typedef std::future<UpdateAccountPasswordPolicyOutcome> UpdateAccountPasswordPolicyOutcomeCallable;
      typedef std::future<UpdateAssumeRolePolicyOutcome> UpdateAssumeRolePolicyOutcomeCallable;
      typedef std::future<UpdateGroupOutcome> UpdateGroupOutcomeCallable;
      typedef std::future<UpdateLoginProfileOutcome> UpdateLoginProfileOutcomeCallable;
      typedef std::future<UpdateOpenIDConnectProviderThumbprintOutcome> UpdateOpenIDConnectProviderThumbprintOutcomeCallable;
      typedef std::future<UpdateRoleOutcome> UpdateRoleOutcomeCallable;
      typedef std::future<UpdateRoleDescriptionOutcome> UpdateRoleDescriptionOutcomeCallable;
      typedef std::future<UpdateSAMLProviderOutcome> UpdateSAMLProviderOutcomeCallable;
      typedef std::future<UpdateSSHPublicKeyOutcome> UpdateSSHPublicKeyOutcomeCallable;
      typedef std::future<UpdateServerCertificateOutcome> UpdateServerCertificateOutcomeCallable;
      typedef std::future<UpdateServiceSpecificCredentialOutcome> UpdateServiceSpecificCredentialOutcomeCallable;
      typedef std::future<UpdateSigningCertificateOutcome> UpdateSigningCertificateOutcomeCallable;
      typedef std::future<UpdateUserOutcome> UpdateUserOutcomeCallable;
      typedef std::future<UploadSSHPublicKeyOutcome> UploadSSHPublicKeyOutcomeCallable;
      typedef std::future<UploadServerCertificateOutcome> UploadServerCertificateOutcomeCallable;
      typedef std::future<UploadSigningCertificateOutcome> UploadSigningCertificateOutcomeCallable;
      /* End of service model Outcome callable definitions */
    } // namespace Model

    class IAMClient;

    /* Service model async handlers definitions */
    typedef std::function<void(const IAMClient*, const Model::AddClientIDToOpenIDConnectProviderRequest&, const Model::AddClientIDToOpenIDConnectProviderOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > AddClientIDToOpenIDConnectProviderResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::AddRoleToInstanceProfileRequest&, const Model::AddRoleToInstanceProfileOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > AddRoleToInstanceProfileResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::AddUserToGroupRequest&, const Model::AddUserToGroupOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > AddUserToGroupResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::AttachGroupPolicyRequest&, const Model::AttachGroupPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > AttachGroupPolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::AttachRolePolicyRequest&, const Model::AttachRolePolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > AttachRolePolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::AttachUserPolicyRequest&, const Model::AttachUserPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > AttachUserPolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ChangePasswordRequest&, const Model::ChangePasswordOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ChangePasswordResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::CreateAccessKeyRequest&, const Model::CreateAccessKeyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateAccessKeyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::CreateAccountAliasRequest&, const Model::CreateAccountAliasOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateAccountAliasResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::CreateGroupRequest&, const Model::CreateGroupOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateGroupResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::CreateInstanceProfileRequest&, const Model::CreateInstanceProfileOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateInstanceProfileResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::CreateLoginProfileRequest&, const Model::CreateLoginProfileOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateLoginProfileResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::CreateOpenIDConnectProviderRequest&, const Model::CreateOpenIDConnectProviderOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateOpenIDConnectProviderResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::CreatePolicyRequest&, const Model::CreatePolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreatePolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::CreatePolicyVersionRequest&, const Model::CreatePolicyVersionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreatePolicyVersionResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::CreateRoleRequest&, const Model::CreateRoleOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateRoleResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::CreateSAMLProviderRequest&, const Model::CreateSAMLProviderOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateSAMLProviderResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::CreateServiceLinkedRoleRequest&, const Model::CreateServiceLinkedRoleOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateServiceLinkedRoleResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::CreateServiceSpecificCredentialRequest&, const Model::CreateServiceSpecificCredentialOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateServiceSpecificCredentialResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::CreateUserRequest&, const Model::CreateUserOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateUserResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::CreateVirtualMFADeviceRequest&, const Model::CreateVirtualMFADeviceOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateVirtualMFADeviceResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeactivateMFADeviceRequest&, const Model::DeactivateMFADeviceOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeactivateMFADeviceResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeleteAccessKeyRequest&, const Model::DeleteAccessKeyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteAccessKeyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeleteAccountAliasRequest&, const Model::DeleteAccountAliasOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteAccountAliasResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeleteAccountPasswordPolicyRequest&, const Model::DeleteAccountPasswordPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteAccountPasswordPolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeleteGroupRequest&, const Model::DeleteGroupOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteGroupResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeleteGroupPolicyRequest&, const Model::DeleteGroupPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteGroupPolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeleteInstanceProfileRequest&, const Model::DeleteInstanceProfileOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteInstanceProfileResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeleteLoginProfileRequest&, const Model::DeleteLoginProfileOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteLoginProfileResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeleteOpenIDConnectProviderRequest&, const Model::DeleteOpenIDConnectProviderOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteOpenIDConnectProviderResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeletePolicyRequest&, const Model::DeletePolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeletePolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeletePolicyVersionRequest&, const Model::DeletePolicyVersionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeletePolicyVersionResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeleteRoleRequest&, const Model::DeleteRoleOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteRoleResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeleteRolePermissionsBoundaryRequest&, const Model::DeleteRolePermissionsBoundaryOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteRolePermissionsBoundaryResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeleteRolePolicyRequest&, const Model::DeleteRolePolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteRolePolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeleteSAMLProviderRequest&, const Model::DeleteSAMLProviderOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteSAMLProviderResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeleteSSHPublicKeyRequest&, const Model::DeleteSSHPublicKeyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteSSHPublicKeyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeleteServerCertificateRequest&, const Model::DeleteServerCertificateOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteServerCertificateResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeleteServiceLinkedRoleRequest&, const Model::DeleteServiceLinkedRoleOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteServiceLinkedRoleResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeleteServiceSpecificCredentialRequest&, const Model::DeleteServiceSpecificCredentialOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteServiceSpecificCredentialResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeleteSigningCertificateRequest&, const Model::DeleteSigningCertificateOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteSigningCertificateResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeleteUserRequest&, const Model::DeleteUserOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteUserResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeleteUserPermissionsBoundaryRequest&, const Model::DeleteUserPermissionsBoundaryOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteUserPermissionsBoundaryResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeleteUserPolicyRequest&, const Model::DeleteUserPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteUserPolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DeleteVirtualMFADeviceRequest&, const Model::DeleteVirtualMFADeviceOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteVirtualMFADeviceResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DetachGroupPolicyRequest&, const Model::DetachGroupPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DetachGroupPolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DetachRolePolicyRequest&, const Model::DetachRolePolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DetachRolePolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DetachUserPolicyRequest&, const Model::DetachUserPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DetachUserPolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DisableOrganizationsRootCredentialsManagementRequest&, const Model::DisableOrganizationsRootCredentialsManagementOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DisableOrganizationsRootCredentialsManagementResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::DisableOrganizationsRootSessionsRequest&, const Model::DisableOrganizationsRootSessionsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DisableOrganizationsRootSessionsResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::EnableMFADeviceRequest&, const Model::EnableMFADeviceOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > EnableMFADeviceResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::EnableOrganizationsRootCredentialsManagementRequest&, const Model::EnableOrganizationsRootCredentialsManagementOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > EnableOrganizationsRootCredentialsManagementResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::EnableOrganizationsRootSessionsRequest&, const Model::EnableOrganizationsRootSessionsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > EnableOrganizationsRootSessionsResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GenerateCredentialReportRequest&, const Model::GenerateCredentialReportOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GenerateCredentialReportResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GenerateOrganizationsAccessReportRequest&, const Model::GenerateOrganizationsAccessReportOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GenerateOrganizationsAccessReportResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GenerateServiceLastAccessedDetailsRequest&, const Model::GenerateServiceLastAccessedDetailsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GenerateServiceLastAccessedDetailsResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetAccessKeyLastUsedRequest&, const Model::GetAccessKeyLastUsedOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetAccessKeyLastUsedResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetAccountAuthorizationDetailsRequest&, const Model::GetAccountAuthorizationDetailsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetAccountAuthorizationDetailsResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetAccountPasswordPolicyRequest&, const Model::GetAccountPasswordPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetAccountPasswordPolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetAccountSummaryRequest&, const Model::GetAccountSummaryOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetAccountSummaryResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetContextKeysForCustomPolicyRequest&, const Model::GetContextKeysForCustomPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetContextKeysForCustomPolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetContextKeysForPrincipalPolicyRequest&, const Model::GetContextKeysForPrincipalPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetContextKeysForPrincipalPolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetCredentialReportRequest&, const Model::GetCredentialReportOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetCredentialReportResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetGroupRequest&, const Model::GetGroupOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetGroupResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetGroupPolicyRequest&, const Model::GetGroupPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetGroupPolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetInstanceProfileRequest&, const Model::GetInstanceProfileOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetInstanceProfileResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetLoginProfileRequest&, const Model::GetLoginProfileOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetLoginProfileResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetMFADeviceRequest&, const Model::GetMFADeviceOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetMFADeviceResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetOpenIDConnectProviderRequest&, const Model::GetOpenIDConnectProviderOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetOpenIDConnectProviderResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetOrganizationsAccessReportRequest&, const Model::GetOrganizationsAccessReportOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetOrganizationsAccessReportResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetPolicyRequest&, const Model::GetPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetPolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetPolicyVersionRequest&, const Model::GetPolicyVersionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetPolicyVersionResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetRoleRequest&, const Model::GetRoleOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetRoleResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetRolePolicyRequest&, const Model::GetRolePolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetRolePolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetSAMLProviderRequest&, const Model::GetSAMLProviderOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetSAMLProviderResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetSSHPublicKeyRequest&, const Model::GetSSHPublicKeyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetSSHPublicKeyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetServerCertificateRequest&, const Model::GetServerCertificateOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetServerCertificateResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetServiceLastAccessedDetailsRequest&, const Model::GetServiceLastAccessedDetailsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetServiceLastAccessedDetailsResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetServiceLastAccessedDetailsWithEntitiesRequest&, const Model::GetServiceLastAccessedDetailsWithEntitiesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetServiceLastAccessedDetailsWithEntitiesResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetServiceLinkedRoleDeletionStatusRequest&, const Model::GetServiceLinkedRoleDeletionStatusOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetServiceLinkedRoleDeletionStatusResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetUserRequest&, const Model::GetUserOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetUserResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::GetUserPolicyRequest&, const Model::GetUserPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetUserPolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListAccessKeysRequest&, const Model::ListAccessKeysOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListAccessKeysResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListAccountAliasesRequest&, const Model::ListAccountAliasesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListAccountAliasesResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListAttachedGroupPoliciesRequest&, const Model::ListAttachedGroupPoliciesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListAttachedGroupPoliciesResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListAttachedRolePoliciesRequest&, const Model::ListAttachedRolePoliciesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListAttachedRolePoliciesResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListAttachedUserPoliciesRequest&, const Model::ListAttachedUserPoliciesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListAttachedUserPoliciesResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListEntitiesForPolicyRequest&, const Model::ListEntitiesForPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListEntitiesForPolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListGroupPoliciesRequest&, const Model::ListGroupPoliciesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListGroupPoliciesResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListGroupsRequest&, const Model::ListGroupsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListGroupsResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListGroupsForUserRequest&, const Model::ListGroupsForUserOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListGroupsForUserResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListInstanceProfileTagsRequest&, const Model::ListInstanceProfileTagsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListInstanceProfileTagsResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListInstanceProfilesRequest&, const Model::ListInstanceProfilesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListInstanceProfilesResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListInstanceProfilesForRoleRequest&, const Model::ListInstanceProfilesForRoleOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListInstanceProfilesForRoleResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListMFADeviceTagsRequest&, const Model::ListMFADeviceTagsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListMFADeviceTagsResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListMFADevicesRequest&, const Model::ListMFADevicesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListMFADevicesResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListOpenIDConnectProviderTagsRequest&, const Model::ListOpenIDConnectProviderTagsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListOpenIDConnectProviderTagsResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListOpenIDConnectProvidersRequest&, const Model::ListOpenIDConnectProvidersOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListOpenIDConnectProvidersResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListOrganizationsFeaturesRequest&, const Model::ListOrganizationsFeaturesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListOrganizationsFeaturesResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListPoliciesRequest&, const Model::ListPoliciesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListPoliciesResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListPoliciesGrantingServiceAccessRequest&, const Model::ListPoliciesGrantingServiceAccessOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListPoliciesGrantingServiceAccessResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListPolicyTagsRequest&, const Model::ListPolicyTagsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListPolicyTagsResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListPolicyVersionsRequest&, const Model::ListPolicyVersionsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListPolicyVersionsResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListRolePoliciesRequest&, const Model::ListRolePoliciesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListRolePoliciesResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListRoleTagsRequest&, const Model::ListRoleTagsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListRoleTagsResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListRolesRequest&, const Model::ListRolesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListRolesResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListSAMLProviderTagsRequest&, const Model::ListSAMLProviderTagsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListSAMLProviderTagsResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListSAMLProvidersRequest&, const Model::ListSAMLProvidersOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListSAMLProvidersResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListSSHPublicKeysRequest&, const Model::ListSSHPublicKeysOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListSSHPublicKeysResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListServerCertificateTagsRequest&, const Model::ListServerCertificateTagsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListServerCertificateTagsResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListServerCertificatesRequest&, const Model::ListServerCertificatesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListServerCertificatesResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListServiceSpecificCredentialsRequest&, const Model::ListServiceSpecificCredentialsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListServiceSpecificCredentialsResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListSigningCertificatesRequest&, const Model::ListSigningCertificatesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListSigningCertificatesResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListUserPoliciesRequest&, const Model::ListUserPoliciesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListUserPoliciesResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListUserTagsRequest&, const Model::ListUserTagsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListUserTagsResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListUsersRequest&, const Model::ListUsersOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListUsersResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ListVirtualMFADevicesRequest&, const Model::ListVirtualMFADevicesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListVirtualMFADevicesResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::PutGroupPolicyRequest&, const Model::PutGroupPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutGroupPolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::PutRolePermissionsBoundaryRequest&, const Model::PutRolePermissionsBoundaryOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutRolePermissionsBoundaryResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::PutRolePolicyRequest&, const Model::PutRolePolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutRolePolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::PutUserPermissionsBoundaryRequest&, const Model::PutUserPermissionsBoundaryOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutUserPermissionsBoundaryResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::PutUserPolicyRequest&, const Model::PutUserPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutUserPolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::RemoveClientIDFromOpenIDConnectProviderRequest&, const Model::RemoveClientIDFromOpenIDConnectProviderOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > RemoveClientIDFromOpenIDConnectProviderResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::RemoveRoleFromInstanceProfileRequest&, const Model::RemoveRoleFromInstanceProfileOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > RemoveRoleFromInstanceProfileResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::RemoveUserFromGroupRequest&, const Model::RemoveUserFromGroupOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > RemoveUserFromGroupResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ResetServiceSpecificCredentialRequest&, const Model::ResetServiceSpecificCredentialOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ResetServiceSpecificCredentialResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::ResyncMFADeviceRequest&, const Model::ResyncMFADeviceOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ResyncMFADeviceResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::SetDefaultPolicyVersionRequest&, const Model::SetDefaultPolicyVersionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > SetDefaultPolicyVersionResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::SetSecurityTokenServicePreferencesRequest&, const Model::SetSecurityTokenServicePreferencesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > SetSecurityTokenServicePreferencesResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::SimulateCustomPolicyRequest&, const Model::SimulateCustomPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > SimulateCustomPolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::SimulatePrincipalPolicyRequest&, const Model::SimulatePrincipalPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > SimulatePrincipalPolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::TagInstanceProfileRequest&, const Model::TagInstanceProfileOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > TagInstanceProfileResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::TagMFADeviceRequest&, const Model::TagMFADeviceOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > TagMFADeviceResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::TagOpenIDConnectProviderRequest&, const Model::TagOpenIDConnectProviderOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > TagOpenIDConnectProviderResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::TagPolicyRequest&, const Model::TagPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > TagPolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::TagRoleRequest&, const Model::TagRoleOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > TagRoleResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::TagSAMLProviderRequest&, const Model::TagSAMLProviderOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > TagSAMLProviderResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::TagServerCertificateRequest&, const Model::TagServerCertificateOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > TagServerCertificateResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::TagUserRequest&, const Model::TagUserOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > TagUserResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UntagInstanceProfileRequest&, const Model::UntagInstanceProfileOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UntagInstanceProfileResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UntagMFADeviceRequest&, const Model::UntagMFADeviceOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UntagMFADeviceResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UntagOpenIDConnectProviderRequest&, const Model::UntagOpenIDConnectProviderOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UntagOpenIDConnectProviderResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UntagPolicyRequest&, const Model::UntagPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UntagPolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UntagRoleRequest&, const Model::UntagRoleOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UntagRoleResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UntagSAMLProviderRequest&, const Model::UntagSAMLProviderOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UntagSAMLProviderResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UntagServerCertificateRequest&, const Model::UntagServerCertificateOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UntagServerCertificateResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UntagUserRequest&, const Model::UntagUserOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UntagUserResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UpdateAccessKeyRequest&, const Model::UpdateAccessKeyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateAccessKeyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UpdateAccountPasswordPolicyRequest&, const Model::UpdateAccountPasswordPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateAccountPasswordPolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UpdateAssumeRolePolicyRequest&, const Model::UpdateAssumeRolePolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateAssumeRolePolicyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UpdateGroupRequest&, const Model::UpdateGroupOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateGroupResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UpdateLoginProfileRequest&, const Model::UpdateLoginProfileOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateLoginProfileResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UpdateOpenIDConnectProviderThumbprintRequest&, const Model::UpdateOpenIDConnectProviderThumbprintOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateOpenIDConnectProviderThumbprintResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UpdateRoleRequest&, const Model::UpdateRoleOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateRoleResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UpdateRoleDescriptionRequest&, const Model::UpdateRoleDescriptionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateRoleDescriptionResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UpdateSAMLProviderRequest&, const Model::UpdateSAMLProviderOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateSAMLProviderResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UpdateSSHPublicKeyRequest&, const Model::UpdateSSHPublicKeyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateSSHPublicKeyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UpdateServerCertificateRequest&, const Model::UpdateServerCertificateOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateServerCertificateResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UpdateServiceSpecificCredentialRequest&, const Model::UpdateServiceSpecificCredentialOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateServiceSpecificCredentialResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UpdateSigningCertificateRequest&, const Model::UpdateSigningCertificateOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateSigningCertificateResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UpdateUserRequest&, const Model::UpdateUserOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateUserResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UploadSSHPublicKeyRequest&, const Model::UploadSSHPublicKeyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UploadSSHPublicKeyResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UploadServerCertificateRequest&, const Model::UploadServerCertificateOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UploadServerCertificateResponseReceivedHandler;
    typedef std::function<void(const IAMClient*, const Model::UploadSigningCertificateRequest&, const Model::UploadSigningCertificateOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UploadSigningCertificateResponseReceivedHandler;
    /* End of service model async handlers definitions */
  } // namespace IAM
} // namespace Aws
