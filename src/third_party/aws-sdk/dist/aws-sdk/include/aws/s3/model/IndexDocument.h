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
   * <p>Container for the <code>Suffix</code> element.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/IndexDocument">AWS
   * API Reference</a></p>
   */
  class IndexDocument
  {
  public:
    AWS_S3_API IndexDocument();
    AWS_S3_API IndexDocument(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API IndexDocument& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>A suffix that is appended to a request that is for a directory on the website
     * endpoint. (For example, if the suffix is <code>index.html</code> and you make a
     * request to <code>samplebucket/images/</code>, the data that is returned will be
     * for the object with the key name <code>images/index.html</code>.) The suffix
     * must not be empty and must not include a slash character.</p> 
     * <p>Replacement must be made for object keys containing special characters (such
     * as carriage returns) when using XML requests. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/object-keys.html#object-key-xml-related-constraints">
     * XML related object key constraints</a>.</p> 
     */
    inline const Aws::String& GetSuffix() const{ return m_suffix; }
    inline bool SuffixHasBeenSet() const { return m_suffixHasBeenSet; }
    inline void SetSuffix(const Aws::String& value) { m_suffixHasBeenSet = true; m_suffix = value; }
    inline void SetSuffix(Aws::String&& value) { m_suffixHasBeenSet = true; m_suffix = std::move(value); }
    inline void SetSuffix(const char* value) { m_suffixHasBeenSet = true; m_suffix.assign(value); }
    inline IndexDocument& WithSuffix(const Aws::String& value) { SetSuffix(value); return *this;}
    inline IndexDocument& WithSuffix(Aws::String&& value) { SetSuffix(std::move(value)); return *this;}
    inline IndexDocument& WithSuffix(const char* value) { SetSuffix(value); return *this;}
    ///@}
  private:

    Aws::String m_suffix;
    bool m_suffixHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
