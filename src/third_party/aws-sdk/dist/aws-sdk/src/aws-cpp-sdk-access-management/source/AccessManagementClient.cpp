/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/access-management/AccessManagementClient.h>

#include <aws/iam/IAMClient.h>

#include <aws/iam/model/AddUserToGroupRequest.h>
#include <aws/iam/model/AttachGroupPolicyRequest.h>
#include <aws/iam/model/AttachRolePolicyRequest.h>
#include <aws/iam/model/AttachUserPolicyRequest.h>
#include <aws/iam/model/CreateAccessKeyRequest.h>
#include <aws/iam/model/CreateAccessKeyResult.h>
#include <aws/iam/model/CreateGroupRequest.h>
#include <aws/iam/model/CreateGroupResult.h>
#include <aws/iam/model/CreatePolicyRequest.h>
#include <aws/iam/model/CreatePolicyResult.h>
#include <aws/iam/model/CreateRoleRequest.h>
#include <aws/iam/model/CreateRoleResult.h>
#include <aws/iam/model/CreateUserRequest.h>
#include <aws/iam/model/CreateUserResult.h>
#include <aws/iam/model/DeactivateMFADeviceRequest.h>
#include <aws/iam/model/DeleteAccessKeyRequest.h>
#include <aws/iam/model/DeleteGroupPolicyRequest.h>
#include <aws/iam/model/DeleteGroupRequest.h>
#include <aws/iam/model/DeleteLoginProfileRequest.h>
#include <aws/iam/model/DeletePolicyRequest.h>
#include <aws/iam/model/DeleteRoleRequest.h>
#include <aws/iam/model/DeleteRolePolicyRequest.h>
#include <aws/iam/model/DeleteSigningCertificateRequest.h>
#include <aws/iam/model/DeleteUserPolicyRequest.h>
#include <aws/iam/model/DeleteUserRequest.h>
#include <aws/iam/model/DetachGroupPolicyRequest.h>
#include <aws/iam/model/DetachRolePolicyRequest.h>
#include <aws/iam/model/DetachUserPolicyRequest.h>
#include <aws/iam/model/GetGroupRequest.h>
#include <aws/iam/model/GetGroupResult.h>
#include <aws/iam/model/GetLoginProfileRequest.h>
#include <aws/iam/model/GetLoginProfileResult.h>
#include <aws/iam/model/GetRoleRequest.h>
#include <aws/iam/model/GetRoleResult.h>
#include <aws/iam/model/GetUserRequest.h>
#include <aws/iam/model/GetUserResult.h>
#include <aws/iam/model/ListAccessKeysRequest.h>
#include <aws/iam/model/ListAccessKeysResult.h>
#include <aws/iam/model/ListAttachedGroupPoliciesRequest.h>
#include <aws/iam/model/ListAttachedGroupPoliciesResult.h>
#include <aws/iam/model/ListAttachedRolePoliciesRequest.h>
#include <aws/iam/model/ListAttachedRolePoliciesResult.h>
#include <aws/iam/model/ListAttachedUserPoliciesRequest.h>
#include <aws/iam/model/ListAttachedUserPoliciesResult.h>
#include <aws/iam/model/ListEntitiesForPolicyRequest.h>
#include <aws/iam/model/ListEntitiesForPolicyResult.h>
#include <aws/iam/model/ListGroupPoliciesRequest.h>
#include <aws/iam/model/ListGroupPoliciesResult.h>
#include <aws/iam/model/ListGroupsForUserRequest.h>
#include <aws/iam/model/ListGroupsForUserResult.h>
#include <aws/iam/model/ListInstanceProfilesForRoleRequest.h>
#include <aws/iam/model/ListInstanceProfilesForRoleResult.h>
#include <aws/iam/model/ListMFADevicesRequest.h>
#include <aws/iam/model/ListMFADevicesResult.h>
#include <aws/iam/model/ListPoliciesRequest.h>
#include <aws/iam/model/ListPoliciesResult.h>
#include <aws/iam/model/ListRolePoliciesRequest.h>
#include <aws/iam/model/ListRolePoliciesResult.h>
#include <aws/iam/model/ListSigningCertificatesRequest.h>
#include <aws/iam/model/ListSigningCertificatesResult.h>
#include <aws/iam/model/ListUserPoliciesRequest.h>
#include <aws/iam/model/ListUserPoliciesResult.h>
#include <aws/iam/model/RemoveRoleFromInstanceProfileRequest.h>
#include <aws/iam/model/RemoveUserFromGroupRequest.h>

#include <aws/cognito-identity/CognitoIdentityClient.h>

#include <aws/cognito-identity/model/CreateIdentityPoolRequest.h>
#include <aws/cognito-identity/model/DeleteIdentityPoolRequest.h>
#include <aws/cognito-identity/model/GetIdentityPoolRolesRequest.h>
#include <aws/cognito-identity/model/GetIdentityPoolRolesResult.h>
#include <aws/cognito-identity/model/ListIdentityPoolsRequest.h>
#include <aws/cognito-identity/model/ListIdentityPoolsResult.h>
#include <aws/cognito-identity/model/SetIdentityPoolRolesRequest.h>

#include <aws/core/utils/logging/LogMacros.h>

#include <regex>
#include <fstream>

using namespace Aws::AccessManagement;
using namespace Aws::IAM;
using namespace Aws::IAM::Model;
using namespace Aws::CognitoIdentity;
using namespace Aws::CognitoIdentity::Model;

namespace Aws
{
namespace AccessManagement
{

static const char *LOG_TAG = "AccessManagement";

AccessManagementClient::AccessManagementClient(std::shared_ptr< Aws::IAM::IAMClient >& iamClient, std::shared_ptr< Aws::CognitoIdentity::CognitoIdentityClient >& cognitoClient) :
    m_iamClient(iamClient),
    m_cognitoClient(cognitoClient)
{
}

AccessManagementClient::~AccessManagementClient()
{
}

QueryResult AccessManagementClient::GetGroup(const Aws::String& groupName, Aws::IAM::Model::Group& groupData)
{
    GetGroupRequest getGroupRequest;
    getGroupRequest.SetGroupName(groupName);

    auto outcome = m_iamClient->GetGroup(getGroupRequest);
    if (!outcome.IsSuccess())
    {
        if (outcome.GetError().GetErrorType() == IAMErrors::NO_SUCH_ENTITY)
        {
            return QueryResult::NO;
        }
        else
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "GetGroup failed for group " << groupName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            return QueryResult::FAILURE;
        }
    }
    
    groupData = outcome.GetResult().GetGroup();

    return QueryResult::YES;
}

QueryResult AccessManagementClient::GetPolicy(const Aws::String& policyName, Aws::IAM::Model::Policy& policyData)
{
    ListPoliciesRequest listPoliciesRequest;

    bool done = false;
    while(!done)
    {
        // List the policies
        auto outcome = m_iamClient->ListPolicies(listPoliciesRequest);
        if (!outcome.IsSuccess())
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "ListPolicies failed: " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            return QueryResult::FAILURE;
        }
    
        auto policyDescriptions = outcome.GetResult().GetPolicies();
        auto policyIter = std::find_if(policyDescriptions.cbegin(), policyDescriptions.cend(), [&](const Policy& desc){ return desc.GetPolicyName() == policyName; });
        if(policyIter != policyDescriptions.cend())
        {
            policyData = *policyIter;
            return QueryResult::YES;
        }

        const Aws::String &marker = outcome.GetResult().GetMarker();
        if(marker.size() > 0)
        {
            // If there are more policies in our account, keep looking
            listPoliciesRequest.SetMarker(marker);
        }
        else
        {
            // bummer, couldn't find a match, give up
            done = true;
        }
    }

