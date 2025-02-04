/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/Owner.h>
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
   * <p>Information about the delete marker.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/DeleteMarkerEntry">AWS
   * API Reference</a></p>
   */
  class DeleteMarkerEntry
  {
  public:
    AWS_S3_API DeleteMarkerEntry();
    AWS_S3_API DeleteMarkerEntry(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API DeleteMarkerEntry& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The account that created the delete marker.&gt;</p>
     */
    inline const Owner& GetOwner() const{ return m_owner; }
    inline bool OwnerHasBeenSet() const { return m_ownerHasBeenSet; }
    inline void SetOwner(const Owner& value) { m_ownerHasBeenSet = true; m_owner = value; }
    inline void SetOwner(Owner&& value) { m_ownerHasBeenSet = true; m_owner = std::move(value); }
    inline DeleteMarkerEntry& WithOwner(const Owner& value) { SetOwner(value); return *this;}
    inline DeleteMarkerEntry& WithOwner(Owner&& value) { SetOwner(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The object key.</p>
     */
    inline const Aws::String& GetKey() const{ return m_key; }
    inline bool KeyHasBeenSet() const { return m_keyHasBeenSet; }
    inline void SetKey(const Aws::String& value) { m_keyHasBeenSet = true; m_key = value; }
    inline void SetKey(Aws::String&& value) { m_keyHasBeenSet = true; m_key = std::move(value); }
    inline void SetKey(const char* value) { m_keyHasBeenSet = true; m_key.assign(value); }
    inline DeleteMarkerEntry& WithKey(const Aws::String& value) { SetKey(value); return *this;}
    inline DeleteMarkerEntry& WithKey(Aws::String&& value) { SetKey(std::move(value)); return *this;}
    inline DeleteMarkerEntry& WithKey(const char* value) { SetKey(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Version ID of an object.</p>
     */
    inline const Aws::String& GetVersionId() const{ return m_versionId; }
    inline bool VersionIdHasBeenSet() const { return m_versionIdHasBeenSet; }
    inline void SetVersionId(const Aws::String& value) { m_versionIdHasBeenSet = true; m_versionId = value; }
    inline void SetVersionId(Aws::String&& value) { m_versionIdHasBeenSet = true; m_versionId = std::move(value); }
    inline void SetVersionId(const char* value) { m_versionIdHasBeenSet = true; m_versionId.assign(value); }
    inline DeleteMarkerEntry& WithVersionId(const Aws::String& value) { SetVersionId(value); return *this;}
    inline DeleteMarkerEntry& WithVersionId(Aws::String&& value) { SetVersionId(std::move(value)); return *this;}
    inline DeleteMarkerEntry& WithVersionId(const char* value) { SetVersionId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether the object is (true) or is not (false) the latest version
     * of an object.</p>
     */
    inline bool GetIsLatest() const{ return m_isLatest; }
    inline bool IsLatestHasBeenSet() const { return m_isLatestHasBeenSet; }
    inline void SetIsLatest(bool value) { m_isLatestHasBeenSet = true; m_isLatest = value; }
    inline DeleteMarkerEntry& WithIsLatest(bool value) { SetIsLatest(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Date and time when the object was last modified.</p>
     */
    inline const Aws::Utils::DateTime& GetLastModified() const{ return m_lastModified; }
    inline bool LastModifiedHasBeenSet() const { return m_lastModifiedHasBeenSet; }
    inline void SetLastModified(const Aws::Utils::DateTime& value) { m_lastModifiedHasBeenSet = true; m_lastModified = value; }
    inline void SetLastModified(Aws::Utils::DateTime&& value) { m_lastModifiedHasBeenSet = true; m_lastModified = std::move(value); }
    inline DeleteMarkerEntry& WithLastModified(const Aws::Utils::DateTime& value) { SetLastModified(value); return *this;}
    inline DeleteMarkerEntry& WithLastModified(Aws::Utils::DateTime&& value) { SetLastModified(std::move(value)); return *this;}
    ///@}
  private:

    Owner m_owner;
    bool m_ownerHasBeenSet = false;

    Aws::String m_key;
    bool m_keyHasBeenSet = false;

    Aws::String m_versionId;
    bool m_versionIdHasBeenSet = false;

    bool m_isLatest;
    bool m_isLatestHasBeenSet = false;

    Aws::Utils::DateTime m_lastModified;
    bool m_lastModifiedHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
