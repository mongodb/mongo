#ifndef AWS_S3_REQUEST_H
#define AWS_S3_REQUEST_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/byte_buf.h>
#include <aws/common/linked_list.h>
#include <aws/common/ref_count.h>
#include <aws/common/thread.h>
#include <aws/s3/s3.h>

#include <aws/s3/private/s3_buffer_pool.h>
#include <aws/s3/private/s3_checksums.h>

struct aws_http_message;
struct aws_signable;
struct aws_s3_meta_request;

enum aws_s3_request_flags {
    AWS_S3_REQUEST_FLAG_RECORD_RESPONSE_HEADERS = 0x00000001,
    AWS_S3_REQUEST_FLAG_PART_SIZE_RESPONSE_BODY = 0x00000002,
    AWS_S3_REQUEST_FLAG_ALWAYS_SEND = 0x00000004,
    AWS_S3_REQUEST_FLAG_PART_SIZE_REQUEST_BODY = 0x00000008,
};

/**
 * Information sent in the telemetry_callback after each aws_s3_request finished/retried from meta request.
 */
struct aws_s3_request_metrics {
    struct aws_allocator *allocator;

    struct {
        /* The time stamp when the request started by S3 client, which is prepared time by the client. Timestamps
         * are from `aws_high_res_clock_get_ticks`. This will always be available. */
        int64_t start_timestamp_ns;
        /* The time stamp when the request finished by S3 client succeed or failed or to be retried. Timestamps
         * are from `aws_high_res_clock_get_ticks`. This will always be available. */
        int64_t end_timestamp_ns;
        /* The time duration for the request from start to finish. end_timestamp_ns - start_timestamp_ns. This will
         * always be available. */
        int64_t total_duration_ns;

        /* The time stamp when the request started to be encoded. -1 means data not available. Timestamp
         * are from `aws_high_res_clock_get_ticks` */
        int64_t send_start_timestamp_ns;
        /* The time stamp when the request finished to be encoded. -1 means data not available.
         * Timestamp are from `aws_high_res_clock_get_ticks` */
        int64_t send_end_timestamp_ns;
        /* The time duration for the request from start encoding to finish encoding (send_end_timestamp_ns -
         * send_start_timestamp_ns). When send_end_timestamp_ns is -1, means data not available. */
        int64_t sending_duration_ns;

        /* The time stamp when the response started to be received from the network channel. -1 means data not
         * available. Timestamp are from `aws_high_res_clock_get_ticks` */
        int64_t receive_start_timestamp_ns;
        /* The time stamp when the response finished to be received from the network channel. -1 means data not
         * available. Timestamp are from `aws_high_res_clock_get_ticks` */
        int64_t receive_end_timestamp_ns;
        /* The time duration for the request from start receiving to finish receiving (receive_end_timestamp_ns -
         * receive_start_timestamp_ns). When receive_end_timestamp_ns is 0, means data not available. */
        int64_t receiving_duration_ns;

        /* The time stamp when the request started to be signed. -1 means data not
         * available. Timestamp are from `aws_high_res_clock_get_ticks` */
        int64_t sign_start_timestamp_ns;
        /* The time stamp when the response finished to be signed. -1 means data not
         * available. Timestamp are from `aws_high_res_clock_get_ticks` */
        int64_t sign_end_timestamp_ns;
        /* The time duration for the request from start signing to finish signing (sign_end_timestamp_ns -
         * sign_start_timestamp_ns). When sign_end_timestamp_ns is 0, means data not available. */
        int64_t signing_duration_ns;
    } time_metrics;

    struct {
        /* Response status code for the request */
        int response_status;
        /* HTTP Headers of the response received. */
        struct aws_http_headers *response_headers;
        /* Path and query of the request. */
        struct aws_string *request_path_query;
        /* The host address of the request. */
        struct aws_string *host_address;
        /* The the request ID header value. */
        struct aws_string *request_id;
        /* S3 operation name for the request */
        struct aws_string *operation_name;
        /* The type of request made */
        enum aws_s3_request_type request_type;
    } req_resp_info_metrics;

    struct {
        /* The IP address of the request connected to */
        struct aws_string *ip_address;
        /* The pointer to the connection that request was made from */
        void *connection_id;
        /* The aws_thread_id_t to the thread that request ran on */
        aws_thread_id_t thread_id;
        /* The stream-id, which is the idex when the stream was activated. */
        uint32_t stream_id;
        /* CRT error code when the aws_s3_request finishes. */
        int error_code;
    } crt_info_metrics;

    struct aws_ref_count ref_count;
};

/* Represents a single request made to S3. */
struct aws_s3_request {

    /* Linked list node used for queuing. */
    struct aws_linked_list_node node;

    /* Linked list node used for tracking the request is active from HTTP level. */
    struct aws_linked_list_node cancellable_http_streams_list_node;

    /* The meta request lock must be held to access the data */
    struct {
        /* The underlying http stream, only valid when the request is active from HTTP level */
        struct aws_http_stream *cancellable_http_stream;
    } synced_data;

    /* TODO Ref count on the request is no longer needed--only one part of code should ever be holding onto a request,
     * and we can just transfer ownership.*/
    struct aws_ref_count ref_count;

    struct aws_allocator *allocator;

    /* Owning meta request. */
    struct aws_s3_meta_request *meta_request;

    /* Request body to use when sending the request. The contents of this body will be re-used if a request is
     * retried.*/
    struct aws_byte_buf request_body;

    struct aws_s3_buffer_pool_ticket *ticket;

