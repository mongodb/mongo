// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/interruptible.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/time_support.h"

#include <string>

#include <boost/optional.hpp>

namespace mongo::transport::grpc {

/**
 * A bidirectional channel with two ends. Each end constitutes a single producer and a single
 * consumer.
 *
 * The "left" and "right" ends of the pipe have identical capabilities.
 */
class BidirectionalPipe {
public:
    class End {
    public:
        /**
         * Attempts to write a message to the pipe, returning true on success.
         * If the pipe is closed before or during the write, this method returns false.
         * If the write is interrupted by the provided Interruptible, this method will throw an
         * exception.
         */
        bool write(ConstSharedBuffer msg,
                   Interruptible* interruptible = Interruptible::notInterruptible()) {
            try {
                auto toWrite = SharedBuffer::allocate(msg.capacity());
                memcpy(toWrite.get(), msg.get(), msg.capacity());
                _sendHalf.push(std::move(toWrite), interruptible);
                return true;
            } catch (DBException& e) {
                if (_isPipeClosedError(e)) {
                    return false;
                }
                throw;
            }
        }

        /**
         * Attempts to read a message to the pipe.
         * If the pipe is closed before or during the read, this method returns
         * boost::optional::none(). If the read is interrupted by the provided Interruptible, this
         * method will throw an exception.
         */
        boost::optional<SharedBuffer> read(
            Interruptible* interruptible = Interruptible::notInterruptible()) {
            try {
                return _recvHalf.pop(interruptible);
            } catch (DBException& e) {
                if (_isPipeClosedError(e)) {
                    return boost::none;
                }
                throw;
            }
        }

        /**
         * Close both the read and write halves of this end of the pipe. In-progress reads and
         * writes on this end and writes on the other end will be interrupted.
         *
         * Messages that have already been transmitted through this end of the pipe can still be
         * read by the other end.
         */
        void close() {
            _sendHalf.close();
            _recvHalf.close();
        }

        /**
         * Close only the writing portion of this end of the pipe. This will cause any reads on
         * the other end to fail once all the previously sent messages have been read. Messages can
         * still be read from this end.
         */
        void closeWriting() {
            _sendHalf.close();
        }

        /**
         * Returns true when at least one of the following conditions is met:
         *  - This end of the pipe is closed.
         *  - The other end of the pipe is closed and there are no more messages to be read.
         */
        bool isConsumed() const {
            auto stats = _recvHalfCtrl.getStats();
            return stats.consumerEndClosed || (stats.queueDepth == 0 && stats.producerEndClosed);
        }

    private:
        friend BidirectionalPipe;

        explicit End(SingleProducerSingleConsumerQueue<SharedBuffer>::Producer send,
                     SingleProducerSingleConsumerQueue<SharedBuffer>::Consumer recv,
                     SingleProducerSingleConsumerQueue<SharedBuffer>::Controller recvCtrl)
            : _sendHalf{std::move(send)},
              _recvHalf{std::move(recv)},
              _recvHalfCtrl(std::move(recvCtrl)) {}

        bool _isPipeClosedError(const DBException& e) const {
            return e.code() == ErrorCodes::ProducerConsumerQueueEndClosed ||
                e.code() == ErrorCodes::ProducerConsumerQueueConsumed;
        }

        SingleProducerSingleConsumerQueue<SharedBuffer>::Producer _sendHalf;
        SingleProducerSingleConsumerQueue<SharedBuffer>::Consumer _recvHalf;
        SingleProducerSingleConsumerQueue<SharedBuffer>::Controller _recvHalfCtrl;
    };

    BidirectionalPipe() {
        SingleProducerSingleConsumerQueue<SharedBuffer>::Pipe pipe1;
        SingleProducerSingleConsumerQueue<SharedBuffer>::Pipe pipe2;

        left = std::unique_ptr<End>(new End(
            std::move(pipe1.producer), std::move(pipe2.consumer), std::move(pipe2.controller)));
        right = std::unique_ptr<End>(new End(
            std::move(pipe2.producer), std::move(pipe1.consumer), std::move(pipe1.controller)));
    }

    std::unique_ptr<End> left;
    std::unique_ptr<End> right;
};
}  // namespace mongo::transport::grpc
