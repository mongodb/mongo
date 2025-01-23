/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/DateTime.h>
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
   * <p> In terms of implementation, a Bucket is a resource. </p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/Bucket">AWS API
   * Reference</a></p>
   */
  class Bucket
  {
  public:
    AWS_S3_API Bucket();
    AWS_S3_API Bucket(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API Bucket& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The name of the bucket.</p>
     */
    inline const Aws::String& GetName() const{ return m_name; }
    inline bool NameHasBeenSet() const { return m_nameHasBeenSet; }
    inline void SetName(const Aws::String& value) { m_nameHasBeenSet = true; m_name = value; }
    inline void SetName(Aws::String&& value) { m_nameHasBeenSet = true; m_name = std::move(value); }
    inline void SetName(const char* value) { m_nameHasBeenSet = true; m_name.assign(value); }
    inline Bucket& WithName(const Aws::String& value) { SetName(value); return *this;}
    inline Bucket& WithName(Aws::String&& value) { SetName(std::move(value)); return *this;}
    inline Bucket& WithName(const char* value) { SetName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Date the bucket was created. This date can change when making changes to your
     * bucket, such as editing its bucket policy.</p>
     */
    inline const Aws::Utils::DateTime& GetCreationDate() const{ return m_creationDate; }
    inline bool CreationDateHasBeenSet() const { return m_creationDateHasBeenSet; }
    inline void SetCreationDate(const Aws::Utils::DateTime& value) { m_creationDateHasBeenSet = true; m_creationDate = value; }
    inline void SetCreationDate(Aws::Utils::DateTime&& value) { m_creationDateHasBeenSet = true; m_creationDate = std::move(value); }
    inline Bucket& WithCreationDate(const Aws::Utils::DateTime& value) { SetCreationDate(value); return *this;}
    inline Bucket& WithCreationDate(Aws::Utils::DateTime&& value) { SetCreationDate(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p> <code>BucketRegion</code> indicates the Amazon Web Services region where the
     * bucket is located. If the request contains at least one valid parameter, it is
     * included in the response.</p>
     */
    inline const Aws::String& GetBucketRegion() const{ return m_bucketRegion; }
    inline bool BucketRegionHasBeenSet() const { return m_bucketRegionHasBeenSet; }
    inline void SetBucketRegion(const Aws::String& value) { m_bucketRegionHasBeenSet = true; m_bucketRegion = value; }
    inline void SetBucketRegion(Aws::String&& value) { m_bucketRegionHasBeenSet = true; m_bucketRegion = std::move(value); }
    inline void SetBucketRegion(const char* value) { m_bucketRegionHasBeenSet = true; m_bucketRegion.assign(value); }
    inline Bucket& WithBucketRegion(const Aws::String& value) { SetBucketRegion(value); return *this;}
    inline Bucket& WithBucketRegion(Aws::String&& value) { SetBucketRegion(std::move(value)); return *this;}
    inline Bucket& WithBucketRegion(const char* value) { SetBucketRegion(value); return *this;}
    ///@}
  private:

    Aws::String m_name;
    bool m_nameHasBeenSet = false;

    Aws::Utils::DateTime m_creationDate;
    bool m_creationDateHasBeenSet = false;

    Aws::String m_bucketRegion;
    bool m_bucketRegionHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
