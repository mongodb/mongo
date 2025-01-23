/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/UploadSSHPublicKeyResult.h>
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

UploadSSHPublicKeyResult::UploadSSHPublicKeyResult()
{
}

UploadSSHPublicKeyResult::UploadSSHPublicKeyResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  *this = result;
}

UploadSSHPublicKeyResult& UploadSSHPublicKeyResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "UploadSSHPublicKeyResult"))
  {
    resultNode = rootNode.FirstChild("UploadSSHPublicKeyResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode sSHPublicKeyNode = resultNode.FirstChild("SSHPublicKey");
    if(!sSHPublicKeyNode.IsNull())
    {
      m_sSHPublicKey = sSHPublicKeyNode;
    }
  }

  if (!rootNode.IsNull()) {
    XmlNode responseMetadataNode = rootNode.FirstChild("ResponseMetadata");
    m_responseMetadata = responseMetadataNode;
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::UploadSSHPublicKeyResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
