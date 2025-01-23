/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/Rule.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Utils::Xml;
using namespace Aws::Utils;

namespace Aws
{
namespace S3
{
namespace Model
{

Rule::Rule() : 
    m_expirationHasBeenSet(false),
    m_iDHasBeenSet(false),
    m_prefixHasBeenSet(false),
    m_status(ExpirationStatus::NOT_SET),
    m_statusHasBeenSet(false),
    m_transitionHasBeenSet(false),
    m_noncurrentVersionTransitionHasBeenSet(false),
    m_noncurrentVersionExpirationHasBeenSet(false),
    m_abortIncompleteMultipartUploadHasBeenSet(false)
{
}

Rule::Rule(const XmlNode& xmlNode)
  : Rule()
{
  *this = xmlNode;
}

Rule& Rule::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode expirationNode = resultNode.FirstChild("Expiration");
    if(!expirationNode.IsNull())
    {
      m_expiration = expirationNode;
      m_expirationHasBeenSet = true;
    }
    XmlNode iDNode = resultNode.FirstChild("ID");
    if(!iDNode.IsNull())
    {
      m_iD = Aws::Utils::Xml::DecodeEscapedXmlText(iDNode.GetText());
      m_iDHasBeenSet = true;
    }
    XmlNode prefixNode = resultNode.FirstChild("Prefix");
    if(!prefixNode.IsNull())
    {
      m_prefix = Aws::Utils::Xml::DecodeEscapedXmlText(prefixNode.GetText());
      m_prefixHasBeenSet = true;
    }
    XmlNode statusNode = resultNode.FirstChild("Status");
    if(!statusNode.IsNull())
    {
      m_status = ExpirationStatusMapper::GetExpirationStatusForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(statusNode.GetText()).c_str()).c_str());
      m_statusHasBeenSet = true;
    }
    XmlNode transitionNode = resultNode.FirstChild("Transition");
    if(!transitionNode.IsNull())
    {
      m_transition = transitionNode;
      m_transitionHasBeenSet = true;
    }
    XmlNode noncurrentVersionTransitionNode = resultNode.FirstChild("NoncurrentVersionTransition");
    if(!noncurrentVersionTransitionNode.IsNull())
    {
      m_noncurrentVersionTransition = noncurrentVersionTransitionNode;
      m_noncurrentVersionTransitionHasBeenSet = true;
    }
    XmlNode noncurrentVersionExpirationNode = resultNode.FirstChild("NoncurrentVersionExpiration");
    if(!noncurrentVersionExpirationNode.IsNull())
    {
      m_noncurrentVersionExpiration = noncurrentVersionExpirationNode;
      m_noncurrentVersionExpirationHasBeenSet = true;
    }
    XmlNode abortIncompleteMultipartUploadNode = resultNode.FirstChild("AbortIncompleteMultipartUpload");
    if(!abortIncompleteMultipartUploadNode.IsNull())
    {
      m_abortIncompleteMultipartUpload = abortIncompleteMultipartUploadNode;
      m_abortIncompleteMultipartUploadHasBeenSet = true;
    }
  }

  return *this;
}

void Rule::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_expirationHasBeenSet)
  {
   XmlNode expirationNode = parentNode.CreateChildElement("Expiration");
   m_expiration.AddToNode(expirationNode);
  }

  if(m_iDHasBeenSet)
  {
   XmlNode iDNode = parentNode.CreateChildElement("ID");
   iDNode.SetText(m_iD);
  }

  if(m_prefixHasBeenSet)
  {
   XmlNode prefixNode = parentNode.CreateChildElement("Prefix");
   prefixNode.SetText(m_prefix);
  }

  if(m_statusHasBeenSet)
  {
   XmlNode statusNode = parentNode.CreateChildElement("Status");
   statusNode.SetText(ExpirationStatusMapper::GetNameForExpirationStatus(m_status));
  }

  if(m_transitionHasBeenSet)
  {
   XmlNode transitionNode = parentNode.CreateChildElement("Transition");
   m_transition.AddToNode(transitionNode);
  }

  if(m_noncurrentVersionTransitionHasBeenSet)
  {
   XmlNode noncurrentVersionTransitionNode = parentNode.CreateChildElement("NoncurrentVersionTransition");
   m_noncurrentVersionTransition.AddToNode(noncurrentVersionTransitionNode);
  }

  if(m_noncurrentVersionExpirationHasBeenSet)
  {
   XmlNode noncurrentVersionExpirationNode = parentNode.CreateChildElement("NoncurrentVersionExpiration");
   m_noncurrentVersionExpiration.AddToNode(noncurrentVersionExpirationNode);
  }

  if(m_abortIncompleteMultipartUploadHasBeenSet)
  {
   XmlNode abortIncompleteMultipartUploadNode = parentNode.CreateChildElement("AbortIncompleteMultipartUpload");
   m_abortIncompleteMultipartUpload.AddToNode(abortIncompleteMultipartUploadNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
