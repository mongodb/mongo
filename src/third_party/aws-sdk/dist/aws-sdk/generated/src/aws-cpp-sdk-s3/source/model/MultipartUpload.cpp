/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/MultipartUpload.h>
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

MultipartUpload::MultipartUpload() : 
    m_uploadIdHasBeenSet(false),
    m_keyHasBeenSet(false),
    m_initiatedHasBeenSet(false),
    m_storageClass(StorageClass::NOT_SET),
    m_storageClassHasBeenSet(false),
    m_ownerHasBeenSet(false),
    m_initiatorHasBeenSet(false),
    m_checksumAlgorithm(ChecksumAlgorithm::NOT_SET),
    m_checksumAlgorithmHasBeenSet(false)
{
}

MultipartUpload::MultipartUpload(const XmlNode& xmlNode)
  : MultipartUpload()
{
  *this = xmlNode;
}

MultipartUpload& MultipartUpload::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode uploadIdNode = resultNode.FirstChild("UploadId");
    if(!uploadIdNode.IsNull())
    {
      m_uploadId = Aws::Utils::Xml::DecodeEscapedXmlText(uploadIdNode.GetText());
      m_uploadIdHasBeenSet = true;
    }
    XmlNode keyNode = resultNode.FirstChild("Key");
    if(!keyNode.IsNull())
    {
      m_key = Aws::Utils::Xml::DecodeEscapedXmlText(keyNode.GetText());
      m_keyHasBeenSet = true;
    }
    XmlNode initiatedNode = resultNode.FirstChild("Initiated");
    if(!initiatedNode.IsNull())
    {
      m_initiated = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(initiatedNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
      m_initiatedHasBeenSet = true;
    }
    XmlNode storageClassNode = resultNode.FirstChild("StorageClass");
    if(!storageClassNode.IsNull())
    {
      m_storageClass = StorageClassMapper::GetStorageClassForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(storageClassNode.GetText()).c_str()).c_str());
      m_storageClassHasBeenSet = true;
    }
    XmlNode ownerNode = resultNode.FirstChild("Owner");
    if(!ownerNode.IsNull())
    {
      m_owner = ownerNode;
      m_ownerHasBeenSet = true;
    }
    XmlNode initiatorNode = resultNode.FirstChild("Initiator");
    if(!initiatorNode.IsNull())
    {
      m_initiator = initiatorNode;
      m_initiatorHasBeenSet = true;
    }
    XmlNode checksumAlgorithmNode = resultNode.FirstChild("ChecksumAlgorithm");
    if(!checksumAlgorithmNode.IsNull())
    {
      m_checksumAlgorithm = ChecksumAlgorithmMapper::GetChecksumAlgorithmForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(checksumAlgorithmNode.GetText()).c_str()).c_str());
      m_checksumAlgorithmHasBeenSet = true;
    }
  }

  return *this;
}

void MultipartUpload::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_uploadIdHasBeenSet)
  {
   XmlNode uploadIdNode = parentNode.CreateChildElement("UploadId");
   uploadIdNode.SetText(m_uploadId);
  }

  if(m_keyHasBeenSet)
  {
   XmlNode keyNode = parentNode.CreateChildElement("Key");
   keyNode.SetText(m_key);
  }

  if(m_initiatedHasBeenSet)
  {
   XmlNode initiatedNode = parentNode.CreateChildElement("Initiated");
   initiatedNode.SetText(m_initiated.ToGmtString(Aws::Utils::DateFormat::ISO_8601));
  }

  if(m_storageClassHasBeenSet)
  {
   XmlNode storageClassNode = parentNode.CreateChildElement("StorageClass");
   storageClassNode.SetText(StorageClassMapper::GetNameForStorageClass(m_storageClass));
  }

  if(m_ownerHasBeenSet)
  {
   XmlNode ownerNode = parentNode.CreateChildElement("Owner");
   m_owner.AddToNode(ownerNode);
  }

  if(m_initiatorHasBeenSet)
  {
   XmlNode initiatorNode = parentNode.CreateChildElement("Initiator");
   m_initiator.AddToNode(initiatorNode);
  }

  if(m_checksumAlgorithmHasBeenSet)
  {
   XmlNode checksumAlgorithmNode = parentNode.CreateChildElement("ChecksumAlgorithm");
   checksumAlgorithmNode.SetText(ChecksumAlgorithmMapper::GetNameForChecksumAlgorithm(m_checksumAlgorithm));
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
