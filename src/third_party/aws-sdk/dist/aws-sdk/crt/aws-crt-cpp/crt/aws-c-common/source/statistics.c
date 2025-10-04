/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/statistics.h>

void aws_crt_statistics_handler_process_statistics(
    struct aws_crt_statistics_handler *handler,
    struct aws_crt_statistics_sample_interval *interval,
    struct aws_array_list *stats,
    void *context) {
    handler->vtable->process_statistics(handler, interval, stats, context);
}

uint64_t aws_crt_statistics_handler_get_report_interval_ms(struct aws_crt_statistics_handler *handler) {
    return handler->vtable->get_report_interval_ms(handler);
}

void aws_crt_statistics_handler_destroy(struct aws_crt_statistics_handler *handler) {
    if (handler == NULL) {
        return;
    }

    handler->vtable->destroy(handler);
}
