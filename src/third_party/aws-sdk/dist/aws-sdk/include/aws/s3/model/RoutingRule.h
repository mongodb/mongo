/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/Condition.h>
#include <aws/s3/model/Redirect.h>
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
   * <p>Specifies the redirect behavior and when a redirect is applied. For more
   * information about routing rules, see <a
   * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/how-to-page-redirect.html#advanced-conditional-redirects">Configuring
   * advanced conditional redirects</a> in the <i>Amazon S3 User
   * Guide</i>.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/RoutingRule">AWS API
   * Reference</a></p>
   */
  class RoutingRule
  {
  public:
    AWS_S3_API RoutingRule();
    AWS_S3_API RoutingRule(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API RoutingRule& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>A container for describing a condition that must be met for the specified
     * redirect to apply. For example, 1. If request is for pages in the
     * <code>/docs</code> folder, redirect to the <code>/documents</code> folder. 2. If
     * request results in HTTP error 4xx, redirect request to another host where you
     * might process the error.</p>
     */
    inline const Condition& GetCondition() const{ return m_condition; }
    inline bool ConditionHasBeenSet() const { return m_conditionHasBeenSet; }
    inline void SetCondition(const Condition& value) { m_conditionHasBeenSet = true; m_condition = value; }
    inline void SetCondition(Condition&& value) { m_conditionHasBeenSet = true; m_condition = std::move(value); }
    inline RoutingRule& WithCondition(const Condition& value) { SetCondition(value); return *this;}
    inline RoutingRule& WithCondition(Condition&& value) { SetCondition(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Container for redirect information. You can redirect requests to another
     * host, to another page, or with another protocol. In the event of an error, you
     * can specify a different error code to return.</p>
     */
    inline const Redirect& GetRedirect() const{ return m_redirect; }
    inline bool RedirectHasBeenSet() const { return m_redirectHasBeenSet; }
    inline void SetRedirect(const Redirect& value) { m_redirectHasBeenSet = true; m_redirect = value; }
    inline void SetRedirect(Redirect&& value) { m_redirectHasBeenSet = true; m_redirect = std::move(value); }
    inline RoutingRule& WithRedirect(const Redirect& value) { SetRedirect(value); return *this;}
    inline RoutingRule& WithRedirect(Redirect&& value) { SetRedirect(std::move(value)); return *this;}
    ///@}
  private:

    Condition m_condition;
    bool m_conditionHasBeenSet = false;

    Redirect m_redirect;
    bool m_redirectHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