    return QueryResult::NO;
}

QueryResult AccessManagementClient::GetRole(const Aws::String& roleName, Aws::IAM::Model::Role& roleData)
{
    GetRoleRequest getRoleRequest;
    getRoleRequest.SetRoleName(roleName);

    auto outcome = m_iamClient->GetRole(getRoleRequest);
    if (!outcome.IsSuccess())
    {
        if (outcome.GetError().GetErrorType() == IAMErrors::NO_SUCH_ENTITY)
        {
            return QueryResult::NO;
        }
        else
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "GetRole failed for role " << roleName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            return QueryResult::FAILURE;
        }
    }

    roleData = outcome.GetResult().GetRole();
    return QueryResult::YES;
}

QueryResult AccessManagementClient::GetUser(const Aws::String& userName, Aws::IAM::Model::User& userData)
{
    GetUserRequest getUserRequest;
    if (!userName.empty())
    {
        getUserRequest.SetUserName(userName);
    }

    auto outcome = m_iamClient->GetUser(getUserRequest);
    if (!outcome.IsSuccess())
    {
        if (outcome.GetError().GetErrorType() == IAMErrors::NO_SUCH_ENTITY)
        {
            return QueryResult::NO;
        }
        else
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "GetUser failed for user " << userName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            return QueryResult::FAILURE;
        }
    }
    
    userData = outcome.GetResult().GetUser();

    return QueryResult::YES;
}

bool AccessManagementClient::CreateGroup(const Aws::String& groupName, Aws::IAM::Model::Group& groupData)
{
    CreateGroupRequest createRequest;
    createRequest.SetGroupName(groupName);

    auto outcome = m_iamClient->CreateGroup(createRequest);
    if (!outcome.IsSuccess())
    {
        if (outcome.GetError().GetErrorType() != IAMErrors::ENTITY_ALREADY_EXISTS)
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "CreateGroup failed for group " << groupName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            return false;
        }

        return GetGroup(groupName, groupData) == QueryResult::YES;
    }

    groupData = outcome.GetResult().GetGroup();
    return true;
}

bool AccessManagementClient::CreatePolicy(const Aws::String& policyName, const Aws::String& policyDocument, Aws::IAM::Model::Policy& policyData)
{
    CreatePolicyRequest createRequest;
    createRequest.SetPolicyName(policyName);
    createRequest.SetPolicyDocument(policyDocument);

    auto outcome = m_iamClient->CreatePolicy(createRequest);
    if (!outcome.IsSuccess())
    {
        if (outcome.GetError().GetErrorType() != IAMErrors::ENTITY_ALREADY_EXISTS)
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "CreatePolicy failed for policy " << policyName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            return false;
        }

        return GetPolicy(policyName, policyData) == QueryResult::YES;
    }

    policyData = outcome.GetResult().GetPolicy();
    return true;
}

bool AccessManagementClient::CreateRole(const Aws::String& roleName, const Aws::String& assumedPolicyDocument, Aws::IAM::Model::Role& roleData)
{
    CreateRoleRequest createRoleRequest;
    createRoleRequest.SetRoleName(roleName);
    createRoleRequest.SetAssumeRolePolicyDocument(assumedPolicyDocument);

    auto outcome = m_iamClient->CreateRole(createRoleRequest);
    if (!outcome.IsSuccess())
    {
        if (outcome.GetError().GetErrorType() != IAMErrors::ENTITY_ALREADY_EXISTS)
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "CreateRole failed for role " << roleName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            return false;
        }

        return GetRole(roleName, roleData) == QueryResult::YES;
    }

    roleData = outcome.GetResult().GetRole();
    return true;
}

bool AccessManagementClient::CreateUser(const Aws::String& userName, Aws::IAM::Model::User& userData)
{
    CreateUserRequest createRequest;
    createRequest.SetUserName(userName);
    auto outcome = m_iamClient->CreateUser(createRequest);
    if (!outcome.IsSuccess())
    {
        if (outcome.GetError().GetErrorType() != IAMErrors::ENTITY_ALREADY_EXISTS)
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "CreateUser failed for user " << userName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            return false;
        }

        return GetUser(userName, userData) == QueryResult::YES;
    }

    userData = outcome.GetResult().GetUser();
    return true;
}

QueryResult AccessManagementClient::IsUserInGroup(const Aws::String& userName, const Aws::String& groupName)
{
    GetGroupRequest getGroupRequest;
    getGroupRequest.SetGroupName(groupName);

    bool done = false;
    while(!done)
    {
        // query the group users
        auto outcome = m_iamClient->GetGroup(getGroupRequest);
        if (!outcome.IsSuccess())
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "GetGroup failed for group " << groupName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            return QueryResult::FAILURE;
        }
    
        auto users = outcome.GetResult().GetUsers();
        auto userIter = std::find_if(users.cbegin(), users.cend(), [&](const User& userDesc){ return userDesc.GetUserName() == userName; });
        if(userIter != users.cend())
        {
            return QueryResult::YES;
        }

        const Aws::String &marker = outcome.GetResult().GetMarker();
        if(marker.size() > 0)
        {
            // If there are more users in our group, keep looking
            getGroupRequest.SetMarker(marker);
        }
        else
        {
            // bummer, couldn't find a match, give up
            done = true;
        }
    }

    return QueryResult::NO;
}

