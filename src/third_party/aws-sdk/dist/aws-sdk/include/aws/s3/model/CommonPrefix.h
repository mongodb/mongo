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
   * <p>Container for all (if there are any) keys between Prefix and the next
   * occurrence of the string specified by a delimiter. CommonPrefixes lists keys
   * that act like subdirectories in the directory specified by Prefix. For example,
   * if the prefix is notes/ and the delimiter is a slash (/) as in
   * notes/summer/july, the common prefix is notes/summer/. </p><p><h3>See Also:</h3>
   * <a href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/CommonPrefix">AWS
   * API Reference</a></p>
   */
  class CommonPrefix
  {
  public:
    AWS_S3_API CommonPrefix();
    AWS_S3_API CommonPrefix(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API CommonPrefix& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Container for the specified common prefix.</p>
     */
    inline const Aws::String& GetPrefix() const{ return m_prefix; }
    inline bool PrefixHasBeenSet() const { return m_prefixHasBeenSet; }
    inline void SetPrefix(const Aws::String& value) { m_prefixHasBeenSet = true; m_prefix = value; }
    inline void SetPrefix(Aws::String&& value) { m_prefixHasBeenSet = true; m_prefix = std::move(value); }
    inline void SetPrefix(const char* value) { m_prefixHasBeenSet = true; m_prefix.assign(value); }
    inline CommonPrefix& WithPrefix(const Aws::String& value) { SetPrefix(value); return *this;}
    inline CommonPrefix& WithPrefix(Aws::String&& value) { SetPrefix(std::move(value)); return *this;}
    inline CommonPrefix& WithPrefix(const char* value) { SetPrefix(value); return *this;}
    ///@}
  private:

    Aws::String m_prefix;
    bool m_prefixHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
