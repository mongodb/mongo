/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <aws/access-management/AccessManagement_EXPORTS.h>

#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/memory/stl/AWSString.h>

#include <functional>

namespace Aws
{
namespace CognitoIdentity
{

class CognitoIdentityClient;

} // namespace CognitoIdentity;

namespace IAM
{

class IAMClient;

namespace Model
{
class Group;
class Policy;
class Role;
class User;

} // Model
} // namespace IAM

namespace AccessManagement
{

enum class QueryResult
{
    YES,
    NO,
    FAILURE
};

enum class IdentityPoolRoleBindingType
{
    AUTHENTICATED,
    UNAUTHENTICATED
};

class AWS_ACCESS_MANAGEMENT_API AccessManagementClient
{
    public:

        AccessManagementClient(std::shared_ptr< Aws::IAM::IAMClient >& iamClient, std::shared_ptr< Aws::CognitoIdentity::CognitoIdentityClient >& cognitoClient);
        ~AccessManagementClient();

        using PolicyGeneratorFunction = std::function< Aws::String(void) >;

        // Misc
        static Aws::String ExtractAccountIdFromArn(const Aws::String& arn);

        // Compound Operation API for IAM
        bool GetOrCreateGroup (const Aws::String& groupName, Aws::IAM::Model::Group& groupData);
        bool GetOrCreatePolicy(const Aws::String& policyName, const PolicyGeneratorFunction& policyGenerator, Aws::IAM::Model::Policy& policyData);
        bool GetOrCreateRole  (const Aws::String& roleName, const PolicyGeneratorFunction& assumedPolicyGenerator, Aws::IAM::Model::Role& roleData);
        bool GetOrCreateUser  (const Aws::String& userName, Aws::IAM::Model::User& userData);
        Aws::String GetAccountId();

        bool AttachPolicyToGroupIfNot(const Aws::IAM::Model::Policy& policyData, const Aws::String& groupName);
        bool AttachPolicyToRoleIfNot (const Aws::IAM::Model::Policy& policyData, const Aws::String& roleName);
        bool AttachPolicyToUserIfNot (const Aws::IAM::Model::Policy& policyData, const Aws::String& userName);

        bool AddUserToGroupIfNot(const Aws::String& userName, const Aws::String& groupName);

        bool VerifyOrCreateCredentialsFileForUser(const Aws::String& credentialsFilename, const Aws::String& userName);

        // Compound Operation API for Cognito
        bool GetOrCreateIdentityPool(const Aws::String& poolName, bool allowUnauthenticated, Aws::String& identityPoolId);

        bool BindRoleToIdentityPoolIfNot(const Aws::String& identityPoolId, const Aws::String& roleArn, IdentityPoolRoleBindingType roleKey);


        // Simple IAM API
        // State query
        QueryResult GetGroup (const Aws::String& groupName,  Aws::IAM::Model::Group&  groupData);
        QueryResult GetPolicy(const Aws::String& policyName, Aws::IAM::Model::Policy& policyData);
        QueryResult GetRole  (const Aws::String& roleName,   Aws::IAM::Model::Role&   roleData);
        QueryResult GetUser  (const Aws::String& userName,   Aws::IAM::Model::User&   userData);

        // Creation
        bool CreateGroup (const Aws::String& groupName, Aws::IAM::Model::Group& groupData);
        bool CreatePolicy(const Aws::String& policyName, const Aws::String& policyDocument, Aws::IAM::Model::Policy& policyData);
        bool CreateRole  (const Aws::String& roleName, const Aws::String& assumedPolicyDocument, Aws::IAM::Model::Role& roleData);
        bool CreateUser  (const Aws::String& userName, Aws::IAM::Model::User& userData);

        // Policy-Principal Relations
        bool AttachPolicyToGroup(const Aws::String& policyArn, const Aws::String& groupName);
        bool AttachPolicyToRole (const Aws::String& policyArn, const Aws::String& roleName);
        bool AttachPolicyToUser (const Aws::String& policyArn, const Aws::String& userName);

        bool DetachPolicyFromGroup(const Aws::String& policyArn, const Aws::String& groupName);
        bool DetachPolicyFromRole (const Aws::String& policyArn, const Aws::String& roleName);
        bool DetachPolicyFromUser (const Aws::String& policyArn, const Aws::String& userName);

        QueryResult IsPolicyAttachedToGroup(const Aws::String& policyName, const Aws::String& groupName);
        QueryResult IsPolicyAttachedToRole (const Aws::String& policyName, const Aws::String& roleName);
        QueryResult IsPolicyAttachedToUser (const Aws::String& policyName, const Aws::String& userName);

        // User-Group Relations
        QueryResult IsUserInGroup(const Aws::String& userName, const Aws::String& groupName);
        bool AddUserToGroup      (const Aws::String& userName, const Aws::String& groupName);
        bool RemoveUserFromGroup (const Aws::String& userName, const Aws::String& groupName);

        // Deletion
        bool DeleteGroup (const Aws::String& groupName);
        bool DeletePolicy(const Aws::String& policyName);
        bool DeleteRole  (const Aws::String& roleName);
        bool DeleteUser  (const Aws::String& userName);       

        bool DoesCredentialsFileExist    (const Aws::String& credentialsFilename);
        bool CreateCredentialsFileForUser(const Aws::String& credentialsFilename, const Aws::String& userName);

        //
        // Cognito integration
        QueryResult GetIdentityPool   (const Aws::String& poolName, Aws::String& identityPoolId);
        bool        CreateIdentityPool(const Aws::String& poolName, bool allowUnauthenticated, Aws::String& identityPoolId);
        bool        DeleteIdentityPool(const Aws::String& poolName);


        QueryResult IsRoleBoundToIdentityPool(const Aws::String& identityPoolId, const Aws::String& roleArn, IdentityPoolRoleBindingType roleKey);
        bool        BindRoleToIdentityPool   (const Aws::String& identityPoolId, const Aws::String& roleArn, IdentityPoolRoleBindingType roleKey);

    private:

        bool RemoveUsersFromGroup(const Aws::String& groupName);
        bool DetachPoliciesFromGroup(const Aws::String& groupName);
        bool DeleteInlinePoliciesFromGroup(const Aws::String& groupName);

        bool DeleteAccessKeysForUser(const Aws::String& userName);
        bool RemoveUserFromGroups(const Aws::String& userName);
        bool RemoveCertificatesFromUser(const Aws::String& userName);
        bool RemovePasswordFromUser(const Aws::String& userName);
        bool DeleteInlinePoliciesFromUser(const Aws::String& userName);
        bool RemoveMFAFromUser(const Aws::String& userName);
        bool DetachPoliciesFromUser(const Aws::String& userName);

        bool RemovePolicyFromEntities(const Aws::String& policyArn);

        bool RemoveRoleFromInstanceProfiles(const Aws::String& roleName);
        bool DeleteInlinePoliciesFromRole(const Aws::String& roleName);
        bool DetachPoliciesFromRole(const Aws::String& roleName);

        std::shared_ptr< Aws::IAM::IAMClient > m_iamClient;
        std::shared_ptr< Aws::CognitoIdentity::CognitoIdentityClient > m_cognitoClient;

};




} // namespace AccessManagement
} // namespace Aws
