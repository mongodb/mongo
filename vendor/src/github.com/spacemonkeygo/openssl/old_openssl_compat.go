// Copyright (C) 2014 Space Monkey, Inc.
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

// +build !darwin brew
// +build cgo

package openssl

// #include <openssl/ssl.h>
import "C"

// these constants do not exist in the openssl version packaged with os x. when
// darwin decides to update the base openssl version, we can move these back
// to the appropriate spots in the source. as a workaround, if you need access
// to these constants on darwin, use homebrew or whatever to install a more
// recent version of os x, and build the package with the '-tags brew' flag

const (
	UnsupportedConstraintSyntax VerifyResult = C.X509_V_ERR_UNSUPPORTED_CONSTRAINT_SYNTAX
	UnsupportedConstraintType   VerifyResult = C.X509_V_ERR_UNSUPPORTED_CONSTRAINT_TYPE
	UnsupportedExtensionFeature VerifyResult = C.X509_V_ERR_UNSUPPORTED_EXTENSION_FEATURE
	ExcludedViolation           VerifyResult = C.X509_V_ERR_EXCLUDED_VIOLATION
	SubtreeMinmax               VerifyResult = C.X509_V_ERR_SUBTREE_MINMAX
	UnsupportedNameSyntax       VerifyResult = C.X509_V_ERR_UNSUPPORTED_NAME_SYNTAX
	DifferentCrlScope           VerifyResult = C.X509_V_ERR_DIFFERENT_CRL_SCOPE
	PermittedViolation          VerifyResult = C.X509_V_ERR_PERMITTED_VIOLATION
	CrlPathValidationError      VerifyResult = C.X509_V_ERR_CRL_PATH_VALIDATION_ERROR
)

const (
	NoTLSv1_1 Options = C.SSL_OP_NO_TLSv1_1
	NoTLSv1_2 Options = C.SSL_OP_NO_TLSv1_2
)
