#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/Exports.h>
#include <aws/crt/Types.h>
#include <aws/io/channel.h>

#include <chrono>
#include <cstddef>

struct aws_array_list;
struct aws_io_message;

namespace Aws
{
    namespace Crt
    {
        namespace Io
        {
            enum class ChannelDirection
            {
                Read,
                Write,
            };

            enum class MessageType
            {
                ApplicationData,
            };

            enum class TaskStatus
            {
                RunReady,
                Canceled,
            };

            /**
             * Wrapper for aws-c-io channel handlers. The semantics are identical as the functions on
             * aws_channel_handler.
             *
             * All virtual calls are made from the same thread (the channel's thread).
             */
            class AWS_CRT_CPP_API ChannelHandler
            {
              public:
                virtual ~ChannelHandler() = default;

                ChannelHandler(const ChannelHandler &) = delete;
                ChannelHandler &operator=(const ChannelHandler &) = delete;

              protected:
                /**
                 * Called by the channel when a message is available for processing in the read direction. It is your
                 * responsibility to call aws_mem_release(message->allocator, message); on message when you are finished
                 * with it.
                 *
                 * Also keep in mind that your slot's internal window has been decremented. You'll want to call
                 * aws_channel_slot_increment_read_window() at some point in the future if you want to keep receiving
                 * data.
                 *
                 * @return AWS_OP_SUCCESS if the message is being processed.
                 * If the message cannot be processed raise an error and return AWS_OP_ERR
                 * and do NOT release the message, it will be released by the caller.
                 */
                virtual int ProcessReadMessage(struct aws_io_message *message) = 0;

                /**
                 * Called by the channel when a message is available for processing in the write direction. It is your
                 * responsibility to call aws_mem_release(message->allocator, message); on message when you are finished
                 * with it.
                 *
                 * @return AWS_OP_SUCCESS if the message is being processed.
                 * If the message cannot be processed raise an error and return AWS_OP_ERR
                 * and do NOT release the message, it will be released by the caller.
                 */
                virtual int ProcessWriteMessage(struct aws_io_message *message) = 0;

                /**
                 * Called by the channel when a downstream handler has issued a window increment. You'll want to update
                 * your internal state and likely propagate a window increment message of your own by calling
                 * IncrementUpstreamReadWindow()
                 *
                 * @return AWS_OP_SUCCESS if successful.
                 * Otherwise, raise an error and return AWS_OP_ERR.
                 */
                virtual int IncrementReadWindow(size_t size) = 0;

                /**
                 * The channel calls shutdown on all handlers twice, once to shut down reading, and once to shut down
                 * writing. Shutdown always begins with the left-most handler, and proceeds to the right with dir set to
                 * ChannelDirection::Read. Then shutdown is called on handlers from right to left with dir set to
                 * ChannelDirection::Write.
                 *
                 * The shutdown process does not need to complete immediately and may rely on scheduled tasks.
                 * The handler MUST call OnShutdownComplete() when it is finished,
                 * which propagates shutdown to the next handler.  If 'freeScarceResourcesImmediately' is true,
                 * then resources vulnerable to denial-of-service attacks (such as sockets and file handles)
                 * must be closed immediately before the shutdown process complete.
                 */
                virtual void ProcessShutdown(
                    ChannelDirection dir,
                    int errorCode,
                    bool freeScarceResourcesImmediately) = 0;

                /**
                 * Called by the channel when the handler is added to a slot, to get the initial window size.
                 */
                virtual size_t InitialWindowSize() = 0;

                /**
                 * Called by the channel anytime a handler is added or removed, provides a hint for downstream
                 * handlers to avoid message fragmentation due to message overhead.
                 */
                virtual size_t MessageOverhead() = 0;

                /**
                 * Directs the channel handler to reset all of the internal statistics it tracks about itself.
                 */
                virtual void ResetStatistics() {};

                /**
                 * Adds a pointer to the handler's internal statistics (if they exist) to a list of statistics
                 * structures associated with the channel's handler chain.
                 */
                virtual void GatherStatistics(struct aws_array_list *) {}

