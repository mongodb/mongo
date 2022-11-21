/*
 * Copyright 2021-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef KMS_KMIP_TAG_TYPE_PRIVATE_H
#define KMS_KMIP_TAG_TYPE_PRIVATE_H

#include "kms_message/kms_message_defines.h"

/* clang-format off */
#define KMS_XMACRO                                                                          \
   KMS_X (ActivationDate, 0x420001)                                                         \
   KMS_X (ApplicationData, 0x420002)                                                        \
   KMS_X (ApplicationNamespace, 0x420003)                                                   \
   KMS_X (ApplicationSpecificInformation, 0x420004)                                         \
   KMS_X (ArchiveDate, 0x420005)                                                            \
   KMS_X (AsynchronousCorrelationValue, 0x420006)                                           \
   KMS_X (AsynchronousIndicator, 0x420007)                                                  \
   KMS_X (Attribute, 0x420008)                                                              \
   KMS_X (AttributeIndex, 0x420009)                                                         \
   KMS_X (AttributeName, 0x42000A)                                                          \
   KMS_X (AttributeValue, 0x42000B)                                                         \
   KMS_X (Authentication, 0x42000C)                                                         \
   KMS_X (BatchCount, 0x42000D)                                                             \
   KMS_X (BatchErrorContinuationOption, 0x42000E)                                           \
   KMS_X (BatchItem, 0x42000F)                                                              \
   KMS_X (BatchOrderOption, 0x420010)                                                       \
   KMS_X (BlockCipherMode, 0x420011)                                                        \
   KMS_X (CancellationResult, 0x420012)                                                     \
   KMS_X (Certificate, 0x420013)                                                            \
   KMS_X (CertificateIdentifier, 0x420014)              /* deprecated as of version 1.1 */  \
   KMS_X (CertificateIssuer, 0x420015)                  /* deprecated as of version 1.1 */  \
   KMS_X (CertificateIssuerAlternativeName, 0x420016)   /* deprecated as of version 1.1 */  \
   KMS_X (CertificateIssuerDistinguishedName, 0x420017) /* deprecated as of version 1.1 */  \
   KMS_X (CertificateRequest, 0x420018)                                                     \
   KMS_X (CertificateRequestType, 0x420019)                                                 \
   KMS_X (CertificateSubject, 0x42001A)                  /* deprecated as of version 1.1 */ \
   KMS_X (CertificateSubjectAlternativeName, 0x42001B)   /* deprecated as of version 1.1 */ \
   KMS_X (CertificateSubjectDistinguishedName, 0x42001C) /* deprecated as of version 1.1 */ \
   KMS_X (CertificateType, 0x42001D)                                                        \
   KMS_X (CertificateValue, 0x42001E)                                                       \
   KMS_X (CommonTemplateAttribute, 0x42001F)                                                \
   KMS_X (CompromiseDate, 0x420020)                                                         \
   KMS_X (CompromiseOccurrenceDate, 0x420021)                                               \
   KMS_X (ContactInformation, 0x420022)                                                     \
   KMS_X (Credential, 0x420023)                                                             \
   KMS_X (CredentialType, 0x420024)                                                         \
   KMS_X (CredentialValue, 0x420025)                                                        \
   KMS_X (CriticalityIndicator, 0x420026)                                                   \
   KMS_X (CRTCoefficient, 0x420027)                                                         \
   KMS_X (CryptographicAlgorithm, 0x420028)                                                 \
   KMS_X (CryptographicDomainParameters, 0x420029)                                          \
   KMS_X (CryptographicLength, 0x42002A)                                                    \
   KMS_X (CryptographicParameters, 0x42002B)                                                \
   KMS_X (CryptographicUsageMask, 0x42002C)                                                 \
   KMS_X (CustomAttribute, 0x42002D)                                                        \
   KMS_X (D, 0x42002E)                                                                      \
   KMS_X (DeactivationDate, 0x42002F)                                                       \
   KMS_X (DerivationData, 0x420030)                                                         \
   KMS_X (DerivationMethod, 0x420031)                                                       \
   KMS_X (DerivationParameters, 0x420032)                                                   \
   KMS_X (DestroyDate, 0x420033)                                                            \
   KMS_X (Digest, 0x420034)                                                                 \
   KMS_X (DigestValue, 0x420035)                                                            \
   KMS_X (EncryptionKeyInformation, 0x420036)                                               \
   KMS_X (G, 0x420037)                                                                      \
   KMS_X (HashingAlgorithm, 0x420038)                                                       \
   KMS_X (InitialDate, 0x420039)                                                            \
   KMS_X (InitializationVector, 0x42003A)                                                   \
   KMS_X (Issuer, 0x42003B) /* deprecated as of version 1.1 */                              \
   KMS_X (IterationCount, 0x42003C)                                                         \
   KMS_X (IVCounterNonce, 0x42003D)                                                         \
   KMS_X (J, 0x42003E)                                                                      \
   KMS_X (Key, 0x42003F)                                                                    \
   KMS_X (KeyBlock, 0x420040)                                                               \
   KMS_X (KeyCompressionType, 0x420041)                                                     \
   KMS_X (KeyFormatType, 0x420042)                                                          \
   KMS_X (KeyMaterial, 0x420043)                                                            \
   KMS_X (KeyPartIdentifier, 0x420044)                                                      \
   KMS_X (KeyValue, 0x420045)                                                               \
   KMS_X (KeyWrappingData, 0x420046)                                                        \
   KMS_X (KeyWrappingSpecification, 0x420047)                                               \
   KMS_X (LastChangeDate, 0x420048)                                                         \
   KMS_X (LeaseTime, 0x420049)                                                              \
   KMS_X (Link, 0x42004A)                                                                   \
   KMS_X (LinkType, 0x42004B)                                                               \
   KMS_X (LinkedObjectIdentifier, 0x42004C)                                                 \
   KMS_X (MACSignature, 0x42004D)                                                           \
   KMS_X (MACSignatureKeyInformation, 0x42004E)                                             \
   KMS_X (MaximumItems, 0x42004F)                                                           \
   KMS_X (MaximumResponseSize, 0x420050)                                                    \
   KMS_X (MessageExtension, 0x420051)                                                       \
   KMS_X (Modulus, 0x420052)                                                                \
   KMS_X (Name, 0x420053)                                                                   \
   KMS_X (NameType, 0x420054)                                                               \
   KMS_X (NameValue, 0x420055)                                                              \
   KMS_X (ObjectGroup, 0x420056)                                                            \
   KMS_X (ObjectType, 0x420057)                                                             \
   KMS_X (Offset, 0x420058)                                                                 \
   KMS_X (OpaqueDataType, 0x420059)                                                         \
   KMS_X (OpaqueDataValue, 0x42005A)                                                        \
   KMS_X (OpaqueObject, 0x42005B)                                                           \
   KMS_X (Operation, 0x42005C)                                                              \
   KMS_X (OperationPolicyName, 0x42005D) /* deprecated */                                   \
   KMS_X (P, 0x42005E)                                                                      \
   KMS_X (PaddingMethod, 0x42005F)                                                          \
   KMS_X (PrimeExponentP, 0x420060)                                                         \
   KMS_X (PrimeExponentQ, 0x420061)                                                         \
   KMS_X (PrimeFieldSize, 0x420062)                                                         \
   KMS_X (PrivateExponent, 0x420063)                                                        \
   KMS_X (PrivateKey, 0x420064)                                                             \
   KMS_X (PrivateKeyTemplateAttribute, 0x420065)                                            \
   KMS_X (PrivateKeyUniqueIdentifier, 0x420066)                                             \
   KMS_X (ProcessStartDate, 0x420067)                                                       \
   KMS_X (ProtectStopDate, 0x420068)                                                        \
   KMS_X (ProtocolVersion, 0x420069)                                                        \
   KMS_X (ProtocolVersionMajor, 0x42006A)                                                   \
   KMS_X (ProtocolVersionMinor, 0x42006B)                                                   \
   KMS_X (PublicExponent, 0x42006C)                                                         \
   KMS_X (PublicKey, 0x42006D)                                                              \
   KMS_X (PublicKeyTemplateAttribute, 0x42006E)                                             \
   KMS_X (PublicKeyUniqueIdentifier, 0x42006F)                                              \
   KMS_X (PutFunction, 0x420070)                                                            \
   KMS_X (Q, 0x420071)                                                                      \
   KMS_X (QString, 0x420072)                                                                \
   KMS_X (Qlength, 0x420073)                                                                \
   KMS_X (QueryFunction, 0x420074)                                                          \
   KMS_X (RecommendedCurve, 0x420075)                                                       \
   KMS_X (ReplacedUniqueIdentifier, 0x420076)                                               \
   KMS_X (RequestHeader, 0x420077)                                                          \
   KMS_X (RequestMessage, 0x420078)                                                         \
   KMS_X (RequestPayload, 0x420079)                                                         \
   KMS_X (ResponseHeader, 0x42007A)                                                         \
   KMS_X (ResponseMessage, 0x42007B)                                                        \
   KMS_X (ResponsePayload, 0x42007C)                                                        \
   KMS_X (ResultMessage, 0x42007D)                                                          \
   KMS_X (ResultReason, 0x42007E)                                                           \
   KMS_X (ResultStatus, 0x42007F)                                                           \
   KMS_X (RevocationMessage, 0x420080)                                                      \
   KMS_X (RevocationReason, 0x420081)                                                       \
   KMS_X (RevocationReasonCode, 0x420082)                                                   \
   KMS_X (KeyRoleType, 0x420083)                                                            \
   KMS_X (Salt, 0x420084)                                                                   \
   KMS_X (SecretData, 0x420085)                                                             \
   KMS_X (SecretDataType, 0x420086)                                                         \
   KMS_X (SerialNumber, 0x420087) /* deprecated as of version 1.1 */                        \
   KMS_X (ServerInformation, 0x420088)                                                      \
   KMS_X (SplitKey, 0x420089)                                                               \
   KMS_X (SplitKeyMethod, 0x42008A)                                                         \
   KMS_X (SplitKeyParts, 0x42008B)                                                          \
   KMS_X (SplitKeyThreshold, 0x42008C)                                                      \
   KMS_X (State, 0x42008D)                                                                  \
   KMS_X (StorageStatusMask, 0x42008E)                                                      \
   KMS_X (SymmetricKey, 0x42008F)                                                           \
   KMS_X (Template, 0x420090)                                                               \
   KMS_X (TemplateAttribute, 0x420091)                                                      \
   KMS_X (TimeStamp, 0x420092)                                                              \
   KMS_X (UniqueBatchItemID, 0x420093)                                                      \
   KMS_X (UniqueIdentifier, 0x420094)                                                       \
   KMS_X (UsageLimits, 0x420095)                                                            \
   KMS_X (UsageLimitsCount, 0x420096)                                                       \
   KMS_X (UsageLimitsTotal, 0x420097)                                                       \
   KMS_X (UsageLimitsUnit, 0x420098)                                                        \
   KMS_X (Username, 0x420099)                                                               \
   KMS_X (ValidityDate, 0x42009A)                                                           \
   KMS_X (ValidityIndicator, 0x42009B)                                                      \
   KMS_X (VendorExtension, 0x42009C)                                                        \
   KMS_X (VendorIdentification, 0x42009D)                                                   \
   KMS_X (WrappingMethod, 0x42009E)                                                         \
   KMS_X (X, 0x42009F)                                                                      \
   KMS_X (Y, 0x4200A0)                                                                      \
   KMS_X (Password, 0x4200A1)                                                               \
   KMS_X (DeviceIdentifier, 0x4200A2)                                                       \
   KMS_X (EncodingOption, 0x4200A3)                                                         \
   KMS_X (ExtensionInformation, 0x4200A4)                                                   \
   KMS_X (ExtensionName, 0x4200A5)                                                          \
   KMS_X (ExtensionTag, 0x4200A6)                                                           \
   KMS_X (ExtensionType, 0x4200A7)                                                          \
   KMS_X (Fresh, 0x4200A8)                                                                  \
   KMS_X (MachineIdentifier, 0x4200A9)                                                      \
   KMS_X (MediaIdentifier, 0x4200AA)                                                        \
   KMS_X (NetworkIdentifier, 0x4200AB)                                                      \
   KMS_X (ObjectGroupMember, 0x4200AC)                                                      \
   KMS_X (CertificateLength, 0x4200AD)                                                      \
   KMS_X (DigitalSignatureAlgorithm, 0x4200AE)                                              \
   KMS_X (CertificateSerialNumber, 0x4200AF)                                                \
   KMS_X (DeviceSerialNumber, 0x4200B0)                                                     \
   KMS_X (IssuerAlternativeName, 0x4200B1)                                                  \
   KMS_X (IssuerDistinguishedName, 0x4200B2)                                                \
   KMS_X (SubjectAlternativeName, 0x4200B3)                                                 \
   KMS_X (SubjectDistinguishedName, 0x4200B4)                                               \
   KMS_X (X509CertificateIdentifier, 0x4200B5)                                              \
   KMS_X (X509CertificateIssuer, 0x4200B6)                                                  \
   KMS_X (X509CertificateSubject, 0x4200B7)                                                 \
   KMS_X (KeyValueLocation, 0x4200B8)                                                       \
   KMS_X (KeyValueLocationValue, 0x4200B9)                                                  \
   KMS_X (KeyValueLocationType, 0x4200BA)                                                   \
   KMS_X (KeyValuePresent, 0x4200BB)                                                        \
   KMS_X (OriginalCreationDate, 0x4200BC)                                                   \
   KMS_X (PGPKey, 0x4200BD)                                                                 \
   KMS_X (PGPKeyVersion, 0x4200BE)                                                          \
   KMS_X (AlternativeName, 0x4200BF)                                                        \
   KMS_X (AlternativeNameValue, 0x4200C0)                                                   \
   KMS_X (AlternativeNameType, 0x4200C1)                                                    \
   KMS_X (Data, 0x4200C2)                                                                   \
   KMS_X (SignatureData, 0x4200C3)                                                          \
   KMS_X (DataLength, 0x4200C4)                                                             \
   KMS_X (RandomIV, 0x4200C5)                                                               \
   KMS_X (MACData, 0x4200C6)                                                                \
   KMS_X (AttestationType, 0x4200C7)                                                        \
   KMS_X (Nonce, 0x4200C8)                                                                  \
   KMS_X (NonceID, 0x4200C9)                                                                \
   KMS_X (NonceValue, 0x4200CA)                                                             \
   KMS_X (AttestationMeasurement, 0x4200CB)                                                 \
   KMS_X (AttestationAssertion, 0x4200CC)                                                   \
   KMS_X (IVLength, 0x4200CD)                                                               \
   KMS_X (TagLength, 0x4200CE)                                                              \
   KMS_X (FixedFieldLength, 0x4200CF)                                                       \
   KMS_X (CounterLength, 0x4200D0)                                                          \
   KMS_X (InitialCounterValue, 0x4200D1)                                                    \
   KMS_X (InvocationFieldLength, 0x4200D2)                                                  \
   KMS_X (AttestationCapableIndicator, 0x4200D3)                                            \
   KMS_X (OffsetItems, 0x4200D4)                                                            \
   KMS_X (LocatedItems, 0x4200D5)                                                           \
   KMS_X (CorrelationValue, 0x4200D6)                                                       \
   KMS_X (InitIndicator, 0x4200D7)                                                          \
   KMS_X (FinalIndicator, 0x4200D8)                                                         \
   KMS_X (RNGParameters, 0x4200D9)                                                          \
   KMS_X (RNGAlgorithm, 0x4200DA)                                                           \
   KMS_X (DRBGAlgorithm, 0x4200DB)                                                          \
   KMS_X (FIPS186Variation, 0x4200DC)                                                       \
   KMS_X (PredictionResistance, 0x4200DD)                                                   \
   KMS_X (RandomNumberGenerator, 0x4200DE)                                                  \
   KMS_X (ValidationInformation, 0x4200DF)                                                  \
   KMS_X (ValidationAuthorityType, 0x4200E0)                                                \
   KMS_X (ValidationAuthorityCountry, 0x4200E1)                                             \
   KMS_X (ValidationAuthorityURI, 0x4200E2)                                                 \
   KMS_X (ValidationVersionMajor, 0x4200E3)                                                 \
   KMS_X (ValidationVersionMinor, 0x4200E4)                                                 \
   KMS_X (ValidationType, 0x4200E5)                                                         \
   KMS_X (ValidationLevel, 0x4200E6)                                                        \
   KMS_X (ValidationCertificateIdentifier, 0x4200E7)                                        \
   KMS_X (ValidationCertificateURI, 0x4200E8)                                               \
   KMS_X (ValidationVendorURI, 0x4200E9)                                                    \
   KMS_X (ValidationProfile, 0x4200EA)                                                      \
   KMS_X (ProfileInformation, 0x4200EB)                                                     \
   KMS_X (ProfileName, 0x4200EC)                                                            \
   KMS_X (ServerURI, 0x4200ED)                                                              \
   KMS_X (ServerPort, 0x4200EE)                                                             \
   KMS_X (StreamingCapability, 0x4200EF)                                                    \
   KMS_X (AsynchronousCapability, 0x4200F0)                                                 \
   KMS_X (AttestationCapability, 0x4200F1)                                                  \
   KMS_X (UnwrapMode, 0x4200F2)                                                             \
   KMS_X (DestroyAction, 0x4200F3)                                                          \
   KMS_X (ShreddingAlgorithm, 0x4200F4)                                                     \
   KMS_X (RNGMode, 0x4200F5)                                                                \
   KMS_X (ClientRegistrationMethod, 0x4200F6)                                               \
   KMS_X (CapabilityInformation, 0x4200F7)                                                  \
   KMS_X (KeyWrapType, 0x4200F8)                                                            \
   KMS_X (BatchUndoCapability, 0x4200F9)                                                    \
   KMS_X (BatchContinueCapability, 0x4200FA)                                                \
   KMS_X (PKCS12FriendlyName, 0x4200FB)                                                     \
   KMS_X (Description, 0x4200FC)                                                            \
   KMS_X (Comment, 0x4200FD)                                                                \
   KMS_X (AuthenticatedEncryptionAdditionalData, 0x4200FE)                                  \
   KMS_X (AuthenticatedEncryptionTag, 0x4200FF)                                             \
   KMS_X (SaltLength, 0x420100)                                                             \
   KMS_X (MaskGenerator, 0x420101)                                                          \
   KMS_X (MaskGeneratorHashingAlgorithm, 0x420102)                                          \
   KMS_X (PSource, 0x420103)                                                                \
   KMS_X (TrailerField, 0x420104)                                                           \
   KMS_X (ClientCorrelationValue, 0x420105)                                                 \
   KMS_X (ServerCorrelationValue, 0x420106)                                                 \
   KMS_X (DigestedData, 0x420107)                                                           \
   KMS_X (CertificateSubjectCN, 0x420108)                                                   \
   KMS_X (CertificateSubjectO, 0x420109)                                                    \
   KMS_X (CertificateSubjectOU, 0x42010A)                                                   \
   KMS_X (CertificateSubjectEmail, 0x42010B)                                                \
   KMS_X (CertificateSubjectC, 0x42010C)                                                    \
   KMS_X (CertificateSubjectST, 0x42010D)                                                   \
   KMS_X (CertificateSubjectL, 0x42010E)                                                    \
   KMS_X (CertificateSubjectUID, 0x42010F)                                                  \
   KMS_X (CertificateSubjectSerialNumber, 0x420110)                                         \
   KMS_X (CertificateSubjectTitle, 0x420111)                                                \
   KMS_X (CertificateSubjectDC, 0x420112)                                                   \
   KMS_X (CertificateSubjectDNQualifier, 0x420113)                                          \
   KMS_X (CertificateIssuerCN, 0x420114)                                                    \
   KMS_X (CertificateIssuerO, 0x420115)                                                     \
   KMS_X (CertificateIssuerOU, 0x420116)                                                    \
   KMS_X (CertificateIssuerEmail, 0x420117)                                                 \
   KMS_X (CertificateIssuerC, 0x420118)                                                     \
   KMS_X (CertificateIssuerST, 0x420119)                                                    \
   KMS_X (CertificateIssuerL, 0x42011A)                                                     \
   KMS_X (CertificateIssuerUID, 0x42011B)                                                   \
   KMS_X (CertificateIssuerSerialNumber, 0x42011C)                                          \
   KMS_X (CertificateIssuerTitle, 0x42011D)                                                 \
   KMS_X (CertificateIssuerDC, 0x42011E)                                                    \
   KMS_X (CertificateIssuerDNQualifier, 0x42011F)                                           \
   KMS_X (Sensitive, 0x420120)                                                              \
   KMS_X (AlwaysSensitive, 0x420121)                                                        \
   KMS_X (Extractable, 0x420122)                                                            \
   KMS_X (NeverExtractable, 0x420123)                                                       \
   KMS_X_LAST (ReplaceExisting, 0x420124)
/* clang-format on */

/* Generate an enum with each tag value. */
#define KMS_X(TAG, VAL) KMIP_TAG_##TAG = VAL,
#define KMS_X_LAST(TAG, VAL) KMIP_TAG_##TAG = VAL
typedef enum { KMS_XMACRO } kmip_tag_type_t;
#undef KMS_X
#undef KMS_X_LAST

#define KMS_X(TAG, VAL) \
   case KMIP_TAG_##TAG: \
      return #TAG;
#define KMS_X_LAST KMS_X
static KMS_MSG_INLINE const char *
kmip_tag_to_string (kmip_tag_type_t tag)
{
   switch (tag) {
   default:
      return "Unknown KMIP tag";
      KMS_XMACRO
   }
}
#undef KMS_X
#undef KMS_X_LAST

#undef KMS_XMACRO

#endif /* KMS_KMIP_TAG_TYPE_PRIVATE_H */