    /* Beginning range of this part. */
    /* TODO currently only used by auto_range_get, could be hooked up to auto_range_put as well. */
    uint64_t part_range_start;

    /* Last byte of this part.*/
    /* TODO currently only used by auto_range_get, could be hooked up to auto_range_put as well. */
    uint64_t part_range_end;

    /* Part number that this request refers to.  If this is not a part, this can be 0.  (S3 Part Numbers start at 1.)
     * However, must currently be a valid part number (ie: greater than 0) if the response body is to be streamed to the
     * caller.
     */
    uint32_t part_number;

    /* The upload_timeout used. Zero, if the request is not a upload part */
    size_t upload_timeout_ms;

    /* Number of times aws_s3_meta_request_prepare has been called for a request. During the first call to the virtual
     * prepare function, this will be 0.*/
    uint32_t num_times_prepared;

    /* checksum found in the header of an individual get part http request */
    struct aws_byte_buf request_level_response_header_checksum;

    /* running checksum of the response to an individual get part http request */
    struct aws_s3_checksum *request_level_running_response_sum;
    /* The algorithm used to validate the checksum */
    enum aws_s3_checksum_algorithm validation_algorithm;

    /* Get request only, was there a checksum to validate */
    bool did_validate;

    /* Get request only, if there was an attached checksum to validate did it match the computed checksum */
    bool checksum_match;

    /* Tag that defines what the built request will actually consist of.  This is meant to be space for an enum defined
     * by the derived type.  Request tags do not necessarily map 1:1 with actual S3 API requests. (For example, they can
     * be more contextual, like "first part" instead of just "part".) */

    /* TODO: Eliminate the concept of "request tag" and just use request_type.
     * It's confusing having 2 concepts that are so similar.
     * There's only 1 case where 2 tags used the same type,
     * we can use some other bool/flag to differentiate this 1 case. */
    int request_tag;

    /* Actual S3 type for the single request (may be AWS_S3_REQUEST_TYPE_UNKNOWN) */
    enum aws_s3_request_type request_type;

    /* S3 operation name for the single request (e.g. "CompleteMultipartUpload") */
    struct aws_string *operation_name;

    /* Members of this structure will be repopulated each time the request is sent. If the request fails, and needs to
     * be retried, then the members of this structure will be cleaned up and re-populated on the next send.
     */
    /* TODO rename this anonymous structure to something more intuitive. (Maybe "attempt_data")*/
    struct {

        /* The HTTP message to send for this request. */
        struct aws_http_message *message;

        /* Signable created for the above message. */
        struct aws_signable *signable;

        /* Recorded response headers for the request. Set only when the request desc has record_response_headers set to
         * true or when this response indicates an error. */
        struct aws_http_headers *response_headers;

        /* Recorded response body of the request. */
        struct aws_byte_buf response_body;

        /* Returned response status of this request. */
        int response_status;

        /* The metrics for the request telemetry */
        struct aws_s3_request_metrics *metrics;

        /* The request is required to have the unsigned payload */
        uint32_t require_streaming_unsigned_payload_header : 1;
    } send_data;

    /* When true, response headers from the request will be stored in the request's response_headers variable. */
    uint32_t record_response_headers : 1;

    /* When true, the response body buffer will be allocated in the size of a part. */
    uint32_t has_part_size_response_body : 1;

    /* When true, the request body buffer will be allocated in the size of a part. */
    uint32_t has_part_size_request_body : 1;

    /* When true, this request is being tracked by the client for limiting the amount of in-flight-requests/stats. */
    uint32_t tracked_by_client : 1;

    /* When true, even when the meta request has a finish result set, this request will be sent. */
    uint32_t always_send : 1;

    /* When true, this request is intended to find out the object size. This is currently only used by auto_range_get.
     */
    uint32_t discovers_object_size : 1;

    /* When true, this request does not represent a useful http request and
     * must not be sent, however client must still call corresponding finished
     * callback for the request. Those requests can occur when request is
     * optimistically created during update, but cannot be prepared. ex. when
     * put has no content length, requests will be scheduled as regular to
     * ensure fair distribution against other requests, but can also result in
     * requests for uploading data after the end of the stream (those requests
     * will use below flag to indicate that they should not be sent). */
    uint32_t is_noop : 1;

    /* When true, this request has already been uploaded. we still prepare the request to check the durability. */
    uint32_t was_previously_uploaded : 1;
};

AWS_EXTERN_C_BEGIN

/* Create a new s3 request structure with the given options. */
AWS_S3_API
struct aws_s3_request *aws_s3_request_new(
    struct aws_s3_meta_request *meta_request,
    int request_tag,
    enum aws_s3_request_type request_type,
    uint32_t part_number,
    uint32_t flags);

/* Set up the request to be sent. Called each time before the request is sent. Will initially call
 * aws_s3_request_clean_up_send_data to clear out anything previously existing in send_data. */
AWS_S3_API
void aws_s3_request_setup_send_data(struct aws_s3_request *request, struct aws_http_message *message);

/* Clear out send_data members so that they can be repopulated before the next send. */
AWS_S3_API
void aws_s3_request_clean_up_send_data(struct aws_s3_request *request);

AWS_S3_API
struct aws_s3_request *aws_s3_request_acquire(struct aws_s3_request *request);

AWS_S3_API
struct aws_s3_request *aws_s3_request_release(struct aws_s3_request *request);

AWS_S3_API
struct aws_s3_request_metrics *aws_s3_request_metrics_new(
    struct aws_allocator *allocator,
    const struct aws_s3_request *request,
    const struct aws_http_message *message);

AWS_EXTERN_C_END

#endif
