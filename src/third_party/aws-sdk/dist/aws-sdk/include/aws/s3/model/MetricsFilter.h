/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/Tag.h>
#include <aws/s3/model/MetricsAndOperator.h>
#include <utility>

namespace Aws
{
namespace Utils
{
namespace Xml
{
  class XmlNode;
} // namespace Xml
} // namespace Utils
namespace S3
{
namespace Model
{

  /**
   * <p>Specifies a metrics configuration filter. The metrics configuration only
   * includes objects that meet the filter's criteria. A filter must be a prefix, an
   * object tag, an access point ARN, or a conjunction (MetricsAndOperator). For more
   * information, see <a
   * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketMetricsConfiguration.html">PutBucketMetricsConfiguration</a>.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/MetricsFilter">AWS
   * API Reference</a></p>
   */
  class MetricsFilter
  {
  public:
    AWS_S3_API MetricsFilter();
    AWS_S3_API MetricsFilter(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API MetricsFilter& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The prefix used when evaluating a metrics filter.</p>
     */
    inline const Aws::String& GetPrefix() const{ return m_prefix; }
    inline bool PrefixHasBeenSet() const { return m_prefixHasBeenSet; }
    inline void SetPrefix(const Aws::String& value) { m_prefixHasBeenSet = true; m_prefix = value; }
    inline void SetPrefix(Aws::String&& value) { m_prefixHasBeenSet = true; m_prefix = std::move(value); }
    inline void SetPrefix(const char* value) { m_prefixHasBeenSet = true; m_prefix.assign(value); }
    inline MetricsFilter& WithPrefix(const Aws::String& value) { SetPrefix(value); return *this;}
    inline MetricsFilter& WithPrefix(Aws::String&& value) { SetPrefix(std::move(value)); return *this;}
    inline MetricsFilter& WithPrefix(const char* value) { SetPrefix(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The tag used when evaluating a metrics filter.</p>
     */
    inline const Tag& GetTag() const{ return m_tag; }
    inline bool TagHasBeenSet() const { return m_tagHasBeenSet; }
    inline void SetTag(const Tag& value) { m_tagHasBeenSet = true; m_tag = value; }
    inline void SetTag(Tag&& value) { m_tagHasBeenSet = true; m_tag = std::move(value); }
    inline MetricsFilter& WithTag(const Tag& value) { SetTag(value); return *this;}
    inline MetricsFilter& WithTag(Tag&& value) { SetTag(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The access point ARN used when evaluating a metrics filter.</p>
     */
    inline const Aws::String& GetAccessPointArn() const{ return m_accessPointArn; }
    inline bool AccessPointArnHasBeenSet() const { return m_accessPointArnHasBeenSet; }
    inline void SetAccessPointArn(const Aws::String& value) { m_accessPointArnHasBeenSet = true; m_accessPointArn = value; }
    inline void SetAccessPointArn(Aws::String&& value) { m_accessPointArnHasBeenSet = true; m_accessPointArn = std::move(value); }
    inline void SetAccessPointArn(const char* value) { m_accessPointArnHasBeenSet = true; m_accessPointArn.assign(value); }
    inline MetricsFilter& WithAccessPointArn(const Aws::String& value) { SetAccessPointArn(value); return *this;}
    inline MetricsFilter& WithAccessPointArn(Aws::String&& value) { SetAccessPointArn(std::move(value)); return *this;}
    inline MetricsFilter& WithAccessPointArn(const char* value) { SetAccessPointArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A conjunction (logical AND) of predicates, which is used in evaluating a
     * metrics filter. The operator must have at least two predicates, and an object
     * must match all of the predicates in order for the filter to apply.</p>
     */
    inline const MetricsAndOperator& GetAnd() const{ return m_and; }
    inline bool AndHasBeenSet() const { return m_andHasBeenSet; }
    inline void SetAnd(const MetricsAndOperator& value) { m_andHasBeenSet = true; m_and = value; }
    inline void SetAnd(MetricsAndOperator&& value) { m_andHasBeenSet = true; m_and = std::move(value); }
    inline MetricsFilter& WithAnd(const MetricsAndOperator& value) { SetAnd(value); return *this;}
    inline MetricsFilter& WithAnd(MetricsAndOperator&& value) { SetAnd(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_prefix;
    bool m_prefixHasBeenSet = false;

    Tag m_tag;
    bool m_tagHasBeenSet = false;

    Aws::String m_accessPointArn;
    bool m_accessPointArnHasBeenSet = false;

    MetricsAndOperator m_and;
    bool m_andHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