              public:
                /// @private
                struct aws_channel_handler *SeatForCInterop(const std::shared_ptr<ChannelHandler> &selfRef);

                /**
                 * Return whether the caller is on the same thread as the handler's channel.
                 */
                bool ChannelsThreadIsCallersThread() const;

                /**
                 * Initiate a shutdown of the handler's channel.
                 *
                 * If the channel is already shutting down, this call has no effect.
                 */
                void ShutDownChannel(int errorCode);

                /**
                 * Schedule a task to run on the next "tick" of the event loop.
                 * If the channel is completely shut down, the task will run with the 'Canceled' status.
                 */
                void ScheduleTask(std::function<void(TaskStatus)> &&task);

                /**
                 * Schedule a task to run after a desired length of time has passed.
                 * The task will run with the 'Canceled' status if the channel completes shutdown
                 * before that length of time elapses.
                 */
                void ScheduleTask(std::function<void(TaskStatus)> &&task, std::chrono::nanoseconds run_in);

              protected:
                ChannelHandler(Allocator *allocator = ApiAllocator());

                /**
                 * Acquire an aws_io_message from the channel's pool.
                 */
                struct aws_io_message *AcquireMessageFromPool(MessageType messageType, size_t sizeHint);

                /**
                 * Convenience function that invokes AcquireMessageFromPool(),
                 * asking for the largest reasonable DATA message that can be sent in the write direction,
                 * with upstream overhead accounted for.
                 */
                struct aws_io_message *AcquireMaxSizeMessageForWrite();

                /**
                 * Send a message in the read or write direction.
                 * Returns true if message successfully sent.
                 * If false is returned, you must release the message yourself.
                 */
                bool SendMessage(struct aws_io_message *message, ChannelDirection direction);

                /**
                 * Issue a window update notification upstream.
                 * Returns true if successful.
                 */
                bool IncrementUpstreamReadWindow(size_t windowUpdateSize);

                /**
                 * Must be called by a handler once they have finished their shutdown in the 'dir' direction.
                 * Propagates the shutdown process to the next handler in the channel.
                 */
                void OnShutdownComplete(ChannelDirection direction, int errorCode, bool freeScarceResourcesImmediately);

                /**
                 * Fetches the downstream read window.
                 * This gives you the information necessary to honor the read window.
                 * If you call send_message() and it exceeds this window, the message will be rejected.
                 */
                size_t DownstreamReadWindow() const;

                /**
                 * Fetches the current overhead of upstream handlers.
                 * This provides a hint to avoid fragmentation if you care.
                 */
                size_t UpstreamMessageOverhead() const;

                struct aws_channel_slot *GetSlot() const;

                struct aws_channel_handler m_handler;
                Allocator *m_allocator;

              private:
                std::shared_ptr<ChannelHandler> m_selfReference;
                static struct aws_channel_handler_vtable s_vtable;

                static void s_Destroy(struct aws_channel_handler *handler);
                static int s_ProcessReadMessage(
                    struct aws_channel_handler *,
                    struct aws_channel_slot *,
                    struct aws_io_message *);
                static int s_ProcessWriteMessage(
                    struct aws_channel_handler *,
                    struct aws_channel_slot *,
                    struct aws_io_message *);
                static int s_IncrementReadWindow(struct aws_channel_handler *, struct aws_channel_slot *, size_t size);
                static int s_ProcessShutdown(
                    struct aws_channel_handler *,
                    struct aws_channel_slot *,
                    enum aws_channel_direction,
                    int errorCode,
                    bool freeScarceResourcesImmediately);
                static size_t s_InitialWindowSize(struct aws_channel_handler *);
                static size_t s_MessageOverhead(struct aws_channel_handler *);
                static void s_ResetStatistics(struct aws_channel_handler *);
                static void s_GatherStatistics(struct aws_channel_handler *, struct aws_array_list *statsList);
            };
        } // namespace Io
    } // namespace Crt
} // namespace Aws
