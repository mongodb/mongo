/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/UploadSigningCertificateResult.h>
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

UploadSigningCertificateResult::UploadSigningCertificateResult()
{
}

UploadSigningCertificateResult::UploadSigningCertificateResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  *this = result;
}

UploadSigningCertificateResult& UploadSigningCertificateResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "UploadSigningCertificateResult"))
  {
    resultNode = rootNode.FirstChild("UploadSigningCertificateResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode certificateNode = resultNode.FirstChild("Certificate");
    if(!certificateNode.IsNull())
    {
      m_certificate = certificateNode;
    }
  }

  if (!rootNode.IsNull()) {
    XmlNode responseMetadataNode = rootNode.FirstChild("ResponseMetadata");
    m_responseMetadata = responseMetadataNode;
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::UploadSigningCertificateResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
