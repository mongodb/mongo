/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/IntelligentTieringFilter.h>
#include <aws/s3/model/IntelligentTieringStatus.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/s3/model/Tiering.h>
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
   * <p>Specifies the S3 Intelligent-Tiering configuration for an Amazon S3
   * bucket.</p> <p>For information about the S3 Intelligent-Tiering storage class,
   * see <a
   * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/storage-class-intro.html#sc-dynamic-data-access">Storage
   * class for automatically optimizing frequently and infrequently accessed
   * objects</a>.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/IntelligentTieringConfiguration">AWS
   * API Reference</a></p>
   */
  class IntelligentTieringConfiguration
  {
  public:
    AWS_S3_API IntelligentTieringConfiguration();
    AWS_S3_API IntelligentTieringConfiguration(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API IntelligentTieringConfiguration& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The ID used to identify the S3 Intelligent-Tiering configuration.</p>
     */
    inline const Aws::String& GetId() const{ return m_id; }
    inline bool IdHasBeenSet() const { return m_idHasBeenSet; }
    inline void SetId(const Aws::String& value) { m_idHasBeenSet = true; m_id = value; }
    inline void SetId(Aws::String&& value) { m_idHasBeenSet = true; m_id = std::move(value); }
    inline void SetId(const char* value) { m_idHasBeenSet = true; m_id.assign(value); }
    inline IntelligentTieringConfiguration& WithId(const Aws::String& value) { SetId(value); return *this;}
    inline IntelligentTieringConfiguration& WithId(Aws::String&& value) { SetId(std::move(value)); return *this;}
    inline IntelligentTieringConfiguration& WithId(const char* value) { SetId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies a bucket filter. The configuration only includes objects that meet
     * the filter's criteria.</p>
     */
    inline const IntelligentTieringFilter& GetFilter() const{ return m_filter; }
    inline bool FilterHasBeenSet() const { return m_filterHasBeenSet; }
    inline void SetFilter(const IntelligentTieringFilter& value) { m_filterHasBeenSet = true; m_filter = value; }
    inline void SetFilter(IntelligentTieringFilter&& value) { m_filterHasBeenSet = true; m_filter = std::move(value); }
    inline IntelligentTieringConfiguration& WithFilter(const IntelligentTieringFilter& value) { SetFilter(value); return *this;}
    inline IntelligentTieringConfiguration& WithFilter(IntelligentTieringFilter&& value) { SetFilter(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the status of the configuration.</p>
     */
    inline const IntelligentTieringStatus& GetStatus() const{ return m_status; }
    inline bool StatusHasBeenSet() const { return m_statusHasBeenSet; }
    inline void SetStatus(const IntelligentTieringStatus& value) { m_statusHasBeenSet = true; m_status = value; }
    inline void SetStatus(IntelligentTieringStatus&& value) { m_statusHasBeenSet = true; m_status = std::move(value); }
    inline IntelligentTieringConfiguration& WithStatus(const IntelligentTieringStatus& value) { SetStatus(value); return *this;}
    inline IntelligentTieringConfiguration& WithStatus(IntelligentTieringStatus&& value) { SetStatus(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the S3 Intelligent-Tiering storage class tier of the
     * configuration.</p>
     */
    inline const Aws::Vector<Tiering>& GetTierings() const{ return m_tierings; }
    inline bool TieringsHasBeenSet() const { return m_tieringsHasBeenSet; }
    inline void SetTierings(const Aws::Vector<Tiering>& value) { m_tieringsHasBeenSet = true; m_tierings = value; }
    inline void SetTierings(Aws::Vector<Tiering>&& value) { m_tieringsHasBeenSet = true; m_tierings = std::move(value); }
    inline IntelligentTieringConfiguration& WithTierings(const Aws::Vector<Tiering>& value) { SetTierings(value); return *this;}
    inline IntelligentTieringConfiguration& WithTierings(Aws::Vector<Tiering>&& value) { SetTierings(std::move(value)); return *this;}
    inline IntelligentTieringConfiguration& AddTierings(const Tiering& value) { m_tieringsHasBeenSet = true; m_tierings.push_back(value); return *this; }
    inline IntelligentTieringConfiguration& AddTierings(Tiering&& value) { m_tieringsHasBeenSet = true; m_tierings.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::String m_id;
    bool m_idHasBeenSet = false;

    IntelligentTieringFilter m_filter;
    bool m_filterHasBeenSet = false;

    IntelligentTieringStatus m_status;
    bool m_statusHasBeenSet = false;

    Aws::Vector<Tiering> m_tierings;
    bool m_tieringsHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
