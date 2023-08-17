/*
 * Copyright 2022-present MongoDB, Inc.
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

#ifndef MC_FLE2_RANGE_OPERATOR_PRIVATE_H
#define MC_FLE2_RANGE_OPERATOR_PRIVATE_H

typedef enum {
    FLE2RangeOperator_kNone = 0,
    FLE2RangeOperator_kGt = 1,
    FLE2RangeOperator_kGte = 2,
    FLE2RangeOperator_kLt = 3,
    FLE2RangeOperator_kLte = 4,
    FLE2RangeOperator_min_val = FLE2RangeOperator_kNone,
    FLE2RangeOperator_max_val = FLE2RangeOperator_kLte
} mc_FLE2RangeOperator_t;

#endif // MC_FLE2_RANGE_OPERATOR_PRIVATE_H