bool AccessManagementClient::AddUserToGroup(const Aws::String& userName, const Aws::String& groupName)
{
    AddUserToGroupRequest addUserToGroupRequest;
    addUserToGroupRequest.SetGroupName(groupName);
    addUserToGroupRequest.SetUserName(userName);

    auto outcome = m_iamClient->AddUserToGroup(addUserToGroupRequest);
    if (!outcome.IsSuccess())
    {
        AWS_LOGSTREAM_INFO(LOG_TAG, "AddUserToGroup failed for group " << groupName << " and user " << userName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
        return false;
    }
    
    return true;
}

bool AccessManagementClient::RemoveUserFromGroup(const Aws::String& userName, const Aws::String& groupName)
{
    RemoveUserFromGroupRequest removeRequest;
    removeRequest.SetGroupName(groupName);
    removeRequest.SetUserName(userName);
        
    auto outcome = m_iamClient->RemoveUserFromGroup(removeRequest);
    if(!outcome.IsSuccess())
    {
        AWS_LOGSTREAM_INFO(LOG_TAG, "RemoveUserFromGroup failed for group " << groupName << " and user " << userName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
        return false;
    }

    return true;
}

bool AccessManagementClient::AttachPolicyToGroup(const Aws::String& policyArn, const Aws::String& groupName)
{
    AttachGroupPolicyRequest attachRequest;
    attachRequest.SetGroupName(groupName);
    attachRequest.SetPolicyArn(policyArn);

    auto outcome = m_iamClient->AttachGroupPolicy(attachRequest);
    if (!outcome.IsSuccess())
    {
        AWS_LOGSTREAM_INFO(LOG_TAG, "AttachGroupPolicy failed for group " << groupName << " and policy arn " << policyArn << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
        return false;
    }
    
    return true;
}

bool AccessManagementClient::AttachPolicyToRole(const Aws::String& policyArn, const Aws::String& roleName)
{
    AttachRolePolicyRequest attachRequest;
    attachRequest.SetRoleName(roleName);
    attachRequest.SetPolicyArn(policyArn);

    auto outcome = m_iamClient->AttachRolePolicy(attachRequest);
    if (!outcome.IsSuccess())
    {
        AWS_LOGSTREAM_INFO(LOG_TAG, "AttachRolePolicy failed for role " << roleName << " and policy arn " << policyArn << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
        return false;
    }
    
    return true;
}

bool AccessManagementClient::AttachPolicyToUser(const Aws::String& policyArn, const Aws::String& userName)
{
    AttachUserPolicyRequest attachRequest;
    attachRequest.SetUserName(userName);
    attachRequest.SetPolicyArn(policyArn);

    auto outcome = m_iamClient->AttachUserPolicy(attachRequest);
    if (!outcome.IsSuccess())
    {
        AWS_LOGSTREAM_INFO(LOG_TAG, "AttachRolePolicy failed for user " << userName << " and policy arn " << policyArn << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
        return false;
    }
    
    return true;
}

bool AccessManagementClient::DetachPolicyFromGroup(const Aws::String& policyArn, const Aws::String& groupName)
{
    DetachGroupPolicyRequest detachRequest;
    detachRequest.SetGroupName(groupName);
    detachRequest.SetPolicyArn(policyArn);
        
    auto outcome = m_iamClient->DetachGroupPolicy(detachRequest);
    if (!outcome.IsSuccess())
    {
        AWS_LOGSTREAM_INFO(LOG_TAG, "DetachGroupPolicy failed for group " << groupName << " and policy arn " << policyArn << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
        return false;
    }

    return true;
}

bool AccessManagementClient::DetachPolicyFromRole(const Aws::String& policyArn, const Aws::String& roleName)
{
    DetachRolePolicyRequest detachRequest;
    detachRequest.SetRoleName(roleName);
    detachRequest.SetPolicyArn(policyArn);
        
    auto outcome = m_iamClient->DetachRolePolicy(detachRequest);
    if (!outcome.IsSuccess())
    {
        AWS_LOGSTREAM_INFO(LOG_TAG, "DetachRolePolicy failed for role " << roleName << " and policy arn " << policyArn << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
        return false;
    }

    return true;
}

bool AccessManagementClient::DetachPolicyFromUser(const Aws::String& policyArn, const Aws::String& userName)
{
    DetachUserPolicyRequest detachRequest;
    detachRequest.SetUserName(userName);
    detachRequest.SetPolicyArn(policyArn);
        
    auto outcome = m_iamClient->DetachUserPolicy(detachRequest);
    if (!outcome.IsSuccess())
    {
        AWS_LOGSTREAM_INFO(LOG_TAG, "DetachUserPolicy failed for user " << userName << " and policy arn " << policyArn << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
        return false;
    }

    return true;
}

QueryResult AccessManagementClient::IsPolicyAttachedToGroup(const Aws::String& policyName, const Aws::String& groupName)
{
    ListAttachedGroupPoliciesRequest listRequest;
    listRequest.SetGroupName(groupName);

    bool done = false;
    while(!done)
    {
        // List the policies
        ListAttachedGroupPoliciesOutcome outcome = m_iamClient->ListAttachedGroupPolicies(listRequest);
        if (!outcome.IsSuccess())
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "ListAttachedGroupPolicies failed for group " << groupName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            return QueryResult::FAILURE;
        }
    
        auto attachedPolicies = outcome.GetResult().GetAttachedPolicies();
        auto policyIter = std::find_if(attachedPolicies.cbegin(), attachedPolicies.cend(), [&](const AttachedPolicy& desc){ return desc.GetPolicyName() == policyName; });
        if(policyIter != attachedPolicies.cend())
        {
            return QueryResult::YES;
        }

        const Aws::String &marker = outcome.GetResult().GetMarker();
        if(marker.size() > 0)
        {
            // If there are more policies in our group, keep looking
            listRequest.SetMarker(marker);
        }
        else
        {
            // bummer, couldn't find a match, give up
            done = true;
        }
    }

    return QueryResult::NO;
}

QueryResult AccessManagementClient::IsPolicyAttachedToRole(const Aws::String& policyName, const Aws::String& roleName)
{
    ListAttachedRolePoliciesRequest listRequest;
    listRequest.SetRoleName(roleName);

    bool done = false;
    while(!done)
    {
        // List the policies
        ListAttachedRolePoliciesOutcome outcome = m_iamClient->ListAttachedRolePolicies(listRequest);
        if (!outcome.IsSuccess())
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "ListAttachedRolePolicies failed for role " << roleName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            return QueryResult::FAILURE;
        }
    
        auto attachedPolicies = outcome.GetResult().GetAttachedPolicies();
        auto policyIter = std::find_if(attachedPolicies.cbegin(), attachedPolicies.cend(), [&](const AttachedPolicy& desc){ return desc.GetPolicyName() == policyName; });
        if(policyIter != attachedPolicies.cend())
        {
            return QueryResult::YES;
        }

        const Aws::String &marker = outcome.GetResult().GetMarker();
        if(marker.size() > 0)
        {
            // If there are more policies in our role, keep looking
            listRequest.SetMarker(marker);
        }
        else
        {
            // bummer, couldn't find a match, give up
            done = true;
        }
    }

    return QueryResult::NO;
}

QueryResult AccessManagementClient::IsPolicyAttachedToUser(const Aws::String& policyName, const Aws::String& userName)
{
    ListAttachedUserPoliciesRequest listRequest;
    listRequest.SetUserName(userName);

    bool done = false;
    while(!done)
    {
        // List the policies
        ListAttachedUserPoliciesOutcome outcome = m_iamClient->ListAttachedUserPolicies(listRequest);
        if (!outcome.IsSuccess())
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "ListAttachedUserPolicies failed for user " << userName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            return QueryResult::FAILURE;
        }
    
        auto attachedPolicies = outcome.GetResult().GetAttachedPolicies();
        auto policyIter = std::find_if(attachedPolicies.cbegin(), attachedPolicies.cend(), [&](const AttachedPolicy& desc){ return desc.GetPolicyName() == policyName; });
        if(policyIter != attachedPolicies.cend())
        {
            return QueryResult::YES;
        }

        const Aws::String &marker = outcome.GetResult().GetMarker();
        if(marker.size() > 0)
        {
            // If there are more policies in our user, keep looking
            listRequest.SetMarker(marker);
        }
        else
        {
            // bummer, couldn't find a match, give up
            done = true;
        }
    }

    return QueryResult::NO;
}

// regex is not allocator-aware, so technically we're breaking our memory contract here (http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2011/n3254.pdf)
// Since it's internal, nothing escapes, and what gets allocated/deallocated is very small, I think that's acceptable for now
Aws::String AccessManagementClient::ExtractAccountIdFromArn(const Aws::String& arn)
{
    std::string searchTarget(arn.c_str());
    std::regex accountIdRegex("::(\\d*):");

    std::smatch arnMatchResults;
    std::regex_search(searchTarget, arnMatchResults, accountIdRegex);

    if(arnMatchResults.size() >= 2)
    {
        return arnMatchResults[1].str().c_str();
    }

    return "";
}

bool AccessManagementClient::DoesCredentialsFileExist(const Aws::String& credentialsFilename)
{
    std::ifstream credentialsFile(credentialsFilename.c_str());
    bool result = credentialsFile.good();
    credentialsFile.close();

    return result;
} 

bool AccessManagementClient::CreateCredentialsFileForUser(const Aws::String& credentialsFilename, const Aws::String& userName)
{
    CreateAccessKeyRequest createRequest;
    createRequest.SetUserName(userName);

    CreateAccessKeyOutcome outcome = m_iamClient->CreateAccessKey(createRequest);
    if (!outcome.IsSuccess())
    {
        AWS_LOGSTREAM_INFO(LOG_TAG, "CreateAccessKey failed for user " << userName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
        return false;
    }

    Aws::String access_key(outcome.GetResult().GetAccessKey().GetAccessKeyId());
    Aws::String secret_key(outcome.GetResult().GetAccessKey().GetSecretAccessKey());

    std::ofstream credentialsFile(credentialsFilename.c_str());
    credentialsFile << "[default]\n";
    credentialsFile << "aws_access_key_id=" << access_key << "\n";
    credentialsFile << "aws_secret_access_key=" << secret_key << "\n";

    credentialsFile.close();

    return true;
}

bool AccessManagementClient::RemoveUsersFromGroup(const Aws::String& groupName)
{
    Aws::Vector< Aws::String > userNames;

    GetGroupRequest groupRequest;
    groupRequest.SetGroupName(groupName);

    bool done = false;
    while(!done)
    {
        // query the group users
        GetGroupOutcome outcome = m_iamClient->GetGroup(groupRequest);
        if (!outcome.IsSuccess())
        {
            if(outcome.GetError().GetErrorType() != IAMErrors::NO_SUCH_ENTITY)
            {
                AWS_LOGSTREAM_INFO(LOG_TAG, "GetGroup failed for group " << groupName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
                return false;
            }

            return true;
        }
    
        auto users = outcome.GetResult().GetUsers();
        std::for_each(users.cbegin(), users.cend(), [&](const User& userDesc){ userNames.push_back(userDesc.GetUserName()); } );

        if(outcome.GetResult().GetIsTruncated())
        {
            groupRequest.SetMarker(outcome.GetResult().GetMarker());
        }
        else
        {
            done = true;
        }
    }

    bool success = true;

    for (uint32_t i = 0; i < userNames.size(); ++i)
    {
        success = RemoveUserFromGroup(userNames[i], groupName) && success;
    }

    return success;
}

bool AccessManagementClient::DetachPoliciesFromGroup(const Aws::String& groupName)
{
    Aws::Vector< Aws::String > policyArns;

    ListAttachedGroupPoliciesRequest listRequest;
    listRequest.SetGroupName(groupName);

    bool done = false;
    while(!done)
    {
        // query the group policies
        ListAttachedGroupPoliciesOutcome outcome = m_iamClient->ListAttachedGroupPolicies(listRequest);
        if (!outcome.IsSuccess())
        {
            if(outcome.GetError().GetErrorType() != IAMErrors::NO_SUCH_ENTITY)
            {
                AWS_LOGSTREAM_INFO(LOG_TAG, "ListAttachedGroupPolicies failed for group " << groupName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
                return false;
            }

            return true;
        }
    
        auto policies = outcome.GetResult().GetAttachedPolicies();
        std::for_each(policies.cbegin(), policies.cend(), [&](const AttachedPolicy& policy){ policyArns.push_back(policy.GetPolicyArn()); } );

        if(outcome.GetResult().GetIsTruncated())
        {
            listRequest.SetMarker(outcome.GetResult().GetMarker());
        }
        else
        {
            done = true;
        }
    }

    bool success = true;
    for (uint32_t i = 0; i < policyArns.size(); ++i)
    {
        success = DetachPolicyFromGroup(policyArns[i], groupName) && success;
    }

    return success;
}

bool AccessManagementClient::DeleteInlinePoliciesFromGroup(const Aws::String& groupName)
{
    Aws::Vector< Aws::String > policyNames;

    ListGroupPoliciesRequest listRequest;
    listRequest.SetGroupName(groupName);

    bool done = false;
    while(!done)
    {
        // query the group inline policies
        ListGroupPoliciesOutcome outcome = m_iamClient->ListGroupPolicies(listRequest);
        if (!outcome.IsSuccess())
        {
            if(outcome.GetError().GetErrorType() != IAMErrors::NO_SUCH_ENTITY)
            {
                AWS_LOGSTREAM_INFO(LOG_TAG, "ListGroupPolicies failed for group " << groupName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
                return false;
            }

            return true;
        }
    
        auto policies = outcome.GetResult().GetPolicyNames();
        std::copy(policies.begin(), policies.end(), std::back_inserter(policyNames));

        if(outcome.GetResult().GetIsTruncated())
        {
            listRequest.SetMarker(outcome.GetResult().GetMarker());
        }
        else
        {
            done = true;
        }
    }

    bool success = true;
    for (uint32_t i = 0; i < policyNames.size(); ++i)
    {
        DeleteGroupPolicyRequest deleteRequest;
        deleteRequest.SetGroupName(groupName);
        deleteRequest.SetPolicyName(policyNames[i]);
        
        DeleteGroupPolicyOutcome outcome = m_iamClient->DeleteGroupPolicy(deleteRequest);
        if (!outcome.IsSuccess())
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "DeleteGroupPolicy failed for group " << groupName << " and policy " << policyNames[i] << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            success = false;
        }
    }

    return success;
}

bool AccessManagementClient::DeleteGroup(const Aws::String& groupName)
{
    Aws::IAM::Model::Group groupDesc;
    auto result = GetGroup(groupName, groupDesc);
    if (result != QueryResult::YES) 
    {
        return result == QueryResult::NO;
    }

    if (!RemoveUsersFromGroup(groupName))
    {
        return false;     
    }

    if (!DetachPoliciesFromGroup(groupName))
    {
        return false;  
    }

    if (!DeleteInlinePoliciesFromGroup(groupName))
    {
        return false;  
    }

    DeleteGroupRequest deleteRequest;
    deleteRequest.SetGroupName(groupName.c_str());

    DeleteGroupOutcome outcome = m_iamClient->DeleteGroup(deleteRequest);
    return outcome.IsSuccess() || outcome.GetError().GetErrorType() == IAMErrors::NO_SUCH_ENTITY;
}

bool AccessManagementClient::DeleteAccessKeysForUser(const Aws::String& userName)
{
    Aws::Vector< Aws::String > accessKeys;

    ListAccessKeysRequest listRequest;
    listRequest.SetUserName(userName);

    bool done = false;
    while(!done)
    {
        // query the user access keys
        ListAccessKeysOutcome outcome = m_iamClient->ListAccessKeys(listRequest);
        if (!outcome.IsSuccess())
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "ListAccessKeys failed for user " << userName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            return false;
        }
    
        auto keys = outcome.GetResult().GetAccessKeyMetadata();
        std::for_each(keys.cbegin(), keys.cend(), [&](const AccessKeyMetadata& keyData){ accessKeys.push_back(keyData.GetAccessKeyId()); } );

        if(outcome.GetResult().GetIsTruncated())
        {
            listRequest.SetMarker(outcome.GetResult().GetMarker());
        }
        else
        {
            done = true;
        }
    }

    bool success = true;
    for (uint32_t i = 0; i < accessKeys.size(); ++i)
    {
        DeleteAccessKeyRequest deleteRequest;
        deleteRequest.SetUserName(userName);
        deleteRequest.SetAccessKeyId(accessKeys[i]);
        
        DeleteAccessKeyOutcome outcome = m_iamClient->DeleteAccessKey(deleteRequest);
        if (!outcome.IsSuccess())
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "DeleteAccessKey failed for user " << userName << " and key " << accessKeys[i] << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            success = false;
        }
    }

    return success;
}

bool AccessManagementClient::RemoveUserFromGroups(const Aws::String& userName)
{
    Aws::Vector< Aws::String > groupNames;

    ListGroupsForUserRequest listRequest;
    listRequest.SetUserName(userName);

    bool done = false;
    while(!done)
    {
        // query the user's groups
        ListGroupsForUserOutcome outcome = m_iamClient->ListGroupsForUser(listRequest);
        if (!outcome.IsSuccess())
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "ListGroupsForUser failed for user " << userName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            return false;
        }
    
        auto groups = outcome.GetResult().GetGroups();
        std::for_each(groups.cbegin(), groups.cend(), [&](const Group& groupData){ groupNames.push_back(groupData.GetGroupName()); } );

        if(outcome.GetResult().GetIsTruncated())
        {
            listRequest.SetMarker(outcome.GetResult().GetMarker());
        }
        else
        {
            done = true;
        }
    }

    bool success = true;
    for (uint32_t i = 0; i < groupNames.size(); ++i)
    {
        success = RemoveUserFromGroup(userName, groupNames[i]) && success;
    }

    return success;
}

bool AccessManagementClient::RemoveCertificatesFromUser(const Aws::String& userName)
{
    Aws::Vector< Aws::String > certificateIds;

    ListSigningCertificatesRequest listRequest;
    listRequest.SetUserName(userName);

    bool done = false;
    while(!done)
    {
        // query the user's certificates
        ListSigningCertificatesOutcome outcome = m_iamClient->ListSigningCertificates(listRequest);
        if (!outcome.IsSuccess())
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "ListSigningCertificates failed for user " << userName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            return false;
        }
    
        auto certificates = outcome.GetResult().GetCertificates();
        std::for_each(certificates.cbegin(), certificates.cend(), [&](const SigningCertificate& certificateData){ certificateIds.push_back(certificateData.GetCertificateId()); } );

        if(outcome.GetResult().GetIsTruncated())
        {
            listRequest.SetMarker(outcome.GetResult().GetMarker());
        }
        else
        {
            done = true;
        }
    }

    bool success = true;
    for (uint32_t i = 0; i < certificateIds.size(); ++i)
    {
        DeleteSigningCertificateRequest deleteRequest;
        deleteRequest.SetUserName(userName);
        deleteRequest.SetCertificateId(certificateIds[i]);
        
        DeleteSigningCertificateOutcome outcome = m_iamClient->DeleteSigningCertificate(deleteRequest);
        if (!outcome.IsSuccess())
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "DeleteSigningCertificate failed for user " << userName << " and cert " << certificateIds[i] << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            success = false;
        }
    }

    return success;
}

bool AccessManagementClient::RemovePasswordFromUser(const Aws::String& userName)
{
    GetLoginProfileRequest getRequest;
    getRequest.SetUserName(userName);

    GetLoginProfileOutcome getOutcome = m_iamClient->GetLoginProfile(getRequest);
    if (!getOutcome.IsSuccess())
    {
        auto errorType = getOutcome.GetError().GetErrorType();
        if (errorType != IAMErrors::NO_SUCH_ENTITY)
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "GetLoginProfile failed for user " << userName << ": " << getOutcome.GetError().GetMessage() << " ( " << getOutcome.GetError().GetExceptionName() << " )\n");
        }

        return errorType == IAMErrors::NO_SUCH_ENTITY;
    }

    DeleteLoginProfileRequest deleteRequest;
    deleteRequest.SetUserName(userName);
        
    DeleteLoginProfileOutcome deleteOutcome = m_iamClient->DeleteLoginProfile(deleteRequest);
    if (!deleteOutcome.IsSuccess())
    {
        AWS_LOGSTREAM_INFO(LOG_TAG, "DeleteLoginProfile failed for user " << userName << ": " << deleteOutcome.GetError().GetMessage() << " ( " << deleteOutcome.GetError().GetExceptionName() << " )\n");
        return false;
    }

    return true;
}

bool AccessManagementClient::DeleteInlinePoliciesFromUser(const Aws::String& userName)
{
    Aws::Vector< Aws::String > policyNames;

    ListUserPoliciesRequest listRequest;
    listRequest.SetUserName(userName);

    bool done = false;
    while(!done)
    {
        ListUserPoliciesOutcome outcome = m_iamClient->ListUserPolicies(listRequest);
        if (!outcome.IsSuccess())
        {
            if(outcome.GetError().GetErrorType() != IAMErrors::NO_SUCH_ENTITY)
            {
                AWS_LOGSTREAM_INFO(LOG_TAG, "ListUserPolicies failed for user " << userName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
                return false;
            }

            return true;
        }
    
        auto policies = outcome.GetResult().GetPolicyNames();
        std::copy(policies.begin(), policies.end(), std::back_inserter(policyNames));

        if(outcome.GetResult().GetIsTruncated())
        {
            listRequest.SetMarker(outcome.GetResult().GetMarker());
        }
        else
        {
            done = true;
        }
    }

    bool success = true;
    for (uint32_t i = 0; i < policyNames.size(); ++i)
    {
        DeleteUserPolicyRequest deleteRequest;
        deleteRequest.SetUserName(userName);
        deleteRequest.SetPolicyName(policyNames[i]);
        
        DeleteUserPolicyOutcome outcome = m_iamClient->DeleteUserPolicy(deleteRequest);
        if (!outcome.IsSuccess())
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "DeleteUserPolicy failed for user " << userName << " and policy " << policyNames[i] << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            success = false;
        }
    }

    return success;
}

bool AccessManagementClient::RemoveMFAFromUser(const Aws::String& userName)
{
    Aws::Vector< Aws::String > serialNumbers;

    ListMFADevicesRequest listRequest;
    listRequest.SetUserName(userName);

    bool done = false;
    while(!done)
    {
        // query the user mfa devices
        ListMFADevicesOutcome outcome = m_iamClient->ListMFADevices(listRequest);
        if (!outcome.IsSuccess())
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "ListMFADevices failed for user " << userName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            return false;
        }
    
        auto devices = outcome.GetResult().GetMFADevices();
        std::for_each(devices.cbegin(), devices.cend(), [&](const MFADevice& deviceData){ serialNumbers.push_back(deviceData.GetSerialNumber()); } );

        if(outcome.GetResult().GetIsTruncated())
        {
            listRequest.SetMarker(outcome.GetResult().GetMarker());
        }
        else
        {
            done = true;
        }
    }

    bool success = true;
    for (uint32_t i = 0; i < serialNumbers.size(); ++i)
    {
        DeactivateMFADeviceRequest deactivateRequest;
        deactivateRequest.SetUserName(userName);
        deactivateRequest.SetSerialNumber(serialNumbers[i]);
        
        DeactivateMFADeviceOutcome outcome = m_iamClient->DeactivateMFADevice(deactivateRequest);
        if (!outcome.IsSuccess())
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "DeactivateMFADevice failed for user " << userName << " and device " << serialNumbers[i] << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            success = false;
        }
    }

    return success;
}

