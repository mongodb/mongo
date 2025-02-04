/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/S3TablesDestinationResult.h>
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
   * <p> The metadata table configuration for a general purpose bucket. The
   * destination table bucket must be in the same Region and Amazon Web Services
   * account as the general purpose bucket. The specified metadata table name must be
   * unique within the <code>aws_s3_metadata</code> namespace in the destination
   * table bucket. </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/MetadataTableConfigurationResult">AWS
   * API Reference</a></p>
   */
  class MetadataTableConfigurationResult
  {
  public:
    AWS_S3_API MetadataTableConfigurationResult();
    AWS_S3_API MetadataTableConfigurationResult(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API MetadataTableConfigurationResult& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p> The destination information for the metadata table configuration. The
     * destination table bucket must be in the same Region and Amazon Web Services
     * account as the general purpose bucket. The specified metadata table name must be
     * unique within the <code>aws_s3_metadata</code> namespace in the destination
     * table bucket. </p>
     */
    inline const S3TablesDestinationResult& GetS3TablesDestinationResult() const{ return m_s3TablesDestinationResult; }
    inline bool S3TablesDestinationResultHasBeenSet() const { return m_s3TablesDestinationResultHasBeenSet; }
    inline void SetS3TablesDestinationResult(const S3TablesDestinationResult& value) { m_s3TablesDestinationResultHasBeenSet = true; m_s3TablesDestinationResult = value; }
    inline void SetS3TablesDestinationResult(S3TablesDestinationResult&& value) { m_s3TablesDestinationResultHasBeenSet = true; m_s3TablesDestinationResult = std::move(value); }
    inline MetadataTableConfigurationResult& WithS3TablesDestinationResult(const S3TablesDestinationResult& value) { SetS3TablesDestinationResult(value); return *this;}
    inline MetadataTableConfigurationResult& WithS3TablesDestinationResult(S3TablesDestinationResult&& value) { SetS3TablesDestinationResult(std::move(value)); return *this;}
    ///@}
  private:

    S3TablesDestinationResult m_s3TablesDestinationResult;
    bool m_s3TablesDestinationResultHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
