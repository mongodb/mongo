/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/TransitionDefaultMinimumObjectSize.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
namespace S3
{
namespace Model
{
  class PutBucketLifecycleConfigurationResult
  {
  public:
    AWS_S3_API PutBucketLifecycleConfigurationResult();
    AWS_S3_API PutBucketLifecycleConfigurationResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API PutBucketLifecycleConfigurationResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>Indicates which default minimum object size behavior is applied to the
     * lifecycle configuration.</p>  <p>This parameter applies to general purpose
     * buckets only. It is not supported for directory bucket lifecycle
     * configurations.</p>  <ul> <li> <p> <code>all_storage_classes_128K</code>
     * - Objects smaller than 128 KB will not transition to any storage class by
     * default. </p> </li> <li> <p> <code>varies_by_storage_class</code> - Objects
     * smaller than 128 KB will transition to Glacier Flexible Retrieval or Glacier
     * Deep Archive storage classes. By default, all other storage classes will prevent
     * transitions smaller than 128 KB. </p> </li> </ul> <p>To customize the minimum
     * object size for any transition you can add a filter that specifies a custom
     * <code>ObjectSizeGreaterThan</code> or <code>ObjectSizeLessThan</code> in the
     * body of your transition rule. Custom filters always take precedence over the
     * default transition behavior.</p>
     */
    inline const TransitionDefaultMinimumObjectSize& GetTransitionDefaultMinimumObjectSize() const{ return m_transitionDefaultMinimumObjectSize; }
    inline void SetTransitionDefaultMinimumObjectSize(const TransitionDefaultMinimumObjectSize& value) { m_transitionDefaultMinimumObjectSize = value; }
    inline void SetTransitionDefaultMinimumObjectSize(TransitionDefaultMinimumObjectSize&& value) { m_transitionDefaultMinimumObjectSize = std::move(value); }
    inline PutBucketLifecycleConfigurationResult& WithTransitionDefaultMinimumObjectSize(const TransitionDefaultMinimumObjectSize& value) { SetTransitionDefaultMinimumObjectSize(value); return *this;}
    inline PutBucketLifecycleConfigurationResult& WithTransitionDefaultMinimumObjectSize(TransitionDefaultMinimumObjectSize&& value) { SetTransitionDefaultMinimumObjectSize(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline PutBucketLifecycleConfigurationResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline PutBucketLifecycleConfigurationResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline PutBucketLifecycleConfigurationResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    TransitionDefaultMinimumObjectSize m_transitionDefaultMinimumObjectSize;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
