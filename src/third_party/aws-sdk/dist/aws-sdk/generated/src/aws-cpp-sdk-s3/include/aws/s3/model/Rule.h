/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/LifecycleExpiration.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/ExpirationStatus.h>
#include <aws/s3/model/Transition.h>
#include <aws/s3/model/NoncurrentVersionTransition.h>
#include <aws/s3/model/NoncurrentVersionExpiration.h>
#include <aws/s3/model/AbortIncompleteMultipartUpload.h>
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
   * <p>Specifies lifecycle rules for an Amazon S3 bucket. For more information, see
   * <a
   * href="https://docs.aws.amazon.com/AmazonS3/latest/API/RESTBucketPUTlifecycle.html">Put
   * Bucket Lifecycle Configuration</a> in the <i>Amazon S3 API Reference</i>. For
   * examples, see <a
   * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketLifecycleConfiguration.html#API_PutBucketLifecycleConfiguration_Examples">Put
   * Bucket Lifecycle Configuration Examples</a>.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/Rule">AWS API
   * Reference</a></p>
   */
  class Rule
  {
  public:
    AWS_S3_API Rule();
    AWS_S3_API Rule(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API Rule& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Specifies the expiration for the lifecycle of the object.</p>
     */
    inline const LifecycleExpiration& GetExpiration() const{ return m_expiration; }
    inline bool ExpirationHasBeenSet() const { return m_expirationHasBeenSet; }
    inline void SetExpiration(const LifecycleExpiration& value) { m_expirationHasBeenSet = true; m_expiration = value; }
    inline void SetExpiration(LifecycleExpiration&& value) { m_expirationHasBeenSet = true; m_expiration = std::move(value); }
    inline Rule& WithExpiration(const LifecycleExpiration& value) { SetExpiration(value); return *this;}
    inline Rule& WithExpiration(LifecycleExpiration&& value) { SetExpiration(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Unique identifier for the rule. The value can't be longer than 255
     * characters.</p>
     */
    inline const Aws::String& GetID() const{ return m_iD; }
    inline bool IDHasBeenSet() const { return m_iDHasBeenSet; }
    inline void SetID(const Aws::String& value) { m_iDHasBeenSet = true; m_iD = value; }
    inline void SetID(Aws::String&& value) { m_iDHasBeenSet = true; m_iD = std::move(value); }
    inline void SetID(const char* value) { m_iDHasBeenSet = true; m_iD.assign(value); }
    inline Rule& WithID(const Aws::String& value) { SetID(value); return *this;}
    inline Rule& WithID(Aws::String&& value) { SetID(std::move(value)); return *this;}
    inline Rule& WithID(const char* value) { SetID(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Object key prefix that identifies one or more objects to which this rule
     * applies.</p>  <p>Replacement must be made for object keys containing
     * special characters (such as carriage returns) when using XML requests. For more
     * information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/object-keys.html#object-key-xml-related-constraints">
     * XML related object key constraints</a>.</p> 
     */
    inline const Aws::String& GetPrefix() const{ return m_prefix; }
    inline bool PrefixHasBeenSet() const { return m_prefixHasBeenSet; }
    inline void SetPrefix(const Aws::String& value) { m_prefixHasBeenSet = true; m_prefix = value; }
    inline void SetPrefix(Aws::String&& value) { m_prefixHasBeenSet = true; m_prefix = std::move(value); }
    inline void SetPrefix(const char* value) { m_prefixHasBeenSet = true; m_prefix.assign(value); }
    inline Rule& WithPrefix(const Aws::String& value) { SetPrefix(value); return *this;}
    inline Rule& WithPrefix(Aws::String&& value) { SetPrefix(std::move(value)); return *this;}
    inline Rule& WithPrefix(const char* value) { SetPrefix(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>If <code>Enabled</code>, the rule is currently being applied. If
     * <code>Disabled</code>, the rule is not currently being applied.</p>
     */
    inline const ExpirationStatus& GetStatus() const{ return m_status; }
    inline bool StatusHasBeenSet() const { return m_statusHasBeenSet; }
    inline void SetStatus(const ExpirationStatus& value) { m_statusHasBeenSet = true; m_status = value; }
    inline void SetStatus(ExpirationStatus&& value) { m_statusHasBeenSet = true; m_status = std::move(value); }
    inline Rule& WithStatus(const ExpirationStatus& value) { SetStatus(value); return *this;}
    inline Rule& WithStatus(ExpirationStatus&& value) { SetStatus(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies when an object transitions to a specified storage class. For more
     * information about Amazon S3 lifecycle configuration rules, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/lifecycle-transition-general-considerations.html">Transitioning
     * Objects Using Amazon S3 Lifecycle</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Transition& GetTransition() const{ return m_transition; }
    inline bool TransitionHasBeenSet() const { return m_transitionHasBeenSet; }
    inline void SetTransition(const Transition& value) { m_transitionHasBeenSet = true; m_transition = value; }
    inline void SetTransition(Transition&& value) { m_transitionHasBeenSet = true; m_transition = std::move(value); }
    inline Rule& WithTransition(const Transition& value) { SetTransition(value); return *this;}
    inline Rule& WithTransition(Transition&& value) { SetTransition(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const NoncurrentVersionTransition& GetNoncurrentVersionTransition() const{ return m_noncurrentVersionTransition; }
    inline bool NoncurrentVersionTransitionHasBeenSet() const { return m_noncurrentVersionTransitionHasBeenSet; }
    inline void SetNoncurrentVersionTransition(const NoncurrentVersionTransition& value) { m_noncurrentVersionTransitionHasBeenSet = true; m_noncurrentVersionTransition = value; }
    inline void SetNoncurrentVersionTransition(NoncurrentVersionTransition&& value) { m_noncurrentVersionTransitionHasBeenSet = true; m_noncurrentVersionTransition = std::move(value); }
    inline Rule& WithNoncurrentVersionTransition(const NoncurrentVersionTransition& value) { SetNoncurrentVersionTransition(value); return *this;}
    inline Rule& WithNoncurrentVersionTransition(NoncurrentVersionTransition&& value) { SetNoncurrentVersionTransition(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const NoncurrentVersionExpiration& GetNoncurrentVersionExpiration() const{ return m_noncurrentVersionExpiration; }
    inline bool NoncurrentVersionExpirationHasBeenSet() const { return m_noncurrentVersionExpirationHasBeenSet; }
    inline void SetNoncurrentVersionExpiration(const NoncurrentVersionExpiration& value) { m_noncurrentVersionExpirationHasBeenSet = true; m_noncurrentVersionExpiration = value; }
    inline void SetNoncurrentVersionExpiration(NoncurrentVersionExpiration&& value) { m_noncurrentVersionExpirationHasBeenSet = true; m_noncurrentVersionExpiration = std::move(value); }
    inline Rule& WithNoncurrentVersionExpiration(const NoncurrentVersionExpiration& value) { SetNoncurrentVersionExpiration(value); return *this;}
    inline Rule& WithNoncurrentVersionExpiration(NoncurrentVersionExpiration&& value) { SetNoncurrentVersionExpiration(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const AbortIncompleteMultipartUpload& GetAbortIncompleteMultipartUpload() const{ return m_abortIncompleteMultipartUpload; }
    inline bool AbortIncompleteMultipartUploadHasBeenSet() const { return m_abortIncompleteMultipartUploadHasBeenSet; }
    inline void SetAbortIncompleteMultipartUpload(const AbortIncompleteMultipartUpload& value) { m_abortIncompleteMultipartUploadHasBeenSet = true; m_abortIncompleteMultipartUpload = value; }
    inline void SetAbortIncompleteMultipartUpload(AbortIncompleteMultipartUpload&& value) { m_abortIncompleteMultipartUploadHasBeenSet = true; m_abortIncompleteMultipartUpload = std::move(value); }
    inline Rule& WithAbortIncompleteMultipartUpload(const AbortIncompleteMultipartUpload& value) { SetAbortIncompleteMultipartUpload(value); return *this;}
    inline Rule& WithAbortIncompleteMultipartUpload(AbortIncompleteMultipartUpload&& value) { SetAbortIncompleteMultipartUpload(std::move(value)); return *this;}
    ///@}
  private:

    LifecycleExpiration m_expiration;
    bool m_expirationHasBeenSet = false;

    Aws::String m_iD;
    bool m_iDHasBeenSet = false;

    Aws::String m_prefix;
    bool m_prefixHasBeenSet = false;

    ExpirationStatus m_status;
    bool m_statusHasBeenSet = false;

    Transition m_transition;
    bool m_transitionHasBeenSet = false;

    NoncurrentVersionTransition m_noncurrentVersionTransition;
    bool m_noncurrentVersionTransitionHasBeenSet = false;

    NoncurrentVersionExpiration m_noncurrentVersionExpiration;
    bool m_noncurrentVersionExpirationHasBeenSet = false;

    AbortIncompleteMultipartUpload m_abortIncompleteMultipartUpload;
    bool m_abortIncompleteMultipartUploadHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