bool AccessManagementClient::DetachPoliciesFromUser(const Aws::String& userName)
{
    Aws::Vector< Aws::String > policyArns;

    ListAttachedUserPoliciesRequest listRequest;
    listRequest.SetUserName(userName);

    bool done = false;
    while(!done)
    {
        // query the group attached policies
        ListAttachedUserPoliciesOutcome outcome = m_iamClient->ListAttachedUserPolicies(listRequest);
        if (!outcome.IsSuccess())
        {
            if(outcome.GetError().GetErrorType() != IAMErrors::NO_SUCH_ENTITY)
            {
                AWS_LOGSTREAM_INFO(LOG_TAG, "ListAttachedUserPolicies failed for user " << userName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
                return false;
            }

            return true;
        }
    
        auto policies = outcome.GetResult().GetAttachedPolicies();
        std::for_each(policies.cbegin(), policies.cend(), [&](const AttachedPolicy& policy){ policyArns.push_back(policy.GetPolicyArn()); } );

        if(outcome.GetResult().GetIsTruncated())
        {
            listRequest.SetMarker(outcome.GetResult().GetMarker());
        }
        else
        {
            done = true;
        }
    }

    bool success = true;
    for (uint32_t i = 0; i < policyArns.size(); ++i)
    {
        success = DetachPolicyFromUser(policyArns[i], userName) && success;
    }

    return success;
}

bool AccessManagementClient::DeleteUser(const Aws::String& userName)
{
    Aws::IAM::Model::User userDesc;
    auto result = GetUser(userName, userDesc);
    if (result != QueryResult::YES)
    {
        return result == QueryResult::NO;
    }

    if (!DeleteAccessKeysForUser(userName))
    {
        return false;
    }

    if (!DetachPoliciesFromUser(userName))
    {
        return false;
    }

    if (!DeleteInlinePoliciesFromUser(userName))
    {
        return false;
    }
        
    if (!RemoveMFAFromUser(userName))
    {
        return false;
    }    

    if (!RemovePasswordFromUser(userName))
    {
        return false;
    }

    if (!RemoveCertificatesFromUser(userName))
    {
        return false;
    }

    if (!RemoveUserFromGroups(userName))
    {
        return false;
    }

    DeleteUserRequest deleteRequest;
    deleteRequest.SetUserName(userName.c_str());

    DeleteUserOutcome outcome = m_iamClient->DeleteUser(deleteRequest);
    return outcome.IsSuccess() || outcome.GetError().GetErrorType() == IAMErrors::NO_SUCH_ENTITY;
}

bool AccessManagementClient::RemovePolicyFromEntities(const Aws::String& policyArn)
{
    Aws::Vector< Aws::String > groupNames;
    Aws::Vector< Aws::String > roleNames;
    Aws::Vector< Aws::String > userNames;

    ListEntitiesForPolicyRequest listRequest;
    listRequest.SetPolicyArn(policyArn);

    bool done = false;
    while(!done)
    {
        ListEntitiesForPolicyOutcome outcome = m_iamClient->ListEntitiesForPolicy(listRequest);
        if (!outcome.IsSuccess())
        {
            if(outcome.GetError().GetErrorType() != IAMErrors::NO_SUCH_ENTITY)
            {
                AWS_LOGSTREAM_INFO(LOG_TAG, "ListEntitiesForPolicy failed for policy arn " << policyArn << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
                return false;
            }

            return true;
        }
    
        auto groups = outcome.GetResult().GetPolicyGroups();
        std::for_each(groups.cbegin(), groups.cend(), [&](const PolicyGroup& groupData){ groupNames.push_back(groupData.GetGroupName()); } );

        auto roles = outcome.GetResult().GetPolicyRoles();
        std::for_each(roles.cbegin(), roles.cend(), [&](const PolicyRole& roleData){ roleNames.push_back(roleData.GetRoleName()); } );

        auto users = outcome.GetResult().GetPolicyUsers();
        std::for_each(users.cbegin(), users.cend(), [&](const PolicyUser& userData){ userNames.push_back(userData.GetUserName()); } );

        if(outcome.GetResult().GetIsTruncated())
        {
            listRequest.SetMarker(outcome.GetResult().GetMarker());
        }
        else
        {
            done = true;
        }
    }

    bool success = true;
    for (uint32_t i = 0; i < groupNames.size(); ++i)
    {
        success = DetachPolicyFromGroup(policyArn, groupNames[i]) && success;
    }

    for (uint32_t i = 0; i < roleNames.size(); ++i)
    {
        success = DetachPolicyFromRole(policyArn, roleNames[i]) && success;
    }

    for (uint32_t i = 0; i < userNames.size(); ++i)
    {
        success = DetachPolicyFromUser(policyArn, userNames[i]) && success;
    }

    return success;
}

bool AccessManagementClient::DeletePolicy(const Aws::String& policyName)
{
    Aws::IAM::Model::Policy policyDesc;
    auto result = GetPolicy(policyName, policyDesc);
    if (result != QueryResult::YES)
    {
        return result == QueryResult::NO;
    }

    if (!RemovePolicyFromEntities(policyDesc.GetArn()))
    {
        return false;
    }

    DeletePolicyRequest deleteRequest;
    deleteRequest.SetPolicyArn(policyDesc.GetArn());

    DeletePolicyOutcome outcome = m_iamClient->DeletePolicy(deleteRequest);
    return outcome.IsSuccess() || outcome.GetError().GetErrorType() == IAMErrors::NO_SUCH_ENTITY;
}

bool AccessManagementClient::RemoveRoleFromInstanceProfiles(const Aws::String& roleName)
{
    Aws::Vector< Aws::String > profileNames;

    ListInstanceProfilesForRoleRequest listRequest;
    listRequest.SetRoleName(roleName);

    bool done = false;
    while(!done)
    {
        // query the role's instance profiles
         ListInstanceProfilesForRoleOutcome outcome = m_iamClient-> ListInstanceProfilesForRole(listRequest);
        if (!outcome.IsSuccess())
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "ListInstanceProfilesForRole failed for role " << roleName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            return false;
        }
    
        auto profiles = outcome.GetResult().GetInstanceProfiles();
        std::for_each(profiles.cbegin(), profiles.cend(), [&](const InstanceProfile& profileData){ profileNames.push_back(profileData.GetInstanceProfileName()); } );

        if(outcome.GetResult().GetIsTruncated())
        {
            listRequest.SetMarker(outcome.GetResult().GetMarker());
        }
        else
        {
            done = true;
        }
    }

    bool success = true;
    for (uint32_t i = 0; i < profileNames.size(); ++i)
    {
        RemoveRoleFromInstanceProfileRequest removeRequest;
        removeRequest.SetRoleName(roleName);
        removeRequest.SetInstanceProfileName(profileNames[i]);
        
        RemoveRoleFromInstanceProfileOutcome outcome = m_iamClient->RemoveRoleFromInstanceProfile(removeRequest);
        if (!outcome.IsSuccess())
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "RemoveRoleFromInstanceProfile failed for role " << roleName << " and profile " << profileNames[i] << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            success = false;
        }
    }

    return success;
}

