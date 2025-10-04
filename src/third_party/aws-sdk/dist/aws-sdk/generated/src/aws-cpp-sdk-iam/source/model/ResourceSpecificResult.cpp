/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ResourceSpecificResult.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Utils::Xml;
using namespace Aws::Utils;

namespace Aws
{
namespace IAM
{
namespace Model
{

ResourceSpecificResult::ResourceSpecificResult() : 
    m_evalResourceNameHasBeenSet(false),
    m_evalResourceDecision(PolicyEvaluationDecisionType::NOT_SET),
    m_evalResourceDecisionHasBeenSet(false),
    m_matchedStatementsHasBeenSet(false),
    m_missingContextValuesHasBeenSet(false),
    m_evalDecisionDetailsHasBeenSet(false),
    m_permissionsBoundaryDecisionDetailHasBeenSet(false)
{
}

ResourceSpecificResult::ResourceSpecificResult(const XmlNode& xmlNode)
  : ResourceSpecificResult()
{
  *this = xmlNode;
}

ResourceSpecificResult& ResourceSpecificResult::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode evalResourceNameNode = resultNode.FirstChild("EvalResourceName");
    if(!evalResourceNameNode.IsNull())
    {
      m_evalResourceName = Aws::Utils::Xml::DecodeEscapedXmlText(evalResourceNameNode.GetText());
      m_evalResourceNameHasBeenSet = true;
    }
    XmlNode evalResourceDecisionNode = resultNode.FirstChild("EvalResourceDecision");
    if(!evalResourceDecisionNode.IsNull())
    {
      m_evalResourceDecision = PolicyEvaluationDecisionTypeMapper::GetPolicyEvaluationDecisionTypeForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(evalResourceDecisionNode.GetText()).c_str()).c_str());
      m_evalResourceDecisionHasBeenSet = true;
    }
    XmlNode matchedStatementsNode = resultNode.FirstChild("MatchedStatements");
    if(!matchedStatementsNode.IsNull())
    {
      XmlNode matchedStatementsMember = matchedStatementsNode.FirstChild("member");
      while(!matchedStatementsMember.IsNull())
      {
        m_matchedStatements.push_back(matchedStatementsMember);
        matchedStatementsMember = matchedStatementsMember.NextNode("member");
      }

      m_matchedStatementsHasBeenSet = true;
    }
    XmlNode missingContextValuesNode = resultNode.FirstChild("MissingContextValues");
    if(!missingContextValuesNode.IsNull())
    {
      XmlNode missingContextValuesMember = missingContextValuesNode.FirstChild("member");
      while(!missingContextValuesMember.IsNull())
      {
        m_missingContextValues.push_back(missingContextValuesMember.GetText());
        missingContextValuesMember = missingContextValuesMember.NextNode("member");
      }

      m_missingContextValuesHasBeenSet = true;
    }
    XmlNode evalDecisionDetailsNode = resultNode.FirstChild("EvalDecisionDetails");

    if(!evalDecisionDetailsNode.IsNull())
    {
      XmlNode evalDecisionDetailsEntry = evalDecisionDetailsNode.FirstChild("entry");
      while(!evalDecisionDetailsEntry.IsNull())
      {
        XmlNode keyNode = evalDecisionDetailsEntry.FirstChild("key");
        XmlNode valueNode = evalDecisionDetailsEntry.FirstChild("value");
        m_evalDecisionDetails[keyNode.GetText()] =
            PolicyEvaluationDecisionTypeMapper::GetPolicyEvaluationDecisionTypeForName(StringUtils::Trim(valueNode.GetText().c_str()));
        evalDecisionDetailsEntry = evalDecisionDetailsEntry.NextNode("entry");
      }

      m_evalDecisionDetailsHasBeenSet = true;
    }
    XmlNode permissionsBoundaryDecisionDetailNode = resultNode.FirstChild("PermissionsBoundaryDecisionDetail");
    if(!permissionsBoundaryDecisionDetailNode.IsNull())
    {
      m_permissionsBoundaryDecisionDetail = permissionsBoundaryDecisionDetailNode;
      m_permissionsBoundaryDecisionDetailHasBeenSet = true;
    }
  }

  return *this;
}

