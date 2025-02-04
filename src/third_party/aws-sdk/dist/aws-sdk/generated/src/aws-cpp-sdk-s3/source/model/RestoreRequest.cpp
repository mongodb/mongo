/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/RestoreRequest.h>
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

RestoreRequest::RestoreRequest() : 
    m_days(0),
    m_daysHasBeenSet(false),
    m_glacierJobParametersHasBeenSet(false),
    m_type(RestoreRequestType::NOT_SET),
    m_typeHasBeenSet(false),
    m_tier(Tier::NOT_SET),
    m_tierHasBeenSet(false),
    m_descriptionHasBeenSet(false),
    m_selectParametersHasBeenSet(false),
    m_outputLocationHasBeenSet(false)
{
}

RestoreRequest::RestoreRequest(const XmlNode& xmlNode)
  : RestoreRequest()
{
  *this = xmlNode;
}

RestoreRequest& RestoreRequest::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode daysNode = resultNode.FirstChild("Days");
    if(!daysNode.IsNull())
    {
      m_days = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(daysNode.GetText()).c_str()).c_str());
      m_daysHasBeenSet = true;
    }
    XmlNode glacierJobParametersNode = resultNode.FirstChild("GlacierJobParameters");
    if(!glacierJobParametersNode.IsNull())
    {
      m_glacierJobParameters = glacierJobParametersNode;
      m_glacierJobParametersHasBeenSet = true;
    }
    XmlNode typeNode = resultNode.FirstChild("Type");
    if(!typeNode.IsNull())
    {
      m_type = RestoreRequestTypeMapper::GetRestoreRequestTypeForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(typeNode.GetText()).c_str()).c_str());
      m_typeHasBeenSet = true;
    }
    XmlNode tierNode = resultNode.FirstChild("Tier");
    if(!tierNode.IsNull())
    {
      m_tier = TierMapper::GetTierForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(tierNode.GetText()).c_str()).c_str());
      m_tierHasBeenSet = true;
    }
    XmlNode descriptionNode = resultNode.FirstChild("Description");
    if(!descriptionNode.IsNull())
    {
      m_description = Aws::Utils::Xml::DecodeEscapedXmlText(descriptionNode.GetText());
      m_descriptionHasBeenSet = true;
    }
    XmlNode selectParametersNode = resultNode.FirstChild("SelectParameters");
    if(!selectParametersNode.IsNull())
    {
      m_selectParameters = selectParametersNode;
      m_selectParametersHasBeenSet = true;
    }
    XmlNode outputLocationNode = resultNode.FirstChild("OutputLocation");
    if(!outputLocationNode.IsNull())
    {
      m_outputLocation = outputLocationNode;
      m_outputLocationHasBeenSet = true;
    }
  }

  return *this;
}

void RestoreRequest::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_daysHasBeenSet)
  {
   XmlNode daysNode = parentNode.CreateChildElement("Days");
   ss << m_days;
   daysNode.SetText(ss.str());
   ss.str("");
  }

  if(m_glacierJobParametersHasBeenSet)
  {
   XmlNode glacierJobParametersNode = parentNode.CreateChildElement("GlacierJobParameters");
   m_glacierJobParameters.AddToNode(glacierJobParametersNode);
  }

  if(m_typeHasBeenSet)
  {
   XmlNode typeNode = parentNode.CreateChildElement("Type");
   typeNode.SetText(RestoreRequestTypeMapper::GetNameForRestoreRequestType(m_type));
  }

  if(m_tierHasBeenSet)
  {
   XmlNode tierNode = parentNode.CreateChildElement("Tier");
   tierNode.SetText(TierMapper::GetNameForTier(m_tier));
  }

  if(m_descriptionHasBeenSet)
  {
   XmlNode descriptionNode = parentNode.CreateChildElement("Description");
   descriptionNode.SetText(m_description);
  }

  if(m_selectParametersHasBeenSet)
  {
   XmlNode selectParametersNode = parentNode.CreateChildElement("SelectParameters");
   m_selectParameters.AddToNode(selectParametersNode);
  }

  if(m_outputLocationHasBeenSet)
  {
   XmlNode outputLocationNode = parentNode.CreateChildElement("OutputLocation");
   m_outputLocation.AddToNode(outputLocationNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
