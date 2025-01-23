/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/io/ChannelHandler.h>

#include <chrono>

namespace Aws
{
    namespace Crt
    {
        namespace Io
        {
            int ChannelHandler::s_ProcessReadMessage(
                struct aws_channel_handler *handler,
                struct aws_channel_slot *,
                struct aws_io_message *message)
            {
                auto *channelHandler = reinterpret_cast<ChannelHandler *>(handler->impl);

                return channelHandler->ProcessReadMessage(message);
            }

            int ChannelHandler::s_ProcessWriteMessage(
                struct aws_channel_handler *handler,
                struct aws_channel_slot *,
                struct aws_io_message *message)
            {
                auto *channelHandler = reinterpret_cast<ChannelHandler *>(handler->impl);

                return channelHandler->ProcessWriteMessage(message);
            }

            int ChannelHandler::s_IncrementReadWindow(
                struct aws_channel_handler *handler,
                struct aws_channel_slot *,
                size_t size)
            {
                auto *channelHandler = reinterpret_cast<ChannelHandler *>(handler->impl);

                return channelHandler->IncrementReadWindow(size);
            }

            int ChannelHandler::s_ProcessShutdown(
                struct aws_channel_handler *handler,
                struct aws_channel_slot *,
                enum aws_channel_direction dir,
                int errorCode,
                bool freeScarceResourcesImmediately)
            {
                auto *channelHandler = reinterpret_cast<ChannelHandler *>(handler->impl);

                channelHandler->ProcessShutdown(
                    static_cast<ChannelDirection>(dir), errorCode, freeScarceResourcesImmediately);
                return AWS_OP_SUCCESS;
            }

            size_t ChannelHandler::s_InitialWindowSize(struct aws_channel_handler *handler)
            {
                auto *channelHandler = reinterpret_cast<ChannelHandler *>(handler->impl);
                return channelHandler->InitialWindowSize();
            }

            size_t ChannelHandler::s_MessageOverhead(struct aws_channel_handler *handler)
            {
                auto *channelHandler = reinterpret_cast<ChannelHandler *>(handler->impl);
                return channelHandler->MessageOverhead();
            }

            void ChannelHandler::s_ResetStatistics(struct aws_channel_handler *handler)
            {
                auto *channelHandler = reinterpret_cast<ChannelHandler *>(handler->impl);
                channelHandler->ResetStatistics();
            }

            void ChannelHandler::s_GatherStatistics(
                struct aws_channel_handler *handler,
                struct aws_array_list *statsList)
            {
                auto *channelHandler = reinterpret_cast<ChannelHandler *>(handler->impl);
                channelHandler->GatherStatistics(statsList);
            }

            void ChannelHandler::s_Destroy(struct aws_channel_handler *handler)
            {
                auto *channelHandler = reinterpret_cast<ChannelHandler *>(handler->impl);
                channelHandler->m_selfReference = nullptr;
            }

            struct aws_channel_handler_vtable ChannelHandler::s_vtable = {
                s_ProcessReadMessage,
                s_ProcessWriteMessage,
                s_IncrementReadWindow,
                s_ProcessShutdown,
                s_InitialWindowSize,
                s_MessageOverhead,
                ChannelHandler::s_Destroy,
                s_ResetStatistics,
                s_GatherStatistics,
            };

            ChannelHandler::ChannelHandler(Allocator *allocator) : m_allocator(allocator)
            {
                AWS_ZERO_STRUCT(m_handler);
                m_handler.alloc = allocator;
                m_handler.impl = reinterpret_cast<void *>(this);
                m_handler.vtable = &ChannelHandler::s_vtable;
            }

            struct aws_channel_handler *ChannelHandler::SeatForCInterop(const std::shared_ptr<ChannelHandler> &selfRef)
            {
                AWS_FATAL_ASSERT(this == selfRef.get());
                m_selfReference = selfRef;
                return &m_handler;
            }

