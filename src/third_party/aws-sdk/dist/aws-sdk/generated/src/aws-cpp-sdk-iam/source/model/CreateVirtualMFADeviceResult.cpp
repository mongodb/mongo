/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/CreateVirtualMFADeviceResult.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/logging/LogMacros.h>

#include <utility>

using namespace Aws::IAM::Model;
using namespace Aws::Utils::Xml;
using namespace Aws::Utils::Logging;
using namespace Aws::Utils;
using namespace Aws;

CreateVirtualMFADeviceResult::CreateVirtualMFADeviceResult()
{
}

CreateVirtualMFADeviceResult::CreateVirtualMFADeviceResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  *this = result;
}

CreateVirtualMFADeviceResult& CreateVirtualMFADeviceResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "CreateVirtualMFADeviceResult"))
  {
    resultNode = rootNode.FirstChild("CreateVirtualMFADeviceResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode virtualMFADeviceNode = resultNode.FirstChild("VirtualMFADevice");
    if(!virtualMFADeviceNode.IsNull())
    {
      m_virtualMFADevice = virtualMFADeviceNode;
    }
  }

  if (!rootNode.IsNull()) {
    XmlNode responseMetadataNode = rootNode.FirstChild("ResponseMetadata");
    m_responseMetadata = responseMetadataNode;
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::CreateVirtualMFADeviceResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
