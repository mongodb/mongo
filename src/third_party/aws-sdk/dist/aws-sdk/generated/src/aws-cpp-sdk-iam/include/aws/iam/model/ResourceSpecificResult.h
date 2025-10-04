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
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/iam/model/PermissionsBoundaryDecisionDetail.h>
#include <aws/iam/model/Statement.h>
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
   * <p>Contains the result of the simulation of a single API operation call on a
   * single resource.</p> <p>This data type is used by a member of the
   * <a>EvaluationResult</a> data type.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/ResourceSpecificResult">AWS
   * API Reference</a></p>
   */
  class ResourceSpecificResult
  {
  public:
    AWS_IAM_API ResourceSpecificResult();
    AWS_IAM_API ResourceSpecificResult(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API ResourceSpecificResult& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>The name of the simulated resource, in Amazon Resource Name (ARN) format.</p>
     */
    inline const Aws::String& GetEvalResourceName() const{ return m_evalResourceName; }
    inline bool EvalResourceNameHasBeenSet() const { return m_evalResourceNameHasBeenSet; }
    inline void SetEvalResourceName(const Aws::String& value) { m_evalResourceNameHasBeenSet = true; m_evalResourceName = value; }
    inline void SetEvalResourceName(Aws::String&& value) { m_evalResourceNameHasBeenSet = true; m_evalResourceName = std::move(value); }
    inline void SetEvalResourceName(const char* value) { m_evalResourceNameHasBeenSet = true; m_evalResourceName.assign(value); }
    inline ResourceSpecificResult& WithEvalResourceName(const Aws::String& value) { SetEvalResourceName(value); return *this;}
    inline ResourceSpecificResult& WithEvalResourceName(Aws::String&& value) { SetEvalResourceName(std::move(value)); return *this;}
    inline ResourceSpecificResult& WithEvalResourceName(const char* value) { SetEvalResourceName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The result of the simulation of the simulated API operation on the resource
     * specified in <code>EvalResourceName</code>.</p>
     */
    inline const PolicyEvaluationDecisionType& GetEvalResourceDecision() const{ return m_evalResourceDecision; }
    inline bool EvalResourceDecisionHasBeenSet() const { return m_evalResourceDecisionHasBeenSet; }
    inline void SetEvalResourceDecision(const PolicyEvaluationDecisionType& value) { m_evalResourceDecisionHasBeenSet = true; m_evalResourceDecision = value; }
    inline void SetEvalResourceDecision(PolicyEvaluationDecisionType&& value) { m_evalResourceDecisionHasBeenSet = true; m_evalResourceDecision = std::move(value); }
    inline ResourceSpecificResult& WithEvalResourceDecision(const PolicyEvaluationDecisionType& value) { SetEvalResourceDecision(value); return *this;}
    inline ResourceSpecificResult& WithEvalResourceDecision(PolicyEvaluationDecisionType&& value) { SetEvalResourceDecision(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of the statements in the input policies that determine the result for
     * this part of the simulation. Remember that even if multiple statements allow the
     * operation on the resource, if <i>any</i> statement denies that operation, then
     * the explicit deny overrides any allow. In addition, the deny statement is the
     * only entry included in the result.</p>
     */
    inline const Aws::Vector<Statement>& GetMatchedStatements() const{ return m_matchedStatements; }
    inline bool MatchedStatementsHasBeenSet() const { return m_matchedStatementsHasBeenSet; }
    inline void SetMatchedStatements(const Aws::Vector<Statement>& value) { m_matchedStatementsHasBeenSet = true; m_matchedStatements = value; }
    inline void SetMatchedStatements(Aws::Vector<Statement>&& value) { m_matchedStatementsHasBeenSet = true; m_matchedStatements = std::move(value); }
    inline ResourceSpecificResult& WithMatchedStatements(const Aws::Vector<Statement>& value) { SetMatchedStatements(value); return *this;}
    inline ResourceSpecificResult& WithMatchedStatements(Aws::Vector<Statement>&& value) { SetMatchedStatements(std::move(value)); return *this;}
    inline ResourceSpecificResult& AddMatchedStatements(const Statement& value) { m_matchedStatementsHasBeenSet = true; m_matchedStatements.push_back(value); return *this; }
    inline ResourceSpecificResult& AddMatchedStatements(Statement&& value) { m_matchedStatementsHasBeenSet = true; m_matchedStatements.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>A list of context keys that are required by the included input policies but
     * that were not provided by one of the input parameters. This list is used when a
     * list of ARNs is included in the <code>ResourceArns</code> parameter instead of
     * "*". If you do not specify individual resources, by setting
     * <code>ResourceArns</code> to "*" or by not including the
     * <code>ResourceArns</code> parameter, then any missing context values are instead
     * included under the <code>EvaluationResults</code> section. To discover the
     * context keys used by a set of policies, you can call
     * <a>GetContextKeysForCustomPolicy</a> or
     * <a>GetContextKeysForPrincipalPolicy</a>.</p>
     */
    inline const Aws::Vector<Aws::String>& GetMissingContextValues() const{ return m_missingContextValues; }
    inline bool MissingContextValuesHasBeenSet() const { return m_missingContextValuesHasBeenSet; }
    inline void SetMissingContextValues(const Aws::Vector<Aws::String>& value) { m_missingContextValuesHasBeenSet = true; m_missingContextValues = value; }
    inline void SetMissingContextValues(Aws::Vector<Aws::String>&& value) { m_missingContextValuesHasBeenSet = true; m_missingContextValues = std::move(value); }
    inline ResourceSpecificResult& WithMissingContextValues(const Aws::Vector<Aws::String>& value) { SetMissingContextValues(value); return *this;}
    inline ResourceSpecificResult& WithMissingContextValues(Aws::Vector<Aws::String>&& value) { SetMissingContextValues(std::move(value)); return *this;}
    inline ResourceSpecificResult& AddMissingContextValues(const Aws::String& value) { m_missingContextValuesHasBeenSet = true; m_missingContextValues.push_back(value); return *this; }
    inline ResourceSpecificResult& AddMissingContextValues(Aws::String&& value) { m_missingContextValuesHasBeenSet = true; m_missingContextValues.push_back(std::move(value)); return *this; }
    inline ResourceSpecificResult& AddMissingContextValues(const char* value) { m_missingContextValuesHasBeenSet = true; m_missingContextValues.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>Additional details about the results of the evaluation decision on a single
     * resource. This parameter is returned only for cross-account simulations. This
     * parameter explains how each policy type contributes to the resource-specific
     * evaluation decision.</p>
     */
    inline const Aws::Map<Aws::String, PolicyEvaluationDecisionType>& GetEvalDecisionDetails() const{ return m_evalDecisionDetails; }
    inline bool EvalDecisionDetailsHasBeenSet() const { return m_evalDecisionDetailsHasBeenSet; }
    inline void SetEvalDecisionDetails(const Aws::Map<Aws::String, PolicyEvaluationDecisionType>& value) { m_evalDecisionDetailsHasBeenSet = true; m_evalDecisionDetails = value; }
    inline void SetEvalDecisionDetails(Aws::Map<Aws::String, PolicyEvaluationDecisionType>&& value) { m_evalDecisionDetailsHasBeenSet = true; m_evalDecisionDetails = std::move(value); }
    inline ResourceSpecificResult& WithEvalDecisionDetails(const Aws::Map<Aws::String, PolicyEvaluationDecisionType>& value) { SetEvalDecisionDetails(value); return *this;}
    inline ResourceSpecificResult& WithEvalDecisionDetails(Aws::Map<Aws::String, PolicyEvaluationDecisionType>&& value) { SetEvalDecisionDetails(std::move(value)); return *this;}
    inline ResourceSpecificResult& AddEvalDecisionDetails(const Aws::String& key, const PolicyEvaluationDecisionType& value) { m_evalDecisionDetailsHasBeenSet = true; m_evalDecisionDetails.emplace(key, value); return *this; }
    inline ResourceSpecificResult& AddEvalDecisionDetails(Aws::String&& key, const PolicyEvaluationDecisionType& value) { m_evalDecisionDetailsHasBeenSet = true; m_evalDecisionDetails.emplace(std::move(key), value); return *this; }
    inline ResourceSpecificResult& AddEvalDecisionDetails(const Aws::String& key, PolicyEvaluationDecisionType&& value) { m_evalDecisionDetailsHasBeenSet = true; m_evalDecisionDetails.emplace(key, std::move(value)); return *this; }
    inline ResourceSpecificResult& AddEvalDecisionDetails(Aws::String&& key, PolicyEvaluationDecisionType&& value) { m_evalDecisionDetailsHasBeenSet = true; m_evalDecisionDetails.emplace(std::move(key), std::move(value)); return *this; }
    inline ResourceSpecificResult& AddEvalDecisionDetails(const char* key, PolicyEvaluationDecisionType&& value) { m_evalDecisionDetailsHasBeenSet = true; m_evalDecisionDetails.emplace(key, std::move(value)); return *this; }
    inline ResourceSpecificResult& AddEvalDecisionDetails(const char* key, const PolicyEvaluationDecisionType& value) { m_evalDecisionDetailsHasBeenSet = true; m_evalDecisionDetails.emplace(key, value); return *this; }
    ///@}

    ///@{
    /**
     * <p>Contains information about the effect that a permissions boundary has on a
     * policy simulation when that boundary is applied to an IAM entity.</p>
     */
    inline const PermissionsBoundaryDecisionDetail& GetPermissionsBoundaryDecisionDetail() const{ return m_permissionsBoundaryDecisionDetail; }
    inline bool PermissionsBoundaryDecisionDetailHasBeenSet() const { return m_permissionsBoundaryDecisionDetailHasBeenSet; }
    inline void SetPermissionsBoundaryDecisionDetail(const PermissionsBoundaryDecisionDetail& value) { m_permissionsBoundaryDecisionDetailHasBeenSet = true; m_permissionsBoundaryDecisionDetail = value; }
    inline void SetPermissionsBoundaryDecisionDetail(PermissionsBoundaryDecisionDetail&& value) { m_permissionsBoundaryDecisionDetailHasBeenSet = true; m_permissionsBoundaryDecisionDetail = std::move(value); }
    inline ResourceSpecificResult& WithPermissionsBoundaryDecisionDetail(const PermissionsBoundaryDecisionDetail& value) { SetPermissionsBoundaryDecisionDetail(value); return *this;}
    inline ResourceSpecificResult& WithPermissionsBoundaryDecisionDetail(PermissionsBoundaryDecisionDetail&& value) { SetPermissionsBoundaryDecisionDetail(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_evalResourceName;
    bool m_evalResourceNameHasBeenSet = false;

    PolicyEvaluationDecisionType m_evalResourceDecision;
    bool m_evalResourceDecisionHasBeenSet = false;

    Aws::Vector<Statement> m_matchedStatements;
    bool m_matchedStatementsHasBeenSet = false;

    Aws::Vector<Aws::String> m_missingContextValues;
    bool m_missingContextValuesHasBeenSet = false;

    Aws::Map<Aws::String, PolicyEvaluationDecisionType> m_evalDecisionDetails;
    bool m_evalDecisionDetailsHasBeenSet = false;

    PermissionsBoundaryDecisionDetail m_permissionsBoundaryDecisionDetail;
    bool m_permissionsBoundaryDecisionDetailHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