bool AccessManagementClient::DeleteInlinePoliciesFromRole(const Aws::String& roleName)
{
    Aws::Vector< Aws::String > policyNames;

    ListRolePoliciesRequest listRequest;
    listRequest.SetRoleName(roleName);

    bool done = false;
    while(!done)
    {
        // query the role's inline policies
        ListRolePoliciesOutcome outcome = m_iamClient->ListRolePolicies(listRequest);
        if (!outcome.IsSuccess())
        {
            if(outcome.GetError().GetErrorType() != IAMErrors::NO_SUCH_ENTITY)
            {
                AWS_LOGSTREAM_INFO(LOG_TAG, "ListRolePolicies failed for role " << roleName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
                return false;
            }

            return true;
        }
    
        auto policies = outcome.GetResult().GetPolicyNames();
        std::copy(policies.begin(), policies.end(), std::back_inserter(policyNames));

        if(outcome.GetResult().GetIsTruncated())
        {
            listRequest.SetMarker(outcome.GetResult().GetMarker());
        }
        else
        {
            done = true;
        }
    }

    bool success = true;
    for (uint32_t i = 0; i < policyNames.size(); ++i)
    {
        DeleteRolePolicyRequest deleteRequest;
        deleteRequest.SetRoleName(roleName);
        deleteRequest.SetPolicyName(policyNames[i]);
        
        DeleteRolePolicyOutcome outcome = m_iamClient->DeleteRolePolicy(deleteRequest);
        if (!outcome.IsSuccess())
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "DeleteRolePolicy failed for role " << roleName << " and policy " << policyNames[i] << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            success = false;
        }
    }

    return success;
}

