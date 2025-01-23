/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
   * <p>The error information.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ErrorDocument">AWS
   * API Reference</a></p>
   */
  class ErrorDocument
  {
  public:
    AWS_S3_API ErrorDocument();
    AWS_S3_API ErrorDocument(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API ErrorDocument& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The object key name to use when a 4XX class error occurs.</p> 
     * <p>Replacement must be made for object keys containing special characters (such
     * as carriage returns) when using XML requests. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/object-keys.html#object-key-xml-related-constraints">
     * XML related object key constraints</a>.</p> 
     */
    inline const Aws::String& GetKey() const{ return m_key; }
    inline bool KeyHasBeenSet() const { return m_keyHasBeenSet; }
    inline void SetKey(const Aws::String& value) { m_keyHasBeenSet = true; m_key = value; }
    inline void SetKey(Aws::String&& value) { m_keyHasBeenSet = true; m_key = std::move(value); }
    inline void SetKey(const char* value) { m_keyHasBeenSet = true; m_key.assign(value); }
    inline ErrorDocument& WithKey(const Aws::String& value) { SetKey(value); return *this;}
    inline ErrorDocument& WithKey(Aws::String&& value) { SetKey(std::move(value)); return *this;}
    inline ErrorDocument& WithKey(const char* value) { SetKey(value); return *this;}
    ///@}
  private:

    Aws::String m_key;
    bool m_keyHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
