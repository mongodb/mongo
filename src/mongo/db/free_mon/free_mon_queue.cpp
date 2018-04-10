/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/free_mon/free_mon_queue.h"

#include <chrono>

#include "mongo/util/duration.h"

namespace mongo {

FreeMonMessage::~FreeMonMessage() {}

void FreeMonMessageQueue::enqueue(std::shared_ptr<FreeMonMessage> msg) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        // If we were stopped, drop messages
        if (_stop) {
            return;
        }

        _queue.emplace(msg);

        // Signal the dequeue
        _condvar.notify_one();
    }
}

boost::optional<std::shared_ptr<FreeMonMessage>> FreeMonMessageQueue::dequeue(
    ClockSource* clockSource) {
    {
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        if (_stop) {
            return {};
        }

        Date_t deadlineCV = Date_t::max();
        if (!_queue.empty()) {
            deadlineCV = _queue.top()->getDeadline();
        } else {
            deadlineCV = clockSource->now() + Hours(24);
        }

        _condvar.wait_until(lock, deadlineCV.toSystemTimePoint(), [this, clockSource]() {
            if (_stop) {
                return true;
            }

            if (this->_queue.empty()) {
                return false;
            }

            auto deadlineMessage = this->_queue.top()->getDeadline();
            if (deadlineMessage == Date_t::min()) {
                return true;
            }

            auto now = clockSource->now();

            bool check = deadlineMessage < now;
            return check;
        });

        if (_stop || _queue.empty()) {
            return {};
        }

        auto item = _queue.top();
        _queue.pop();
        return item;
    }
}

void FreeMonMessageQueue::stop() {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        // We can be stopped twice in some situations:
        // 1. Stop on unexpected error
        // 2. Stop on clean shutdown
        if (_stop == false) {
            _stop = true;
            _condvar.notify_one();
        }
    }
}

}  // namespace mongo