void ResourceSpecificResult::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_evalResourceNameHasBeenSet)
  {
      oStream << location << index << locationValue << ".EvalResourceName=" << StringUtils::URLEncode(m_evalResourceName.c_str()) << "&";
  }

  if(m_evalResourceDecisionHasBeenSet)
  {
      oStream << location << index << locationValue << ".EvalResourceDecision=" << PolicyEvaluationDecisionTypeMapper::GetNameForPolicyEvaluationDecisionType(m_evalResourceDecision) << "&";
  }

  if(m_matchedStatementsHasBeenSet)
  {
      unsigned matchedStatementsIdx = 1;
      for(auto& item : m_matchedStatements)
      {
        Aws::StringStream matchedStatementsSs;
        matchedStatementsSs << location << index << locationValue << ".MatchedStatements.member." << matchedStatementsIdx++;
        item.OutputToStream(oStream, matchedStatementsSs.str().c_str());
      }
  }

  if(m_missingContextValuesHasBeenSet)
  {
      unsigned missingContextValuesIdx = 1;
      for(auto& item : m_missingContextValues)
      {
        oStream << location << index << locationValue << ".MissingContextValues.member." << missingContextValuesIdx++ << "=" << StringUtils::URLEncode(item.c_str()) << "&";
      }
  }

  if(m_evalDecisionDetailsHasBeenSet)
  {
      unsigned evalDecisionDetailsIdx = 1;
      for(auto& item : m_evalDecisionDetails)
      {
        oStream << location << index << locationValue << ".EvalDecisionDetails.entry." << evalDecisionDetailsIdx << ".key="
            << StringUtils::URLEncode(item.first.c_str()) << "&";
        oStream << location << index << locationValue << ".EvalDecisionDetails.entry." << evalDecisionDetailsIdx << ".value="
            << StringUtils::URLEncode(PolicyEvaluationDecisionTypeMapper::GetNameForPolicyEvaluationDecisionType(item.second).c_str()) << "&";
        evalDecisionDetailsIdx++;
      }
  }

  if(m_permissionsBoundaryDecisionDetailHasBeenSet)
  {
      Aws::StringStream permissionsBoundaryDecisionDetailLocationAndMemberSs;
      permissionsBoundaryDecisionDetailLocationAndMemberSs << location << index << locationValue << ".PermissionsBoundaryDecisionDetail";
      m_permissionsBoundaryDecisionDetail.OutputToStream(oStream, permissionsBoundaryDecisionDetailLocationAndMemberSs.str().c_str());
  }

}

void ResourceSpecificResult::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_evalResourceNameHasBeenSet)
  {
      oStream << location << ".EvalResourceName=" << StringUtils::URLEncode(m_evalResourceName.c_str()) << "&";
  }
  if(m_evalResourceDecisionHasBeenSet)
  {
      oStream << location << ".EvalResourceDecision=" << PolicyEvaluationDecisionTypeMapper::GetNameForPolicyEvaluationDecisionType(m_evalResourceDecision) << "&";
  }
  if(m_matchedStatementsHasBeenSet)
  {
      unsigned matchedStatementsIdx = 1;
      for(auto& item : m_matchedStatements)
      {
        Aws::StringStream matchedStatementsSs;
        matchedStatementsSs << location <<  ".MatchedStatements.member." << matchedStatementsIdx++;
        item.OutputToStream(oStream, matchedStatementsSs.str().c_str());
      }
  }
  if(m_missingContextValuesHasBeenSet)
  {
      unsigned missingContextValuesIdx = 1;
      for(auto& item : m_missingContextValues)
      {
        oStream << location << ".MissingContextValues.member." << missingContextValuesIdx++ << "=" << StringUtils::URLEncode(item.c_str()) << "&";
      }
  }
  if(m_evalDecisionDetailsHasBeenSet)
  {
      unsigned evalDecisionDetailsIdx = 1;
      for(auto& item : m_evalDecisionDetails)
      {
        oStream << location << ".EvalDecisionDetails.entry."  << evalDecisionDetailsIdx << ".key="
            << StringUtils::URLEncode(item.first.c_str()) << "&";
        oStream << location <<  ".EvalDecisionDetails.entry." << evalDecisionDetailsIdx << ".value="
            << StringUtils::URLEncode(PolicyEvaluationDecisionTypeMapper::GetNameForPolicyEvaluationDecisionType(item.second).c_str()) << "&";
        evalDecisionDetailsIdx++;
      }

  }
  if(m_permissionsBoundaryDecisionDetailHasBeenSet)
  {
      Aws::String permissionsBoundaryDecisionDetailLocationAndMember(location);
      permissionsBoundaryDecisionDetailLocationAndMember += ".PermissionsBoundaryDecisionDetail";
      m_permissionsBoundaryDecisionDetail.OutputToStream(oStream, permissionsBoundaryDecisionDetailLocationAndMember.c_str());
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
