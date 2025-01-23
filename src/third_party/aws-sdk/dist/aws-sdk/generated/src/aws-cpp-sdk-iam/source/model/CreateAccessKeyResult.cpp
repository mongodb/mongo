/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/CreateAccessKeyResult.h>
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

CreateAccessKeyResult::CreateAccessKeyResult()
{
}

CreateAccessKeyResult::CreateAccessKeyResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  *this = result;
}

CreateAccessKeyResult& CreateAccessKeyResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "CreateAccessKeyResult"))
  {
    resultNode = rootNode.FirstChild("CreateAccessKeyResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode accessKeyNode = resultNode.FirstChild("AccessKey");
    if(!accessKeyNode.IsNull())
    {
      m_accessKey = accessKeyNode;
    }
  }

  if (!rootNode.IsNull()) {
    XmlNode responseMetadataNode = rootNode.FirstChild("ResponseMetadata");
    m_responseMetadata = responseMetadataNode;
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::CreateAccessKeyResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
