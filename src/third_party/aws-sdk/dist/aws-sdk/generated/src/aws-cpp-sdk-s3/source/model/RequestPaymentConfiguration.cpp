/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/RequestPaymentConfiguration.h>
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

RequestPaymentConfiguration::RequestPaymentConfiguration() : 
    m_payer(Payer::NOT_SET),
    m_payerHasBeenSet(false)
{
}

RequestPaymentConfiguration::RequestPaymentConfiguration(const XmlNode& xmlNode)
  : RequestPaymentConfiguration()
{
  *this = xmlNode;
}

RequestPaymentConfiguration& RequestPaymentConfiguration::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode payerNode = resultNode.FirstChild("Payer");
    if(!payerNode.IsNull())
    {
      m_payer = PayerMapper::GetPayerForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(payerNode.GetText()).c_str()).c_str());
      m_payerHasBeenSet = true;
    }
  }

  return *this;
}

void RequestPaymentConfiguration::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_payerHasBeenSet)
  {
   XmlNode payerNode = parentNode.CreateChildElement("Payer");
   payerNode.SetText(PayerMapper::GetNameForPayer(m_payer));
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
