/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <string>

#include "mongo/util/interruptible.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/time_support.h"

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
         * Close both ends of the pipe. In progress reads and writes on either end will be
         * interrupted.
         */
        void close() {
            _sendHalf.close();
            _recvHalf.close();
        }

    private:
        friend BidirectionalPipe;

        explicit End(SingleProducerSingleConsumerQueue<SharedBuffer>::Producer send,
                     SingleProducerSingleConsumerQueue<SharedBuffer>::Consumer recv)
            : _sendHalf{std::move(send)}, _recvHalf{std::move(recv)} {}

        bool _isPipeClosedError(const DBException& e) const {
            return e.code() == ErrorCodes::ProducerConsumerQueueEndClosed ||
                e.code() == ErrorCodes::ProducerConsumerQueueConsumed;
        }

        SingleProducerSingleConsumerQueue<SharedBuffer>::Producer _sendHalf;
        SingleProducerSingleConsumerQueue<SharedBuffer>::Consumer _recvHalf;
    };

    BidirectionalPipe() {
        SingleProducerSingleConsumerQueue<SharedBuffer>::Pipe pipe1;
        SingleProducerSingleConsumerQueue<SharedBuffer>::Pipe pipe2;

        left = std::unique_ptr<End>(new End(std::move(pipe1.producer), std::move(pipe2.consumer)));
        right = std::unique_ptr<End>(new End(std::move(pipe2.producer), std::move(pipe1.consumer)));
    }

    std::unique_ptr<End> left;
    std::unique_ptr<End> right;
};
}  // namespace mongo::transport::grpc
