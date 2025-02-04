/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */
#pragma once

/* bit operations on a char[] mask of arbitrary length */
#define S2N_CBIT_BIT(bit)        (1 << ((bit) % 8))
#define S2N_CBIT_BIN(mask, bit)  (mask)[(bit) >> 3]
#define S2N_CBIT_SET(mask, bit)  ((void) (S2N_CBIT_BIN(mask, bit) |= S2N_CBIT_BIT(bit)))
#define S2N_CBIT_CLR(mask, bit)  ((void) (S2N_CBIT_BIN(mask, bit) &= ~S2N_CBIT_BIT(bit)))
#define S2N_CBIT_TEST(mask, bit) ((S2N_CBIT_BIN(mask, bit) & S2N_CBIT_BIT(bit)) != 0)
