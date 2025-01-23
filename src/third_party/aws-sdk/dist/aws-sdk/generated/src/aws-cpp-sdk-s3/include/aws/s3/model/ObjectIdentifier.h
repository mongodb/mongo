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
   * <p>Object Identifier is unique value to identify objects.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ObjectIdentifier">AWS
   * API Reference</a></p>
   */
  class ObjectIdentifier
  {
  public:
    AWS_S3_API ObjectIdentifier();
    AWS_S3_API ObjectIdentifier(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API ObjectIdentifier& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Key name of the object.</p>  <p>Replacement must be made for
     * object keys containing special characters (such as carriage returns) when using
     * XML requests. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/object-keys.html#object-key-xml-related-constraints">
     * XML related object key constraints</a>.</p> 
     */
    inline const Aws::String& GetKey() const{ return m_key; }
    inline bool KeyHasBeenSet() const { return m_keyHasBeenSet; }
    inline void SetKey(const Aws::String& value) { m_keyHasBeenSet = true; m_key = value; }
    inline void SetKey(Aws::String&& value) { m_keyHasBeenSet = true; m_key = std::move(value); }
    inline void SetKey(const char* value) { m_keyHasBeenSet = true; m_key.assign(value); }
    inline ObjectIdentifier& WithKey(const Aws::String& value) { SetKey(value); return *this;}
    inline ObjectIdentifier& WithKey(Aws::String&& value) { SetKey(std::move(value)); return *this;}
    inline ObjectIdentifier& WithKey(const char* value) { SetKey(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Version ID for the specific version of the object to delete.</p> 
     * <p>This functionality is not supported for directory buckets.</p> 
     */
    inline const Aws::String& GetVersionId() const{ return m_versionId; }
    inline bool VersionIdHasBeenSet() const { return m_versionIdHasBeenSet; }
    inline void SetVersionId(const Aws::String& value) { m_versionIdHasBeenSet = true; m_versionId = value; }
    inline void SetVersionId(Aws::String&& value) { m_versionIdHasBeenSet = true; m_versionId = std::move(value); }
    inline void SetVersionId(const char* value) { m_versionIdHasBeenSet = true; m_versionId.assign(value); }
    inline ObjectIdentifier& WithVersionId(const Aws::String& value) { SetVersionId(value); return *this;}
    inline ObjectIdentifier& WithVersionId(Aws::String&& value) { SetVersionId(std::move(value)); return *this;}
    inline ObjectIdentifier& WithVersionId(const char* value) { SetVersionId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>An entity tag (ETag) is an identifier assigned by a web server to a specific
     * version of a resource found at a URL. This header field makes the request method
     * conditional on <code>ETags</code>. </p>  <p>Entity tags (ETags) for S3
     * Express One Zone are random alphanumeric strings unique to the object. </p>
     * 
     */
    inline const Aws::String& GetETag() const{ return m_eTag; }
    inline bool ETagHasBeenSet() const { return m_eTagHasBeenSet; }
    inline void SetETag(const Aws::String& value) { m_eTagHasBeenSet = true; m_eTag = value; }
    inline void SetETag(Aws::String&& value) { m_eTagHasBeenSet = true; m_eTag = std::move(value); }
    inline void SetETag(const char* value) { m_eTagHasBeenSet = true; m_eTag.assign(value); }
    inline ObjectIdentifier& WithETag(const Aws::String& value) { SetETag(value); return *this;}
    inline ObjectIdentifier& WithETag(Aws::String&& value) { SetETag(std::move(value)); return *this;}
    inline ObjectIdentifier& WithETag(const char* value) { SetETag(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>If present, the objects are deleted only if its modification times matches
     * the provided <code>Timestamp</code>. </p>  <p>This functionality is only
     * supported for directory buckets.</p> 
     */
    inline const Aws::Utils::DateTime& GetLastModifiedTime() const{ return m_lastModifiedTime; }
    inline bool LastModifiedTimeHasBeenSet() const { return m_lastModifiedTimeHasBeenSet; }
    inline void SetLastModifiedTime(const Aws::Utils::DateTime& value) { m_lastModifiedTimeHasBeenSet = true; m_lastModifiedTime = value; }
    inline void SetLastModifiedTime(Aws::Utils::DateTime&& value) { m_lastModifiedTimeHasBeenSet = true; m_lastModifiedTime = std::move(value); }
    inline ObjectIdentifier& WithLastModifiedTime(const Aws::Utils::DateTime& value) { SetLastModifiedTime(value); return *this;}
    inline ObjectIdentifier& WithLastModifiedTime(Aws::Utils::DateTime&& value) { SetLastModifiedTime(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>If present, the objects are deleted only if its size matches the provided
     * size in bytes. </p>  <p>This functionality is only supported for directory
     * buckets.</p> 
     */
    inline long long GetSize() const{ return m_size; }
    inline bool SizeHasBeenSet() const { return m_sizeHasBeenSet; }
    inline void SetSize(long long value) { m_sizeHasBeenSet = true; m_size = value; }
    inline ObjectIdentifier& WithSize(long long value) { SetSize(value); return *this;}
    ///@}
  private:

    Aws::String m_key;
    bool m_keyHasBeenSet = false;

    Aws::String m_versionId;
    bool m_versionIdHasBeenSet = false;

    Aws::String m_eTag;
    bool m_eTagHasBeenSet = false;

    Aws::Utils::DateTime m_lastModifiedTime;
    bool m_lastModifiedTimeHasBeenSet = false;

    long long m_size;
    bool m_sizeHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
