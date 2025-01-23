/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/s3/model/TransitionDefaultMinimumObjectSize.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/LifecycleRule.h>
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
  class GetBucketLifecycleConfigurationResult
  {
  public:
    AWS_S3_API GetBucketLifecycleConfigurationResult();
    AWS_S3_API GetBucketLifecycleConfigurationResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API GetBucketLifecycleConfigurationResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>Container for a lifecycle rule.</p>
     */
    inline const Aws::Vector<LifecycleRule>& GetRules() const{ return m_rules; }
    inline void SetRules(const Aws::Vector<LifecycleRule>& value) { m_rules = value; }
    inline void SetRules(Aws::Vector<LifecycleRule>&& value) { m_rules = std::move(value); }
    inline GetBucketLifecycleConfigurationResult& WithRules(const Aws::Vector<LifecycleRule>& value) { SetRules(value); return *this;}
    inline GetBucketLifecycleConfigurationResult& WithRules(Aws::Vector<LifecycleRule>&& value) { SetRules(std::move(value)); return *this;}
    inline GetBucketLifecycleConfigurationResult& AddRules(const LifecycleRule& value) { m_rules.push_back(value); return *this; }
    inline GetBucketLifecycleConfigurationResult& AddRules(LifecycleRule&& value) { m_rules.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>Indicates which default minimum object size behavior is applied to the
     * lifecycle configuration.</p>  <p>This parameter applies to general purpose
     * buckets only. It is not supported for directory bucket lifecycle
     * configurations.</p>  <ul> <li> <p> <code>all_storage_classes_128K</code>
     * - Objects smaller than 128 KB will not transition to any storage class by
     * default.</p> </li> <li> <p> <code>varies_by_storage_class</code> - Objects
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
    inline GetBucketLifecycleConfigurationResult& WithTransitionDefaultMinimumObjectSize(const TransitionDefaultMinimumObjectSize& value) { SetTransitionDefaultMinimumObjectSize(value); return *this;}
    inline GetBucketLifecycleConfigurationResult& WithTransitionDefaultMinimumObjectSize(TransitionDefaultMinimumObjectSize&& value) { SetTransitionDefaultMinimumObjectSize(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetBucketLifecycleConfigurationResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetBucketLifecycleConfigurationResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetBucketLifecycleConfigurationResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::Vector<LifecycleRule> m_rules;

    TransitionDefaultMinimumObjectSize m_transitionDefaultMinimumObjectSize;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
