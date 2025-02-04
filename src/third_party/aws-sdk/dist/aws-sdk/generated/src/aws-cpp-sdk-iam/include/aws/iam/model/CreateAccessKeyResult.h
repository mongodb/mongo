/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/model/AccessKey.h>
#include <aws/iam/model/ResponseMetadata.h>
#include <utility>

namespace Aws
{
template<typename RESULT_TYPE>
class AmazonWebServiceResult;

namespace Utils
{
namespace Xml
{
  class XmlDocument;
} // namespace Xml
} // namespace Utils
namespace IAM
{
namespace Model
{
  /**
   * <p>Contains the response to a successful <a>CreateAccessKey</a> request.
   * </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/CreateAccessKeyResponse">AWS
   * API Reference</a></p>
   */
  class CreateAccessKeyResult
  {
  public:
    AWS_IAM_API CreateAccessKeyResult();
    AWS_IAM_API CreateAccessKeyResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API CreateAccessKeyResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>A structure with details about the access key.</p>
     */
    inline const AccessKey& GetAccessKey() const{ return m_accessKey; }
    inline void SetAccessKey(const AccessKey& value) { m_accessKey = value; }
    inline void SetAccessKey(AccessKey&& value) { m_accessKey = std::move(value); }
    inline CreateAccessKeyResult& WithAccessKey(const AccessKey& value) { SetAccessKey(value); return *this;}
    inline CreateAccessKeyResult& WithAccessKey(AccessKey&& value) { SetAccessKey(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline CreateAccessKeyResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline CreateAccessKeyResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    AccessKey m_accessKey;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
