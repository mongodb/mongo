/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/DataRedundancy.h>
#include <aws/s3/model/BucketType.h>
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
   * <p>Specifies the information about the bucket that will be created. For more
   * information about directory buckets, see <a
   * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/directory-buckets-overview.html">Directory
   * buckets</a> in the <i>Amazon S3 User Guide</i>.</p>  <p>This functionality
   * is only supported by directory buckets.</p> <p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/BucketInfo">AWS API
   * Reference</a></p>
   */
  class BucketInfo
  {
  public:
    AWS_S3_API BucketInfo();
    AWS_S3_API BucketInfo(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API BucketInfo& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The number of Zone (Availability Zone or Local Zone) that's used for
     * redundancy for the bucket.</p>
     */
    inline const DataRedundancy& GetDataRedundancy() const{ return m_dataRedundancy; }
    inline bool DataRedundancyHasBeenSet() const { return m_dataRedundancyHasBeenSet; }
    inline void SetDataRedundancy(const DataRedundancy& value) { m_dataRedundancyHasBeenSet = true; m_dataRedundancy = value; }
    inline void SetDataRedundancy(DataRedundancy&& value) { m_dataRedundancyHasBeenSet = true; m_dataRedundancy = std::move(value); }
    inline BucketInfo& WithDataRedundancy(const DataRedundancy& value) { SetDataRedundancy(value); return *this;}
    inline BucketInfo& WithDataRedundancy(DataRedundancy&& value) { SetDataRedundancy(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The type of bucket.</p>
     */
    inline const BucketType& GetType() const{ return m_type; }
    inline bool TypeHasBeenSet() const { return m_typeHasBeenSet; }
    inline void SetType(const BucketType& value) { m_typeHasBeenSet = true; m_type = value; }
    inline void SetType(BucketType&& value) { m_typeHasBeenSet = true; m_type = std::move(value); }
    inline BucketInfo& WithType(const BucketType& value) { SetType(value); return *this;}
    inline BucketInfo& WithType(BucketType&& value) { SetType(std::move(value)); return *this;}
    ///@}
  private:

    DataRedundancy m_dataRedundancy;
    bool m_dataRedundancyHasBeenSet = false;

    BucketType m_type;
    bool m_typeHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
