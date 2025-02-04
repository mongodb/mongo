/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/Protocol.h>
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
   * <p>Specifies how requests are redirected. In the event of an error, you can
   * specify a different error code to return.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/Redirect">AWS API
   * Reference</a></p>
   */
  class Redirect
  {
  public:
    AWS_S3_API Redirect();
    AWS_S3_API Redirect(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API Redirect& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The host name to use in the redirect request.</p>
     */
    inline const Aws::String& GetHostName() const{ return m_hostName; }
    inline bool HostNameHasBeenSet() const { return m_hostNameHasBeenSet; }
    inline void SetHostName(const Aws::String& value) { m_hostNameHasBeenSet = true; m_hostName = value; }
    inline void SetHostName(Aws::String&& value) { m_hostNameHasBeenSet = true; m_hostName = std::move(value); }
    inline void SetHostName(const char* value) { m_hostNameHasBeenSet = true; m_hostName.assign(value); }
    inline Redirect& WithHostName(const Aws::String& value) { SetHostName(value); return *this;}
    inline Redirect& WithHostName(Aws::String&& value) { SetHostName(std::move(value)); return *this;}
    inline Redirect& WithHostName(const char* value) { SetHostName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The HTTP redirect code to use on the response. Not required if one of the
     * siblings is present.</p>
     */
    inline const Aws::String& GetHttpRedirectCode() const{ return m_httpRedirectCode; }
    inline bool HttpRedirectCodeHasBeenSet() const { return m_httpRedirectCodeHasBeenSet; }
    inline void SetHttpRedirectCode(const Aws::String& value) { m_httpRedirectCodeHasBeenSet = true; m_httpRedirectCode = value; }
    inline void SetHttpRedirectCode(Aws::String&& value) { m_httpRedirectCodeHasBeenSet = true; m_httpRedirectCode = std::move(value); }
    inline void SetHttpRedirectCode(const char* value) { m_httpRedirectCodeHasBeenSet = true; m_httpRedirectCode.assign(value); }
    inline Redirect& WithHttpRedirectCode(const Aws::String& value) { SetHttpRedirectCode(value); return *this;}
    inline Redirect& WithHttpRedirectCode(Aws::String&& value) { SetHttpRedirectCode(std::move(value)); return *this;}
    inline Redirect& WithHttpRedirectCode(const char* value) { SetHttpRedirectCode(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Protocol to use when redirecting requests. The default is the protocol that
     * is used in the original request.</p>
     */
    inline const Protocol& GetProtocol() const{ return m_protocol; }
    inline bool ProtocolHasBeenSet() const { return m_protocolHasBeenSet; }
    inline void SetProtocol(const Protocol& value) { m_protocolHasBeenSet = true; m_protocol = value; }
    inline void SetProtocol(Protocol&& value) { m_protocolHasBeenSet = true; m_protocol = std::move(value); }
    inline Redirect& WithProtocol(const Protocol& value) { SetProtocol(value); return *this;}
    inline Redirect& WithProtocol(Protocol&& value) { SetProtocol(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The object key prefix to use in the redirect request. For example, to
     * redirect requests for all pages with prefix <code>docs/</code> (objects in the
     * <code>docs/</code> folder) to <code>documents/</code>, you can set a condition
     * block with <code>KeyPrefixEquals</code> set to <code>docs/</code> and in the
     * Redirect set <code>ReplaceKeyPrefixWith</code> to <code>/documents</code>. Not
     * required if one of the siblings is present. Can be present only if
     * <code>ReplaceKeyWith</code> is not provided.</p>  <p>Replacement must
     * be made for object keys containing special characters (such as carriage returns)
     * when using XML requests. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/object-keys.html#object-key-xml-related-constraints">
     * XML related object key constraints</a>.</p> 
     */
    inline const Aws::String& GetReplaceKeyPrefixWith() const{ return m_replaceKeyPrefixWith; }
    inline bool ReplaceKeyPrefixWithHasBeenSet() const { return m_replaceKeyPrefixWithHasBeenSet; }
    inline void SetReplaceKeyPrefixWith(const Aws::String& value) { m_replaceKeyPrefixWithHasBeenSet = true; m_replaceKeyPrefixWith = value; }
    inline void SetReplaceKeyPrefixWith(Aws::String&& value) { m_replaceKeyPrefixWithHasBeenSet = true; m_replaceKeyPrefixWith = std::move(value); }
    inline void SetReplaceKeyPrefixWith(const char* value) { m_replaceKeyPrefixWithHasBeenSet = true; m_replaceKeyPrefixWith.assign(value); }
    inline Redirect& WithReplaceKeyPrefixWith(const Aws::String& value) { SetReplaceKeyPrefixWith(value); return *this;}
    inline Redirect& WithReplaceKeyPrefixWith(Aws::String&& value) { SetReplaceKeyPrefixWith(std::move(value)); return *this;}
    inline Redirect& WithReplaceKeyPrefixWith(const char* value) { SetReplaceKeyPrefixWith(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The specific object key to use in the redirect request. For example, redirect
     * request to <code>error.html</code>. Not required if one of the siblings is
     * present. Can be present only if <code>ReplaceKeyPrefixWith</code> is not
     * provided.</p>  <p>Replacement must be made for object keys containing
     * special characters (such as carriage returns) when using XML requests. For more
     * information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/object-keys.html#object-key-xml-related-constraints">
     * XML related object key constraints</a>.</p> 
     */
    inline const Aws::String& GetReplaceKeyWith() const{ return m_replaceKeyWith; }
    inline bool ReplaceKeyWithHasBeenSet() const { return m_replaceKeyWithHasBeenSet; }
    inline void SetReplaceKeyWith(const Aws::String& value) { m_replaceKeyWithHasBeenSet = true; m_replaceKeyWith = value; }
    inline void SetReplaceKeyWith(Aws::String&& value) { m_replaceKeyWithHasBeenSet = true; m_replaceKeyWith = std::move(value); }
    inline void SetReplaceKeyWith(const char* value) { m_replaceKeyWithHasBeenSet = true; m_replaceKeyWith.assign(value); }
    inline Redirect& WithReplaceKeyWith(const Aws::String& value) { SetReplaceKeyWith(value); return *this;}
    inline Redirect& WithReplaceKeyWith(Aws::String&& value) { SetReplaceKeyWith(std::move(value)); return *this;}
    inline Redirect& WithReplaceKeyWith(const char* value) { SetReplaceKeyWith(value); return *this;}
    ///@}
  private:

    Aws::String m_hostName;
    bool m_hostNameHasBeenSet = false;

    Aws::String m_httpRedirectCode;
    bool m_httpRedirectCodeHasBeenSet = false;

    Protocol m_protocol;
    bool m_protocolHasBeenSet = false;

    Aws::String m_replaceKeyPrefixWith;
    bool m_replaceKeyPrefixWithHasBeenSet = false;

    Aws::String m_replaceKeyWith;
    bool m_replaceKeyWithHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