bool AccessManagementClient::DetachPoliciesFromRole(const Aws::String& roleName)
{
    Aws::Vector< Aws::String > policyArns;

    ListAttachedRolePoliciesRequest listRequest;
    listRequest.SetRoleName(roleName);

    bool done = false;
    while(!done)
    {
        // query the role's attached policies
        ListAttachedRolePoliciesOutcome outcome = m_iamClient->ListAttachedRolePolicies(listRequest);
        if (!outcome.IsSuccess())
        {
            if(outcome.GetError().GetErrorType() != IAMErrors::NO_SUCH_ENTITY)
            {
                AWS_LOGSTREAM_INFO(LOG_TAG, "ListAttachedRolePolicies failed for role " << roleName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
                return false;
            }

            return true;
        }
    
        auto policies = outcome.GetResult().GetAttachedPolicies();
        std::for_each(policies.cbegin(), policies.cend(), [&](const AttachedPolicy& policy){ policyArns.push_back(policy.GetPolicyArn()); } );

        if(outcome.GetResult().GetIsTruncated())
        {
            listRequest.SetMarker(outcome.GetResult().GetMarker());
        }
        else
        {
            done = true;
        }
    }

    bool success = true;
    for (uint32_t i = 0; i < policyArns.size(); ++i)
    {
        success = DetachPolicyFromRole(policyArns[i], roleName) && success;
    }

    return success;
}

bool AccessManagementClient::DeleteRole(const Aws::String& roleName)
{
    Aws::IAM::Model::Role roleDesc;
    auto result = GetRole(roleName, roleDesc);
    if (result != QueryResult::YES)
    {
        return result == QueryResult::NO;
    }

    if (!RemoveRoleFromInstanceProfiles(roleName))
    {
        return false;
    }

    if (!DeleteInlinePoliciesFromRole(roleName))
    {
        return false;
    }

    if (!DetachPoliciesFromRole(roleName))
    {
        return false;
    }

    DeleteRoleRequest deleteRequest;
    deleteRequest.SetRoleName(roleName.c_str());

    DeleteRoleOutcome outcome = m_iamClient->DeleteRole(deleteRequest);
    return outcome.IsSuccess() || outcome.GetError().GetErrorType() == IAMErrors::NO_SUCH_ENTITY;
}

bool AccessManagementClient::GetOrCreateGroup(const Aws::String& groupName, Aws::IAM::Model::Group& groupData)
{
    auto result = GetGroup(groupName, groupData);
    switch(result)
    {
        case QueryResult::YES:
            return true;

        case QueryResult::FAILURE:
            return false;

        case QueryResult::NO:
            return CreateGroup(groupName, groupData);
    }

    return false;
}

bool AccessManagementClient::GetOrCreatePolicy(const Aws::String& policyName, const PolicyGeneratorFunction& policyGenerator, Aws::IAM::Model::Policy& policyData)
{
    auto result = GetPolicy(policyName, policyData);
    switch(result)
    {
        case QueryResult::YES:
            return true;

        case QueryResult::FAILURE:
            return false;

        case QueryResult::NO:
            return CreatePolicy(policyName, policyGenerator(), policyData);
    }

    return false;
}

bool AccessManagementClient::GetOrCreateRole(const Aws::String& roleName, const PolicyGeneratorFunction& assumedPolicyGenerator, Aws::IAM::Model::Role& roleData)
{
    auto result = GetRole(roleName, roleData);
    switch(result)
    {
        case QueryResult::YES:
            return true;

        case QueryResult::FAILURE:
            return false;

        case QueryResult::NO:
            return CreateRole(roleName, assumedPolicyGenerator(), roleData);
    }

    return false;
}

bool AccessManagementClient::GetOrCreateUser(const Aws::String& userName, Aws::IAM::Model::User& userData)
{
    auto result = GetUser(userName, userData);
    switch(result)
    {
        case QueryResult::YES:
            return true;

        case QueryResult::FAILURE:
            return false;

        case QueryResult::NO:
            return CreateUser(userName, userData);
    }

    return false;
}

Aws::String AccessManagementClient::GetAccountId()
{
    GetUserRequest getUserRequest;  

    auto outcome = m_iamClient->GetUser(getUserRequest);
    
    if (outcome.IsSuccess())
    {
        return ExtractAccountIdFromArn(outcome.GetResult().GetUser().GetArn());
    }
    else if (outcome.GetError().GetErrorType() == IAM::IAMErrors::ACCESS_DENIED)
    {
        return ExtractAccountIdFromArn(outcome.GetError().GetMessage());
    }
    
    return "";
}

bool AccessManagementClient::AttachPolicyToGroupIfNot(const Policy& policyData, const Aws::String& groupName)
{
    auto result = IsPolicyAttachedToGroup(policyData.GetPolicyName(), groupName);
    switch(result)
    {
        case QueryResult::YES:
            return true;

        case QueryResult::FAILURE:
            return false;

        case QueryResult::NO:
            return AttachPolicyToGroup(policyData.GetArn(), groupName);
    }

    return false;
}

bool AccessManagementClient::AttachPolicyToRoleIfNot(const Policy& policyData, const Aws::String& roleName)
{
    auto result = IsPolicyAttachedToRole(policyData.GetPolicyName(), roleName);
    switch(result)
    {
        case QueryResult::YES:
            return true;

        case QueryResult::FAILURE:
            return false;

        case QueryResult::NO:
            return AttachPolicyToRole(policyData.GetArn(), roleName);
    }

    return false;
}

bool AccessManagementClient::AttachPolicyToUserIfNot (const Policy& policyData, const Aws::String& userName)
{
    auto result = IsPolicyAttachedToUser(policyData.GetPolicyName(), userName);
    switch(result)
    {
        case QueryResult::YES:
            return true;

        case QueryResult::FAILURE:
            return false;

        case QueryResult::NO:
            return AttachPolicyToUser(policyData.GetArn(), userName);
    }

    return false;
}

bool AccessManagementClient::AddUserToGroupIfNot(const Aws::String& userName, const Aws::String& groupName)
{
    auto result = IsUserInGroup(userName, groupName);
    switch(result)
    {
        case QueryResult::YES:
            return true;

        case QueryResult::FAILURE:
            return false;

        case QueryResult::NO:
            return AddUserToGroup(userName, groupName);
    }

    return false;
}

bool AccessManagementClient::VerifyOrCreateCredentialsFileForUser(const Aws::String& credentialsFilename, const Aws::String& userName)
{
    if (DoesCredentialsFileExist(credentialsFilename))
    {
        return true;
    }

    return CreateCredentialsFileForUser(credentialsFilename, userName);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

QueryResult AccessManagementClient::GetIdentityPool(const Aws::String& poolName, Aws::String& identityPoolId)
{    
    static const uint32_t MAX_RESULTS_IDENTITY_POOLS = 20;

    ListIdentityPoolsRequest listPoolsRequest;
    listPoolsRequest.SetMaxResults(MAX_RESULTS_IDENTITY_POOLS);

    bool done = false;
    while(!done)
    {
        ListIdentityPoolsOutcome outcome = m_cognitoClient->ListIdentityPools(listPoolsRequest);
        if (!outcome.IsSuccess())
        {
            AWS_LOGSTREAM_INFO(LOG_TAG, "ListIdentityPools failed: " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
            return QueryResult::FAILURE;
        }
    
        auto pools = outcome.GetResult().GetIdentityPools();
        auto poolIter = std::find_if(pools.cbegin(), pools.cend(), [&](const IdentityPoolShortDescription& poolDesc){ 
                                                                        return poolDesc.GetIdentityPoolName() == poolName; });
        if(poolIter != pools.cend())
        {
            identityPoolId.assign(poolIter->GetIdentityPoolId());
            return QueryResult::YES;
        }

        const Aws::String &nextToken = outcome.GetResult().GetNextToken();
        if(nextToken.size() > 0)
        {
            // If there are more pools in our account, keep looking
            listPoolsRequest.SetNextToken(nextToken);
        }
        else
        {
            // bummer, couldn't find a match, give up
            done = true;
        }
    }

    return QueryResult::NO;
}

bool AccessManagementClient::CreateIdentityPool(const Aws::String& poolName, bool allowUnauthenticated, Aws::String& identityPoolId)
{
    CreateIdentityPoolRequest createPoolRequest;
    createPoolRequest.SetIdentityPoolName(poolName);
    createPoolRequest.SetAllowUnauthenticatedIdentities(allowUnauthenticated);

    CreateIdentityPoolOutcome outcome = m_cognitoClient->CreateIdentityPool(createPoolRequest);
    if (!outcome.IsSuccess())
    {
        AWS_LOGSTREAM_INFO(LOG_TAG, "CreateIdentityPool failed for pool " << poolName << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
        return false;
    }
    
    identityPoolId.assign(outcome.GetResult().GetIdentityPoolId());
    return true;    
}

bool AccessManagementClient::DeleteIdentityPool(const Aws::String& poolName)
{
    Aws::String poolId;
    auto result = GetIdentityPool(poolName, poolId);
    if (result != QueryResult::YES)
    {
        return result == QueryResult::NO;
    }

    DeleteIdentityPoolRequest deleteRequest;
    deleteRequest.SetIdentityPoolId(poolId.c_str());

    DeleteIdentityPoolOutcome outcome = m_cognitoClient->DeleteIdentityPool(deleteRequest);
    return outcome.IsSuccess() || outcome.GetError().GetErrorType() == CognitoIdentityErrors::RESOURCE_NOT_FOUND;
}

static const char* ConvertRoleBindingToMapKey(IdentityPoolRoleBindingType roleKey)
{
    switch(roleKey)
    {
        case IdentityPoolRoleBindingType::AUTHENTICATED:
            return "authenticated";

        case IdentityPoolRoleBindingType::UNAUTHENTICATED:
            return "unauthenticated";
    }

    return "";
}

QueryResult AccessManagementClient::IsRoleBoundToIdentityPool(const Aws::String& identityPoolId, 
                                                              const Aws::String& roleArn,
                                                              IdentityPoolRoleBindingType roleKey)
{
    GetIdentityPoolRolesRequest getRequest;
    getRequest.SetIdentityPoolId(identityPoolId);

    GetIdentityPoolRolesOutcome outcome = m_cognitoClient->GetIdentityPoolRoles(getRequest);
    if (!outcome.IsSuccess())
    {
        AWS_LOGSTREAM_INFO(LOG_TAG, "GetIdentityPoolRoles failed for pool " << identityPoolId << ": " << outcome.GetError().GetMessage() << " ( " << outcome.GetError().GetExceptionName() << " )\n");
        return QueryResult::FAILURE;
    }
    
    auto roleMap = outcome.GetResult().GetRoles();
    auto mapIter = roleMap.find(ConvertRoleBindingToMapKey(roleKey));
    
    if ( mapIter != roleMap.end() && mapIter->second == roleArn )
    {
        return QueryResult::YES;
    }
    else
    {
        return QueryResult::NO;
    }   
}

bool AccessManagementClient::BindRoleToIdentityPool(const Aws::String& identityPoolId, 
                                                    const Aws::String& roleArn,
                                                    IdentityPoolRoleBindingType roleKey)
{
    GetIdentityPoolRolesRequest getRequest;
    getRequest.SetIdentityPoolId(identityPoolId);

    GetIdentityPoolRolesOutcome getOutcome = m_cognitoClient->GetIdentityPoolRoles(getRequest);
    if (!getOutcome.IsSuccess())
    {
        AWS_LOGSTREAM_INFO(LOG_TAG, "GetIdentityPoolRoles failed for pool " << identityPoolId << ": " << getOutcome.GetError().GetMessage() << " ( " << getOutcome.GetError().GetExceptionName() << " )\n");
        return false;
    }

    SetIdentityPoolRolesRequest setRequest;
    setRequest.SetIdentityPoolId(identityPoolId);
    setRequest.SetRoles(getOutcome.GetResult().GetRoles());
    setRequest.AddRoles(ConvertRoleBindingToMapKey(roleKey), roleArn);

    SetIdentityPoolRolesOutcome setOutcome = m_cognitoClient->SetIdentityPoolRoles(setRequest);
    if (!setOutcome.IsSuccess())
    {
        AWS_LOGSTREAM_INFO(LOG_TAG, "SetIdentityPoolRoles failed for pool " << identityPoolId << ": " << setOutcome.GetError().GetMessage() << " ( " << setOutcome.GetError().GetExceptionName() << " )\n");
        return false;
    }

    return true;
}

bool AccessManagementClient::GetOrCreateIdentityPool(const Aws::String& poolName, bool allowUnauthenticated, Aws::String& identityPoolId)
{
    auto result = GetIdentityPool(poolName, identityPoolId);
    switch(result)
    {
        case QueryResult::YES:
            return true;

        case QueryResult::FAILURE:
            return false;

        case QueryResult::NO:
            return CreateIdentityPool(poolName, allowUnauthenticated, identityPoolId);
    }

    return false;
}

bool AccessManagementClient::BindRoleToIdentityPoolIfNot(const Aws::String& identityPoolId, 
                                                         const Aws::String& roleArn,
                                                         IdentityPoolRoleBindingType roleKey)
{
    auto result = IsRoleBoundToIdentityPool(identityPoolId, roleArn, roleKey);
    switch(result)
    {
        case QueryResult::YES:
            return true;

        case QueryResult::FAILURE:
            return false;

        case QueryResult::NO:
            return BindRoleToIdentityPool(identityPoolId, roleArn, roleKey);
    }

    return false;
}

} // namespace AccessManagement
} // namespace Aws