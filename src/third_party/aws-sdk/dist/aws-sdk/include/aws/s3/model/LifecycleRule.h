/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/LifecycleExpiration.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/LifecycleRuleFilter.h>
#include <aws/s3/model/ExpirationStatus.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/s3/model/NoncurrentVersionExpiration.h>
#include <aws/s3/model/AbortIncompleteMultipartUpload.h>
#include <aws/s3/model/Transition.h>
#include <aws/s3/model/NoncurrentVersionTransition.h>
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
   * <p>A lifecycle rule for individual objects in an Amazon S3 bucket.</p> <p>For
   * more information see, <a
   * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/object-lifecycle-mgmt.html">Managing
   * your storage lifecycle</a> in the <i>Amazon S3 User Guide</i>.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/LifecycleRule">AWS
   * API Reference</a></p>
   */
  class LifecycleRule
  {
  public:
    AWS_S3_API LifecycleRule();
    AWS_S3_API LifecycleRule(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API LifecycleRule& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Specifies the expiration for the lifecycle of the object in the form of date,
     * days and, whether the object has a delete marker.</p>
     */
    inline const LifecycleExpiration& GetExpiration() const{ return m_expiration; }
    inline bool ExpirationHasBeenSet() const { return m_expirationHasBeenSet; }
    inline void SetExpiration(const LifecycleExpiration& value) { m_expirationHasBeenSet = true; m_expiration = value; }
    inline void SetExpiration(LifecycleExpiration&& value) { m_expirationHasBeenSet = true; m_expiration = std::move(value); }
    inline LifecycleRule& WithExpiration(const LifecycleExpiration& value) { SetExpiration(value); return *this;}
    inline LifecycleRule& WithExpiration(LifecycleExpiration&& value) { SetExpiration(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Unique identifier for the rule. The value cannot be longer than 255
     * characters.</p>
     */
    inline const Aws::String& GetID() const{ return m_iD; }
    inline bool IDHasBeenSet() const { return m_iDHasBeenSet; }
    inline void SetID(const Aws::String& value) { m_iDHasBeenSet = true; m_iD = value; }
    inline void SetID(Aws::String&& value) { m_iDHasBeenSet = true; m_iD = std::move(value); }
    inline void SetID(const char* value) { m_iDHasBeenSet = true; m_iD.assign(value); }
    inline LifecycleRule& WithID(const Aws::String& value) { SetID(value); return *this;}
    inline LifecycleRule& WithID(Aws::String&& value) { SetID(std::move(value)); return *this;}
    inline LifecycleRule& WithID(const char* value) { SetID(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The <code>Filter</code> is used to identify objects that a Lifecycle Rule
     * applies to. A <code>Filter</code> must have exactly one of <code>Prefix</code>,
     * <code>Tag</code>, or <code>And</code> specified. <code>Filter</code> is required
     * if the <code>LifecycleRule</code> does not contain a <code>Prefix</code>
     * element.</p>  <p> <code>Tag</code> filters are not supported for directory
     * buckets.</p> 
     */
    inline const LifecycleRuleFilter& GetFilter() const{ return m_filter; }
    inline bool FilterHasBeenSet() const { return m_filterHasBeenSet; }
    inline void SetFilter(const LifecycleRuleFilter& value) { m_filterHasBeenSet = true; m_filter = value; }
    inline void SetFilter(LifecycleRuleFilter&& value) { m_filterHasBeenSet = true; m_filter = std::move(value); }
    inline LifecycleRule& WithFilter(const LifecycleRuleFilter& value) { SetFilter(value); return *this;}
    inline LifecycleRule& WithFilter(LifecycleRuleFilter&& value) { SetFilter(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>If 'Enabled', the rule is currently being applied. If 'Disabled', the rule is
     * not currently being applied.</p>
     */
    inline const ExpirationStatus& GetStatus() const{ return m_status; }
    inline bool StatusHasBeenSet() const { return m_statusHasBeenSet; }
    inline void SetStatus(const ExpirationStatus& value) { m_statusHasBeenSet = true; m_status = value; }
    inline void SetStatus(ExpirationStatus&& value) { m_statusHasBeenSet = true; m_status = std::move(value); }
    inline LifecycleRule& WithStatus(const ExpirationStatus& value) { SetStatus(value); return *this;}
    inline LifecycleRule& WithStatus(ExpirationStatus&& value) { SetStatus(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies when an Amazon S3 object transitions to a specified storage
     * class.</p>  <p>This parameter applies to general purpose buckets only. It
     * is not supported for directory bucket lifecycle configurations.</p> 
     */
    inline const Aws::Vector<Transition>& GetTransitions() const{ return m_transitions; }
    inline bool TransitionsHasBeenSet() const { return m_transitionsHasBeenSet; }
    inline void SetTransitions(const Aws::Vector<Transition>& value) { m_transitionsHasBeenSet = true; m_transitions = value; }
    inline void SetTransitions(Aws::Vector<Transition>&& value) { m_transitionsHasBeenSet = true; m_transitions = std::move(value); }
    inline LifecycleRule& WithTransitions(const Aws::Vector<Transition>& value) { SetTransitions(value); return *this;}
    inline LifecycleRule& WithTransitions(Aws::Vector<Transition>&& value) { SetTransitions(std::move(value)); return *this;}
    inline LifecycleRule& AddTransitions(const Transition& value) { m_transitionsHasBeenSet = true; m_transitions.push_back(value); return *this; }
    inline LifecycleRule& AddTransitions(Transition&& value) { m_transitionsHasBeenSet = true; m_transitions.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>Specifies the transition rule for the lifecycle rule that describes when
     * noncurrent objects transition to a specific storage class. If your bucket is
     * versioning-enabled (or versioning is suspended), you can set this action to
     * request that Amazon S3 transition noncurrent object versions to a specific
     * storage class at a set period in the object's lifetime.</p>  <p>This
     * parameter applies to general purpose buckets only. It is not supported for
     * directory bucket lifecycle configurations.</p> 
     */
    inline const Aws::Vector<NoncurrentVersionTransition>& GetNoncurrentVersionTransitions() const{ return m_noncurrentVersionTransitions; }
    inline bool NoncurrentVersionTransitionsHasBeenSet() const { return m_noncurrentVersionTransitionsHasBeenSet; }
    inline void SetNoncurrentVersionTransitions(const Aws::Vector<NoncurrentVersionTransition>& value) { m_noncurrentVersionTransitionsHasBeenSet = true; m_noncurrentVersionTransitions = value; }
    inline void SetNoncurrentVersionTransitions(Aws::Vector<NoncurrentVersionTransition>&& value) { m_noncurrentVersionTransitionsHasBeenSet = true; m_noncurrentVersionTransitions = std::move(value); }
    inline LifecycleRule& WithNoncurrentVersionTransitions(const Aws::Vector<NoncurrentVersionTransition>& value) { SetNoncurrentVersionTransitions(value); return *this;}
    inline LifecycleRule& WithNoncurrentVersionTransitions(Aws::Vector<NoncurrentVersionTransition>&& value) { SetNoncurrentVersionTransitions(std::move(value)); return *this;}
    inline LifecycleRule& AddNoncurrentVersionTransitions(const NoncurrentVersionTransition& value) { m_noncurrentVersionTransitionsHasBeenSet = true; m_noncurrentVersionTransitions.push_back(value); return *this; }
    inline LifecycleRule& AddNoncurrentVersionTransitions(NoncurrentVersionTransition&& value) { m_noncurrentVersionTransitionsHasBeenSet = true; m_noncurrentVersionTransitions.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const NoncurrentVersionExpiration& GetNoncurrentVersionExpiration() const{ return m_noncurrentVersionExpiration; }
    inline bool NoncurrentVersionExpirationHasBeenSet() const { return m_noncurrentVersionExpirationHasBeenSet; }
    inline void SetNoncurrentVersionExpiration(const NoncurrentVersionExpiration& value) { m_noncurrentVersionExpirationHasBeenSet = true; m_noncurrentVersionExpiration = value; }
    inline void SetNoncurrentVersionExpiration(NoncurrentVersionExpiration&& value) { m_noncurrentVersionExpirationHasBeenSet = true; m_noncurrentVersionExpiration = std::move(value); }
    inline LifecycleRule& WithNoncurrentVersionExpiration(const NoncurrentVersionExpiration& value) { SetNoncurrentVersionExpiration(value); return *this;}
    inline LifecycleRule& WithNoncurrentVersionExpiration(NoncurrentVersionExpiration&& value) { SetNoncurrentVersionExpiration(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const AbortIncompleteMultipartUpload& GetAbortIncompleteMultipartUpload() const{ return m_abortIncompleteMultipartUpload; }
    inline bool AbortIncompleteMultipartUploadHasBeenSet() const { return m_abortIncompleteMultipartUploadHasBeenSet; }
    inline void SetAbortIncompleteMultipartUpload(const AbortIncompleteMultipartUpload& value) { m_abortIncompleteMultipartUploadHasBeenSet = true; m_abortIncompleteMultipartUpload = value; }
    inline void SetAbortIncompleteMultipartUpload(AbortIncompleteMultipartUpload&& value) { m_abortIncompleteMultipartUploadHasBeenSet = true; m_abortIncompleteMultipartUpload = std::move(value); }
    inline LifecycleRule& WithAbortIncompleteMultipartUpload(const AbortIncompleteMultipartUpload& value) { SetAbortIncompleteMultipartUpload(value); return *this;}
    inline LifecycleRule& WithAbortIncompleteMultipartUpload(AbortIncompleteMultipartUpload&& value) { SetAbortIncompleteMultipartUpload(std::move(value)); return *this;}
    ///@}
  private:

    LifecycleExpiration m_expiration;
    bool m_expirationHasBeenSet = false;

    Aws::String m_iD;
    bool m_iDHasBeenSet = false;

    LifecycleRuleFilter m_filter;
    bool m_filterHasBeenSet = false;

    ExpirationStatus m_status;
    bool m_statusHasBeenSet = false;

    Aws::Vector<Transition> m_transitions;
    bool m_transitionsHasBeenSet = false;

    Aws::Vector<NoncurrentVersionTransition> m_noncurrentVersionTransitions;
    bool m_noncurrentVersionTransitionsHasBeenSet = false;

    NoncurrentVersionExpiration m_noncurrentVersionExpiration;
    bool m_noncurrentVersionExpirationHasBeenSet = false;

    AbortIncompleteMultipartUpload m_abortIncompleteMultipartUpload;
    bool m_abortIncompleteMultipartUploadHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
