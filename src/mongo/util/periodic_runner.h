/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <functional>
#include <memory>
#include <string>

#include <boost/optional.hpp>

#include "mongo/platform/mutex.h"
#include "mongo/util/time_support.h"

namespace mongo {

class Client;
class PeriodicJobAnchor;

/**
 * An interface for objects that run work items at specified intervals. Each individually scheduled
 * job will be called in series.
 *
 * Implementations may use whatever internal threading and eventing
 * model they wish. Implementations may choose when to stop running
 * scheduled jobs (for example, some implementations may stop running
 * when the server is in global shutdown).
 *
 * The runner will create client objects that it passes to jobs to use.
 */
class PeriodicRunner {
public:
    using Job = std::function<void(Client* client)>;
    using JobAnchor = PeriodicJobAnchor;

    struct PeriodicJob {
        PeriodicJob(std::string name, Job callable, Milliseconds period)
            : name(std::move(name)), job(std::move(callable)), interval(period) {}

        /**
         * name of the job
         */
        std::string name;

        /**
         * A task to be run at regular intervals by the runner.
         */
        Job job;

        /**
         * An interval at which the job should be run.
         */
        Milliseconds interval;
    };

    /**
     * A ControllableJob allows a user to reschedule the execution of a Job
     */
    class ControllableJob {
    public:
        virtual ~ControllableJob() = default;

        /**
         * Starts running the job
         */
        virtual void start() = 0;

        /**
         * Pauses the job temporarily so that it does not execute until
         * unpaused
         */
        virtual void pause() = 0;

        /**
         * Resumes a paused job so that it continues executing each interval
         */
        virtual void resume() = 0;

        /**
         * Stops the job, this function blocks until the job is stopped
         * Safe to invalidate the job callable after calling this.
         */
        virtual void stop() = 0;

        /**
         * Returns the current period for the job
         */
        virtual Milliseconds getPeriod() = 0;

        /**
         * Updates the period of the job.  This takes effect immediately by altering the current
         * scheduling of the task.  I.e. if more than ms have passed since the last execution of the
         * job, it is run immediately.  Otherwise the scheduling is adjusted forward or back by
         * abs(new - old).
         */
        virtual void setPeriod(Milliseconds ms) = 0;
    };

    virtual ~PeriodicRunner();

    /**
     * Creates a new job and adds it to the runner, but does not schedule it.
     * The caller is responsible for calling 'start' on the resulting handle in
     * order to begin the job running. This API should be used when the caller
     * is interested in observing and controlling the job execution state.
     */
    virtual JobAnchor makeJob(PeriodicJob job) = 0;
};

/**
 * A PeriodicJobAnchor allows the holder to control the scheduling of a job for the lifetime of the
 * anchor. When an anchor is destructed, it stops its underlying job.
 *
 * The underlying weak_ptr for this class is not synchronized. In essence, treat use of this class
 * as if it were a raw pointer to a ControllableJob.
 *
 * Each wrapped PeriodicRunner::ControllableJob function on this object throws
 * if the underlying job is gone (e.g. in shutdown).
 */
class [[nodiscard]] PeriodicJobAnchor {
public:
    using Job = PeriodicRunner::ControllableJob;

public:
    // Note that this constructor is only intended for use with PeriodicRunner::makeJob()
    explicit PeriodicJobAnchor(std::shared_ptr<Job> handle);

    PeriodicJobAnchor() = default;
    PeriodicJobAnchor(PeriodicJobAnchor&&) = default;
    PeriodicJobAnchor& operator=(PeriodicJobAnchor&&) = default;

    PeriodicJobAnchor(const PeriodicJobAnchor&) = delete;
    PeriodicJobAnchor& operator=(const PeriodicJobAnchor&) = delete;

    ~PeriodicJobAnchor();

    void start();
    void pause();
    void resume();
    void stop();
    void setPeriod(Milliseconds ms);
    Milliseconds getPeriod();

    /**
     * Abandon responsibility for scheduling the execution of this job
     *
     * This effectively invalidates the anchor.
     */
    void detach();

    /**
     * Returns if this PeriodicJobAnchor is associated with a PeriodicRunner::ControllableJob
     *
     * This function is useful to see if a PeriodicJobAnchor is initialized. It does not necessarily
     * inform whether a PeriodicJobAnchor will throw from a control function above.
     */
    bool isValid() const noexcept;

    explicit operator bool() const noexcept {
        return isValid();
    }

private:
    std::shared_ptr<Job> _handle;
};

}  // namespace mongo