            struct aws_io_message *ChannelHandler::AcquireMessageFromPool(MessageType messageType, size_t sizeHint)
            {
                return aws_channel_acquire_message_from_pool(
                    GetSlot()->channel, static_cast<aws_io_message_type>(messageType), sizeHint);
            }

            struct aws_io_message *ChannelHandler::AcquireMaxSizeMessageForWrite()
            {
                return aws_channel_slot_acquire_max_message_for_write(GetSlot());
            }

            void ChannelHandler::ShutDownChannel(int errorCode)
            {
                aws_channel_shutdown(GetSlot()->channel, errorCode);
            }

            bool ChannelHandler::ChannelsThreadIsCallersThread() const
            {
                return aws_channel_thread_is_callers_thread(GetSlot()->channel);
            }

            bool ChannelHandler::SendMessage(struct aws_io_message *message, ChannelDirection direction)
            {
                return aws_channel_slot_send_message(
                           GetSlot(), message, static_cast<aws_channel_direction>(direction)) == AWS_OP_SUCCESS;
            }

            bool ChannelHandler::IncrementUpstreamReadWindow(size_t windowUpdateSize)
            {
                return aws_channel_slot_increment_read_window(GetSlot(), windowUpdateSize) == AWS_OP_SUCCESS;
            }

            void ChannelHandler::OnShutdownComplete(
                ChannelDirection direction,
                int errorCode,
                bool freeScarceResourcesImmediately)
            {
                aws_channel_slot_on_handler_shutdown_complete(
                    GetSlot(),
                    static_cast<aws_channel_direction>(direction),
                    errorCode,
                    freeScarceResourcesImmediately);
            }

            size_t ChannelHandler::DownstreamReadWindow() const
            {
                if (!GetSlot()->adj_right)
                {
                    return 0;
                }
                return aws_channel_slot_downstream_read_window(GetSlot());
            }

            size_t ChannelHandler::UpstreamMessageOverhead() const
            {
                return aws_channel_slot_upstream_message_overhead(GetSlot());
            }

            struct aws_channel_slot *ChannelHandler::GetSlot() const
            {
                return m_handler.slot;
            }

            struct TaskWrapper
            {
                struct aws_channel_task task
                {
                };
                Allocator *allocator{};
                std::function<void(TaskStatus)> wrappingFn;
            };

            static void s_ChannelTaskCallback(struct aws_channel_task *, void *arg, enum aws_task_status status)
            {
                auto *taskWrapper = reinterpret_cast<TaskWrapper *>(arg);
                taskWrapper->wrappingFn(static_cast<TaskStatus>(status));
                Delete(taskWrapper, taskWrapper->allocator);
            }

            void ChannelHandler::ScheduleTask(std::function<void(TaskStatus)> &&task, std::chrono::nanoseconds run_in)
            {
                auto *wrapper = New<TaskWrapper>(m_allocator);
                wrapper->wrappingFn = std::move(task);
                wrapper->allocator = m_allocator;
                aws_channel_task_init(
                    &wrapper->task, s_ChannelTaskCallback, wrapper, "cpp-crt-custom-channel-handler-task");

                uint64_t currentTimestamp = 0;
                aws_channel_current_clock_time(GetSlot()->channel, &currentTimestamp);
                aws_channel_schedule_task_future(GetSlot()->channel, &wrapper->task, currentTimestamp + run_in.count());
            }

            void ChannelHandler::ScheduleTask(std::function<void(TaskStatus)> &&task)
            {
                auto *wrapper = New<TaskWrapper>(m_allocator);
                wrapper->wrappingFn = std::move(task);
                wrapper->allocator = m_allocator;
                aws_channel_task_init(
                    &wrapper->task, s_ChannelTaskCallback, wrapper, "cpp-crt-custom-channel-handler-task");

                aws_channel_schedule_task_now(GetSlot()->channel, &wrapper->task);
            }

        } // namespace Io
    } // namespace Crt
} // namespace Aws
