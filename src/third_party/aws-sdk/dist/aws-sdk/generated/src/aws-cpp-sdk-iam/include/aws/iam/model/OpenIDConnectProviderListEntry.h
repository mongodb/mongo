/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
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
namespace IAM
{
namespace Model
{

  /**
   * <p>Contains the Amazon Resource Name (ARN) for an IAM OpenID Connect
   * provider.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/OpenIDConnectProviderListEntry">AWS
   * API Reference</a></p>
   */
  class OpenIDConnectProviderListEntry
  {
  public:
    AWS_IAM_API OpenIDConnectProviderListEntry();
    AWS_IAM_API OpenIDConnectProviderListEntry(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API OpenIDConnectProviderListEntry& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    
    inline const Aws::String& GetArn() const{ return m_arn; }
    inline bool ArnHasBeenSet() const { return m_arnHasBeenSet; }
    inline void SetArn(const Aws::String& value) { m_arnHasBeenSet = true; m_arn = value; }
    inline void SetArn(Aws::String&& value) { m_arnHasBeenSet = true; m_arn = std::move(value); }
    inline void SetArn(const char* value) { m_arnHasBeenSet = true; m_arn.assign(value); }
    inline OpenIDConnectProviderListEntry& WithArn(const Aws::String& value) { SetArn(value); return *this;}
    inline OpenIDConnectProviderListEntry& WithArn(Aws::String&& value) { SetArn(std::move(value)); return *this;}
    inline OpenIDConnectProviderListEntry& WithArn(const char* value) { SetArn(value); return *this;}
    ///@}
  private:

    Aws::String m_arn;
    bool m_arnHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
