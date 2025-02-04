#ifndef AWS_COMMON_PACKAGE_H
#define AWS_COMMON_PACKAGE_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

/*
 * Preliminary cap on the number of possible aws-c-libraries participating in shared enum ranges for
 * errors, log subjects, and other cross-library enums. Expandable as needed
 */
#define AWS_PACKAGE_SLOTS 32

/*
 * Each aws-c-* and aws-crt-* library has a unique package id starting from zero.  These are used to macro-calculate
 * correct ranges for the cross-library enumerations.
 */
#define AWS_C_COMMON_PACKAGE_ID 0

#endif /* AWS_COMMON_PACKAGE_H */
