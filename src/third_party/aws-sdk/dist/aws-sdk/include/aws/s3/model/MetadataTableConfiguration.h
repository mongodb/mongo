/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/S3TablesDestination.h>
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
   * <p> The metadata table configuration for a general purpose bucket.
   * </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/MetadataTableConfiguration">AWS
   * API Reference</a></p>
   */
  class MetadataTableConfiguration
  {
  public:
    AWS_S3_API MetadataTableConfiguration();
    AWS_S3_API MetadataTableConfiguration(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API MetadataTableConfiguration& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p> The destination information for the metadata table configuration. The
     * destination table bucket must be in the same Region and Amazon Web Services
     * account as the general purpose bucket. The specified metadata table name must be
     * unique within the <code>aws_s3_metadata</code> namespace in the destination
     * table bucket. </p>
     */
    inline const S3TablesDestination& GetS3TablesDestination() const{ return m_s3TablesDestination; }
    inline bool S3TablesDestinationHasBeenSet() const { return m_s3TablesDestinationHasBeenSet; }
    inline void SetS3TablesDestination(const S3TablesDestination& value) { m_s3TablesDestinationHasBeenSet = true; m_s3TablesDestination = value; }
    inline void SetS3TablesDestination(S3TablesDestination&& value) { m_s3TablesDestinationHasBeenSet = true; m_s3TablesDestination = std::move(value); }
    inline MetadataTableConfiguration& WithS3TablesDestination(const S3TablesDestination& value) { SetS3TablesDestination(value); return *this;}
    inline MetadataTableConfiguration& WithS3TablesDestination(S3TablesDestination&& value) { SetS3TablesDestination(std::move(value)); return *this;}
    ///@}
  private:

    S3TablesDestination m_s3TablesDestination;
    bool m_s3TablesDestinationHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
