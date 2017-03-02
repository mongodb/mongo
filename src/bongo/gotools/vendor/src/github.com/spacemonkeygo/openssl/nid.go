// Copyright (C) 2014 Ryan Hileman
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package openssl

type NID int

const (
	NID_rsadsi                             NID = 1
	NID_pkcs                               NID = 2
	NID_md2                                NID = 3
	NID_md5                                NID = 4
	NID_rc4                                NID = 5
	NID_rsaEncryption                      NID = 6
	NID_md2WithRSAEncryption               NID = 7
	NID_md5WithRSAEncryption               NID = 8
	NID_pbeWithMD2AndDES_CBC               NID = 9
	NID_pbeWithMD5AndDES_CBC               NID = 10
	NID_X500                               NID = 11
	NID_X509                               NID = 12
	NID_commonName                         NID = 13
	NID_countryName                        NID = 14
	NID_localityName                       NID = 15
	NID_stateOrProvinceName                NID = 16
	NID_organizationName                   NID = 17
	NID_organizationalUnitName             NID = 18
	NID_rsa                                NID = 19
	NID_pkcs7                              NID = 20
	NID_pkcs7_data                         NID = 21
	NID_pkcs7_signed                       NID = 22
	NID_pkcs7_enveloped                    NID = 23
	NID_pkcs7_signedAndEnveloped           NID = 24
	NID_pkcs7_digest                       NID = 25
	NID_pkcs7_encrypted                    NID = 26
	NID_pkcs3                              NID = 27
	NID_dhKeyAgreement                     NID = 28
	NID_des_ecb                            NID = 29
	NID_des_cfb64                          NID = 30
	NID_des_cbc                            NID = 31
	NID_des_ede                            NID = 32
	NID_des_ede3                           NID = 33
	NID_idea_cbc                           NID = 34
	NID_idea_cfb64                         NID = 35
	NID_idea_ecb                           NID = 36
	NID_rc2_cbc                            NID = 37
	NID_rc2_ecb                            NID = 38
	NID_rc2_cfb64                          NID = 39
	NID_rc2_ofb64                          NID = 40
	NID_sha                                NID = 41
	NID_shaWithRSAEncryption               NID = 42
	NID_des_ede_cbc                        NID = 43
	NID_des_ede3_cbc                       NID = 44
	NID_des_ofb64                          NID = 45
	NID_idea_ofb64                         NID = 46
	NID_pkcs9                              NID = 47
	NID_pkcs9_emailAddress                 NID = 48
	NID_pkcs9_unstructuredName             NID = 49
	NID_pkcs9_contentType                  NID = 50
	NID_pkcs9_messageDigest                NID = 51
	NID_pkcs9_signingTime                  NID = 52
	NID_pkcs9_countersignature             NID = 53
	NID_pkcs9_challengePassword            NID = 54
	NID_pkcs9_unstructuredAddress          NID = 55
	NID_pkcs9_extCertAttributes            NID = 56
	NID_netscape                           NID = 57
	NID_netscape_cert_extension            NID = 58
	NID_netscape_data_type                 NID = 59
	NID_des_ede_cfb64                      NID = 60
	NID_des_ede3_cfb64                     NID = 61
	NID_des_ede_ofb64                      NID = 62
	NID_des_ede3_ofb64                     NID = 63
	NID_sha1                               NID = 64
	NID_sha1WithRSAEncryption              NID = 65
	NID_dsaWithSHA                         NID = 66
	NID_dsa_2                              NID = 67
	NID_pbeWithSHA1AndRC2_CBC              NID = 68
	NID_id_pbkdf2                          NID = 69
	NID_dsaWithSHA1_2                      NID = 70
	NID_netscape_cert_type                 NID = 71
	NID_netscape_base_url                  NID = 72
	NID_netscape_revocation_url            NID = 73
	NID_netscape_ca_revocation_url         NID = 74
	NID_netscape_renewal_url               NID = 75
	NID_netscape_ca_policy_url             NID = 76
	NID_netscape_ssl_server_name           NID = 77
	NID_netscape_comment                   NID = 78
	NID_netscape_cert_sequence             NID = 79
	NID_desx_cbc                           NID = 80
	NID_id_ce                              NID = 81
	NID_subject_key_identifier             NID = 82
	NID_key_usage                          NID = 83
	NID_private_key_usage_period           NID = 84
	NID_subject_alt_name                   NID = 85
	NID_issuer_alt_name                    NID = 86
	NID_basic_constraints                  NID = 87
	NID_crl_number                         NID = 88
	NID_certificate_policies               NID = 89
	NID_authority_key_identifier           NID = 90
	NID_bf_cbc                             NID = 91
	NID_bf_ecb                             NID = 92
	NID_bf_cfb64                           NID = 93
	NID_bf_ofb64                           NID = 94
	NID_mdc2                               NID = 95
	NID_mdc2WithRSA                        NID = 96
	NID_rc4_40                             NID = 97
	NID_rc2_40_cbc                         NID = 98
	NID_givenName                          NID = 99
	NID_surname                            NID = 100
	NID_initials                           NID = 101
	NID_uniqueIdentifier                   NID = 102
	NID_crl_distribution_points            NID = 103
	NID_md5WithRSA                         NID = 104
	NID_serialNumber                       NID = 105
	NID_title                              NID = 106
	NID_description                        NID = 107
	NID_cast5_cbc                          NID = 108
	NID_cast5_ecb                          NID = 109
	NID_cast5_cfb64                        NID = 110
	NID_cast5_ofb64                        NID = 111
	NID_pbeWithMD5AndCast5_CBC             NID = 112
	NID_dsaWithSHA1                        NID = 113
	NID_md5_sha1                           NID = 114
	NID_sha1WithRSA                        NID = 115
	NID_dsa                                NID = 116
	NID_ripemd160                          NID = 117
	NID_ripemd160WithRSA                   NID = 119
	NID_rc5_cbc                            NID = 120
	NID_rc5_ecb                            NID = 121
	NID_rc5_cfb64                          NID = 122
	NID_rc5_ofb64                          NID = 123
	NID_rle_compression                    NID = 124
	NID_zlib_compression                   NID = 125
	NID_ext_key_usage                      NID = 126
	NID_id_pkix                            NID = 127
	NID_id_kp                              NID = 128
	NID_server_auth                        NID = 129
	NID_client_auth                        NID = 130
	NID_code_sign                          NID = 131
	NID_email_protect                      NID = 132
	NID_time_stamp                         NID = 133
	NID_ms_code_ind                        NID = 134
	NID_ms_code_com                        NID = 135
	NID_ms_ctl_sign                        NID = 136
	NID_ms_sgc                             NID = 137
	NID_ms_efs                             NID = 138
	NID_ns_sgc                             NID = 139
	NID_delta_crl                          NID = 140
	NID_crl_reason                         NID = 141
	NID_invalidity_date                    NID = 142
	NID_sxnet                              NID = 143
	NID_pbe_WithSHA1And128BitRC4           NID = 144
	NID_pbe_WithSHA1And40BitRC4            NID = 145
	NID_pbe_WithSHA1And3_Key_TripleDES_CBC NID = 146
	NID_pbe_WithSHA1And2_Key_TripleDES_CBC NID = 147
	NID_pbe_WithSHA1And128BitRC2_CBC       NID = 148
	NID_pbe_WithSHA1And40BitRC2_CBC        NID = 149
	NID_keyBag                             NID = 150
	NID_pkcs8ShroudedKeyBag                NID = 151
	NID_certBag                            NID = 152
	NID_crlBag                             NID = 153
	NID_secretBag                          NID = 154
	NID_safeContentsBag                    NID = 155
	NID_friendlyName                       NID = 156
	NID_localKeyID                         NID = 157
	NID_x509Certificate                    NID = 158
	NID_sdsiCertificate                    NID = 159
	NID_x509Crl                            NID = 160
	NID_pbes2                              NID = 161
	NID_pbmac1                             NID = 162
	NID_hmacWithSHA1                       NID = 163
	NID_id_qt_cps                          NID = 164
	NID_id_qt_unotice                      NID = 165
	NID_rc2_64_cbc                         NID = 166
	NID_SMIMECapabilities                  NID = 167
	NID_pbeWithMD2AndRC2_CBC               NID = 168
	NID_pbeWithMD5AndRC2_CBC               NID = 169
	NID_pbeWithSHA1AndDES_CBC              NID = 170
	NID_ms_ext_req                         NID = 171
	NID_ext_req                            NID = 172
	NID_name                               NID = 173
	NID_dnQualifier                        NID = 174
	NID_id_pe                              NID = 175
	NID_id_ad                              NID = 176
	NID_info_access                        NID = 177
	NID_ad_OCSP                            NID = 178
	NID_ad_ca_issuers                      NID = 179
	NID_OCSP_sign                          NID = 180
)
