/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/CreateMultipartUploadResult.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::S3::Model;
using namespace Aws::Utils::Xml;
using namespace Aws::Utils;
using namespace Aws;

CreateMultipartUploadResult::CreateMultipartUploadResult() : 
    m_serverSideEncryption(ServerSideEncryption::NOT_SET),
    m_bucketKeyEnabled(false),
    m_requestCharged(RequestCharged::NOT_SET),
    m_checksumAlgorithm(ChecksumAlgorithm::NOT_SET)
{
}

CreateMultipartUploadResult::CreateMultipartUploadResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
  : CreateMultipartUploadResult()
{
  *this = result;
}

CreateMultipartUploadResult& CreateMultipartUploadResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode resultNode = xmlDocument.GetRootElement();

  if(!resultNode.IsNull())
  {
    XmlNode bucketNode = resultNode.FirstChild("Bucket");
    if(!bucketNode.IsNull())
    {
      m_bucket = Aws::Utils::Xml::DecodeEscapedXmlText(bucketNode.GetText());
    }
    XmlNode keyNode = resultNode.FirstChild("Key");
    if(!keyNode.IsNull())
    {
      m_key = Aws::Utils::Xml::DecodeEscapedXmlText(keyNode.GetText());
    }
    XmlNode uploadIdNode = resultNode.FirstChild("UploadId");
    if(!uploadIdNode.IsNull())
    {
      m_uploadId = Aws::Utils::Xml::DecodeEscapedXmlText(uploadIdNode.GetText());
    }
  }

  const auto& headers = result.GetHeaderValueCollection();
  const auto& abortDateIter = headers.find("x-amz-abort-date");
  if(abortDateIter != headers.end())
  {
    m_abortDate = DateTime(abortDateIter->second.c_str(), Aws::Utils::DateFormat::RFC822);
    if(!m_abortDate.WasParseSuccessful())
    {
      AWS_LOGSTREAM_WARN("S3::CreateMultipartUploadResult", "Failed to parse abortDate header as an RFC822 timestamp: " << abortDateIter->second.c_str());
    }
  }

  const auto& abortRuleIdIter = headers.find("x-amz-abort-rule-id");
  if(abortRuleIdIter != headers.end())
  {
    m_abortRuleId = abortRuleIdIter->second;
  }

  const auto& serverSideEncryptionIter = headers.find("x-amz-server-side-encryption");
  if(serverSideEncryptionIter != headers.end())
  {
    m_serverSideEncryption = ServerSideEncryptionMapper::GetServerSideEncryptionForName(serverSideEncryptionIter->second);
  }

  const auto& sSECustomerAlgorithmIter = headers.find("x-amz-server-side-encryption-customer-algorithm");
  if(sSECustomerAlgorithmIter != headers.end())
  {
    m_sSECustomerAlgorithm = sSECustomerAlgorithmIter->second;
  }

  const auto& sSECustomerKeyMD5Iter = headers.find("x-amz-server-side-encryption-customer-key-md5");
  if(sSECustomerKeyMD5Iter != headers.end())
  {
    m_sSECustomerKeyMD5 = sSECustomerKeyMD5Iter->second;
  }

  const auto& sSEKMSKeyIdIter = headers.find("x-amz-server-side-encryption-aws-kms-key-id");
  if(sSEKMSKeyIdIter != headers.end())
  {
    m_sSEKMSKeyId = sSEKMSKeyIdIter->second;
  }

  const auto& sSEKMSEncryptionContextIter = headers.find("x-amz-server-side-encryption-context");
  if(sSEKMSEncryptionContextIter != headers.end())
  {
    m_sSEKMSEncryptionContext = sSEKMSEncryptionContextIter->second;
  }

  const auto& bucketKeyEnabledIter = headers.find("x-amz-server-side-encryption-bucket-key-enabled");
  if(bucketKeyEnabledIter != headers.end())
  {
     m_bucketKeyEnabled = StringUtils::ConvertToBool(bucketKeyEnabledIter->second.c_str());
  }

  const auto& requestChargedIter = headers.find("x-amz-request-charged");
  if(requestChargedIter != headers.end())
  {
    m_requestCharged = RequestChargedMapper::GetRequestChargedForName(requestChargedIter->second);
  }

  const auto& checksumAlgorithmIter = headers.find("x-amz-checksum-algorithm");
  if(checksumAlgorithmIter != headers.end())
  {
    m_checksumAlgorithm = ChecksumAlgorithmMapper::GetChecksumAlgorithmForName(checksumAlgorithmIter->second);
  }

  const auto& requestIdIter = headers.find("x-amz-request-id");
  if(requestIdIter != headers.end())
  {
    m_requestId = requestIdIter->second;
  }

  return *this;
}
