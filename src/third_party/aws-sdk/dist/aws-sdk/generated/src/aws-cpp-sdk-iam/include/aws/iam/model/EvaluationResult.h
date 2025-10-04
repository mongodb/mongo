/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/iam/model/PolicyEvaluationDecisionType.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/iam/model/OrganizationsDecisionDetail.h>
#include <aws/iam/model/PermissionsBoundaryDecisionDetail.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/iam/model/Statement.h>
#include <aws/iam/model/ResourceSpecificResult.h>
#include <utility>

namespace Aws
{
namespace Utils
{
namespace Xml
{
  class XmlNode;
} // namespace Xml
} // namespace Utils
namespace IAM
{
namespace Model
{

  /**
   * <p>Contains the results of a simulation.</p> <p>This data type is used by the
   * return parameter of <code> <a>SimulateCustomPolicy</a> </code> and <code>
   * <a>SimulatePrincipalPolicy</a> </code>.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/EvaluationResult">AWS
   * API Reference</a></p>
   */
  class EvaluationResult
  {
  public:
    AWS_IAM_API EvaluationResult();
    AWS_IAM_API EvaluationResult(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API EvaluationResult& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>The name of the API operation tested on the indicated resource.</p>
     */
    inline const Aws::String& GetEvalActionName() const{ return m_evalActionName; }
    inline bool EvalActionNameHasBeenSet() const { return m_evalActionNameHasBeenSet; }
    inline void SetEvalActionName(const Aws::String& value) { m_evalActionNameHasBeenSet = true; m_evalActionName = value; }
    inline void SetEvalActionName(Aws::String&& value) { m_evalActionNameHasBeenSet = true; m_evalActionName = std::move(value); }
    inline void SetEvalActionName(const char* value) { m_evalActionNameHasBeenSet = true; m_evalActionName.assign(value); }
    inline EvaluationResult& WithEvalActionName(const Aws::String& value) { SetEvalActionName(value); return *this;}
    inline EvaluationResult& WithEvalActionName(Aws::String&& value) { SetEvalActionName(std::move(value)); return *this;}
    inline EvaluationResult& WithEvalActionName(const char* value) { SetEvalActionName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ARN of the resource that the indicated API operation was tested on.</p>
     */
    inline const Aws::String& GetEvalResourceName() const{ return m_evalResourceName; }
    inline bool EvalResourceNameHasBeenSet() const { return m_evalResourceNameHasBeenSet; }
    inline void SetEvalResourceName(const Aws::String& value) { m_evalResourceNameHasBeenSet = true; m_evalResourceName = value; }
    inline void SetEvalResourceName(Aws::String&& value) { m_evalResourceNameHasBeenSet = true; m_evalResourceName = std::move(value); }
    inline void SetEvalResourceName(const char* value) { m_evalResourceNameHasBeenSet = true; m_evalResourceName.assign(value); }
    inline EvaluationResult& WithEvalResourceName(const Aws::String& value) { SetEvalResourceName(value); return *this;}
    inline EvaluationResult& WithEvalResourceName(Aws::String&& value) { SetEvalResourceName(std::move(value)); return *this;}
    inline EvaluationResult& WithEvalResourceName(const char* value) { SetEvalResourceName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The result of the simulation.</p>
     */
    inline const PolicyEvaluationDecisionType& GetEvalDecision() const{ return m_evalDecision; }
    inline bool EvalDecisionHasBeenSet() const { return m_evalDecisionHasBeenSet; }
    inline void SetEvalDecision(const PolicyEvaluationDecisionType& value) { m_evalDecisionHasBeenSet = true; m_evalDecision = value; }
    inline void SetEvalDecision(PolicyEvaluationDecisionType&& value) { m_evalDecisionHasBeenSet = true; m_evalDecision = std::move(value); }
    inline EvaluationResult& WithEvalDecision(const PolicyEvaluationDecisionType& value) { SetEvalDecision(value); return *this;}
    inline EvaluationResult& WithEvalDecision(PolicyEvaluationDecisionType&& value) { SetEvalDecision(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of the statements in the input policies that determine the result for
     * this scenario. Remember that even if multiple statements allow the operation on
     * the resource, if only one statement denies that operation, then the explicit
     * deny overrides any allow. In addition, the deny statement is the only entry
     * included in the result.</p>
     */
    inline const Aws::Vector<Statement>& GetMatchedStatements() const{ return m_matchedStatements; }
    inline bool MatchedStatementsHasBeenSet() const { return m_matchedStatementsHasBeenSet; }
    inline void SetMatchedStatements(const Aws::Vector<Statement>& value) { m_matchedStatementsHasBeenSet = true; m_matchedStatements = value; }
    inline void SetMatchedStatements(Aws::Vector<Statement>&& value) { m_matchedStatementsHasBeenSet = true; m_matchedStatements = std::move(value); }
    inline EvaluationResult& WithMatchedStatements(const Aws::Vector<Statement>& value) { SetMatchedStatements(value); return *this;}
    inline EvaluationResult& WithMatchedStatements(Aws::Vector<Statement>&& value) { SetMatchedStatements(std::move(value)); return *this;}
    inline EvaluationResult& AddMatchedStatements(const Statement& value) { m_matchedStatementsHasBeenSet = true; m_matchedStatements.push_back(value); return *this; }
    inline EvaluationResult& AddMatchedStatements(Statement&& value) { m_matchedStatementsHasBeenSet = true; m_matchedStatements.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>A list of context keys that are required by the included input policies but
     * that were not provided by one of the input parameters. This list is used when
     * the resource in a simulation is "*", either explicitly, or when the
     * <code>ResourceArns</code> parameter blank. If you include a list of resources,
     * then any missing context values are instead included under the
     * <code>ResourceSpecificResults</code> section. To discover the context keys used
     * by a set of policies, you can call <a>GetContextKeysForCustomPolicy</a> or
     * <a>GetContextKeysForPrincipalPolicy</a>.</p>
     */
    inline const Aws::Vector<Aws::String>& GetMissingContextValues() const{ return m_missingContextValues; }
    inline bool MissingContextValuesHasBeenSet() const { return m_missingContextValuesHasBeenSet; }
    inline void SetMissingContextValues(const Aws::Vector<Aws::String>& value) { m_missingContextValuesHasBeenSet = true; m_missingContextValues = value; }
    inline void SetMissingContextValues(Aws::Vector<Aws::String>&& value) { m_missingContextValuesHasBeenSet = true; m_missingContextValues = std::move(value); }
    inline EvaluationResult& WithMissingContextValues(const Aws::Vector<Aws::String>& value) { SetMissingContextValues(value); return *this;}
    inline EvaluationResult& WithMissingContextValues(Aws::Vector<Aws::String>&& value) { SetMissingContextValues(std::move(value)); return *this;}
    inline EvaluationResult& AddMissingContextValues(const Aws::String& value) { m_missingContextValuesHasBeenSet = true; m_missingContextValues.push_back(value); return *this; }
    inline EvaluationResult& AddMissingContextValues(Aws::String&& value) { m_missingContextValuesHasBeenSet = true; m_missingContextValues.push_back(std::move(value)); return *this; }
    inline EvaluationResult& AddMissingContextValues(const char* value) { m_missingContextValuesHasBeenSet = true; m_missingContextValues.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>A structure that details how Organizations and its service control policies
     * affect the results of the simulation. Only applies if the simulated user's
     * account is part of an organization.</p>
     */
    inline const OrganizationsDecisionDetail& GetOrganizationsDecisionDetail() const{ return m_organizationsDecisionDetail; }
    inline bool OrganizationsDecisionDetailHasBeenSet() const { return m_organizationsDecisionDetailHasBeenSet; }
    inline void SetOrganizationsDecisionDetail(const OrganizationsDecisionDetail& value) { m_organizationsDecisionDetailHasBeenSet = true; m_organizationsDecisionDetail = value; }
    inline void SetOrganizationsDecisionDetail(OrganizationsDecisionDetail&& value) { m_organizationsDecisionDetailHasBeenSet = true; m_organizationsDecisionDetail = std::move(value); }
    inline EvaluationResult& WithOrganizationsDecisionDetail(const OrganizationsDecisionDetail& value) { SetOrganizationsDecisionDetail(value); return *this;}
    inline EvaluationResult& WithOrganizationsDecisionDetail(OrganizationsDecisionDetail&& value) { SetOrganizationsDecisionDetail(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Contains information about the effect that a permissions boundary has on a
     * policy simulation when the boundary is applied to an IAM entity.</p>
     */
    inline const PermissionsBoundaryDecisionDetail& GetPermissionsBoundaryDecisionDetail() const{ return m_permissionsBoundaryDecisionDetail; }
    inline bool PermissionsBoundaryDecisionDetailHasBeenSet() const { return m_permissionsBoundaryDecisionDetailHasBeenSet; }
    inline void SetPermissionsBoundaryDecisionDetail(const PermissionsBoundaryDecisionDetail& value) { m_permissionsBoundaryDecisionDetailHasBeenSet = true; m_permissionsBoundaryDecisionDetail = value; }
    inline void SetPermissionsBoundaryDecisionDetail(PermissionsBoundaryDecisionDetail&& value) { m_permissionsBoundaryDecisionDetailHasBeenSet = true; m_permissionsBoundaryDecisionDetail = std::move(value); }
    inline EvaluationResult& WithPermissionsBoundaryDecisionDetail(const PermissionsBoundaryDecisionDetail& value) { SetPermissionsBoundaryDecisionDetail(value); return *this;}
    inline EvaluationResult& WithPermissionsBoundaryDecisionDetail(PermissionsBoundaryDecisionDetail&& value) { SetPermissionsBoundaryDecisionDetail(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Additional details about the results of the cross-account evaluation
     * decision. This parameter is populated for only cross-account simulations. It
     * contains a brief summary of how each policy type contributes to the final
     * evaluation decision.</p> <p>If the simulation evaluates policies within the same
     * account and includes a resource ARN, then the parameter is present but the
     * response is empty. If the simulation evaluates policies within the same account
     * and specifies all resources (<code>*</code>), then the parameter is not
     * returned.</p> <p>When you make a cross-account request, Amazon Web Services
     * evaluates the request in the trusting account and the trusted account. The
     * request is allowed only if both evaluations return <code>true</code>. For more
     * information about how policies are evaluated, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_policies_evaluation-logic.html#policy-eval-basics">Evaluating
     * policies within a single account</a>.</p> <p>If an Organizations SCP included in
     * the evaluation denies access, the simulation ends. In this case, policy
     * evaluation does not proceed any further and this parameter is not returned.</p>
     */
    inline const Aws::Map<Aws::String, PolicyEvaluationDecisionType>& GetEvalDecisionDetails() const{ return m_evalDecisionDetails; }
    inline bool EvalDecisionDetailsHasBeenSet() const { return m_evalDecisionDetailsHasBeenSet; }
    inline void SetEvalDecisionDetails(const Aws::Map<Aws::String, PolicyEvaluationDecisionType>& value) { m_evalDecisionDetailsHasBeenSet = true; m_evalDecisionDetails = value; }
    inline void SetEvalDecisionDetails(Aws::Map<Aws::String, PolicyEvaluationDecisionType>&& value) { m_evalDecisionDetailsHasBeenSet = true; m_evalDecisionDetails = std::move(value); }
    inline EvaluationResult& WithEvalDecisionDetails(const Aws::Map<Aws::String, PolicyEvaluationDecisionType>& value) { SetEvalDecisionDetails(value); return *this;}
    inline EvaluationResult& WithEvalDecisionDetails(Aws::Map<Aws::String, PolicyEvaluationDecisionType>&& value) { SetEvalDecisionDetails(std::move(value)); return *this;}
    inline EvaluationResult& AddEvalDecisionDetails(const Aws::String& key, const PolicyEvaluationDecisionType& value) { m_evalDecisionDetailsHasBeenSet = true; m_evalDecisionDetails.emplace(key, value); return *this; }
    inline EvaluationResult& AddEvalDecisionDetails(Aws::String&& key, const PolicyEvaluationDecisionType& value) { m_evalDecisionDetailsHasBeenSet = true; m_evalDecisionDetails.emplace(std::move(key), value); return *this; }
    inline EvaluationResult& AddEvalDecisionDetails(const Aws::String& key, PolicyEvaluationDecisionType&& value) { m_evalDecisionDetailsHasBeenSet = true; m_evalDecisionDetails.emplace(key, std::move(value)); return *this; }
    inline EvaluationResult& AddEvalDecisionDetails(Aws::String&& key, PolicyEvaluationDecisionType&& value) { m_evalDecisionDetailsHasBeenSet = true; m_evalDecisionDetails.emplace(std::move(key), std::move(value)); return *this; }
    inline EvaluationResult& AddEvalDecisionDetails(const char* key, PolicyEvaluationDecisionType&& value) { m_evalDecisionDetailsHasBeenSet = true; m_evalDecisionDetails.emplace(key, std::move(value)); return *this; }
    inline EvaluationResult& AddEvalDecisionDetails(const char* key, const PolicyEvaluationDecisionType& value) { m_evalDecisionDetailsHasBeenSet = true; m_evalDecisionDetails.emplace(key, value); return *this; }
    ///@}

    ///@{
    /**
     * <p>The individual results of the simulation of the API operation specified in
     * EvalActionName on each resource.</p>
     */
    inline const Aws::Vector<ResourceSpecificResult>& GetResourceSpecificResults() const{ return m_resourceSpecificResults; }
    inline bool ResourceSpecificResultsHasBeenSet() const { return m_resourceSpecificResultsHasBeenSet; }
    inline void SetResourceSpecificResults(const Aws::Vector<ResourceSpecificResult>& value) { m_resourceSpecificResultsHasBeenSet = true; m_resourceSpecificResults = value; }
    inline void SetResourceSpecificResults(Aws::Vector<ResourceSpecificResult>&& value) { m_resourceSpecificResultsHasBeenSet = true; m_resourceSpecificResults = std::move(value); }
    inline EvaluationResult& WithResourceSpecificResults(const Aws::Vector<ResourceSpecificResult>& value) { SetResourceSpecificResults(value); return *this;}
    inline EvaluationResult& WithResourceSpecificResults(Aws::Vector<ResourceSpecificResult>&& value) { SetResourceSpecificResults(std::move(value)); return *this;}
    inline EvaluationResult& AddResourceSpecificResults(const ResourceSpecificResult& value) { m_resourceSpecificResultsHasBeenSet = true; m_resourceSpecificResults.push_back(value); return *this; }
    inline EvaluationResult& AddResourceSpecificResults(ResourceSpecificResult&& value) { m_resourceSpecificResultsHasBeenSet = true; m_resourceSpecificResults.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::String m_evalActionName;
    bool m_evalActionNameHasBeenSet = false;

    Aws::String m_evalResourceName;
    bool m_evalResourceNameHasBeenSet = false;

    PolicyEvaluationDecisionType m_evalDecision;
    bool m_evalDecisionHasBeenSet = false;

    Aws::Vector<Statement> m_matchedStatements;
    bool m_matchedStatementsHasBeenSet = false;

    Aws::Vector<Aws::String> m_missingContextValues;
    bool m_missingContextValuesHasBeenSet = false;

    OrganizationsDecisionDetail m_organizationsDecisionDetail;
    bool m_organizationsDecisionDetailHasBeenSet = false;

    PermissionsBoundaryDecisionDetail m_permissionsBoundaryDecisionDetail;
    bool m_permissionsBoundaryDecisionDetailHasBeenSet = false;

    Aws::Map<Aws::String, PolicyEvaluationDecisionType> m_evalDecisionDetails;
    bool m_evalDecisionDetailsHasBeenSet = false;

    Aws::Vector<ResourceSpecificResult> m_resourceSpecificResults;
    bool m_resourceSpecificResultsHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
