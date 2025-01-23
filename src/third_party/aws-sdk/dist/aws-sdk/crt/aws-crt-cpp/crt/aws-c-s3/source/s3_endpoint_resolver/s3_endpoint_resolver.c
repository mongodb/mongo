/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include "aws/s3/private/s3_endpoint_resolver.h"
#include <aws/s3/s3_endpoint_resolver.h>
#include <aws/sdkutils/endpoints_rule_engine.h>
#include <aws/sdkutils/partitions.h>

struct aws_endpoints_rule_engine *aws_s3_endpoint_resolver_new(struct aws_allocator *allocator) {
    struct aws_endpoints_ruleset *ruleset = NULL;
    struct aws_partitions_config *partitions = NULL;
    struct aws_endpoints_rule_engine *rule_engine = NULL;

    ruleset = aws_endpoints_ruleset_new_from_string(allocator, aws_s3_endpoint_rule_set);
    if (!ruleset) {
        goto cleanup;
    }

    partitions = aws_partitions_config_new_from_string(allocator, aws_s3_endpoint_resolver_partitions);
    if (!partitions) {
        goto cleanup;
    }

    rule_engine = aws_endpoints_rule_engine_new(allocator, ruleset, partitions);

cleanup:
    aws_endpoints_ruleset_release(ruleset);
    aws_partitions_config_release(partitions);
    return rule_engine;
}
