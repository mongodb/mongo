/**
 * Copyright 2011 (c) 10gen Inc.
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
 */

#pragma once

#include "pch.h"
#include "util/string_writer.h"


namespace mongo {

    /*
      This utility class provides an easy way to total up, monitor, warn, and
      signal an error when the amount of memory used for an operation exceeds
      given thresholds.

      Create a local instance of this class, and then inform it of any memory
      that you consume using addToTotal().

      Warnings or errors are issued as usage exceeds certain fractions of
      physical memory on the host, as determined by SystemInfo.

      This class is not guaranteed to warn or signal errors if the host system
      does not support the ability to report its memory, as per the warnings
      for SystemInfo in systeminfo.h.
     */
    class DocMemMonitor {
    public:
        /*
          Constructor.

          Uses default limits for warnings and errors.

          The StringWriter parameter must outlive the DocMemMonitor instance.

          @param pWriter string writer that provides information about the
              operation being monitored
         */
        DocMemMonitor(StringWriter *pWriter);

        /*
          Constructor.

          This variant allows explicit selection of the limits.  Note that
          limits of zero are treated as infinite.

          The StringWriter parameter must outlive the DocMemMonitor instance.

          @param pWriter string writer that provides information about the
              operation being monitored
          @param warnLimit the amount of ram to issue (log) a warning for
          @param errorLimit the amount of ram to throw an error for
         */
        DocMemMonitor(StringWriter *pWriter, size_t warnLimit,
                      size_t errorLimit);

        /*
          Increment the total amount of memory used by the given amount.  If
          the warning threshold is exceeded, a warning will be logged.  If the
          error threshold is exceeded, an error will be thrown.

          @param amount the amount of memory to add to the current total
         */
        void addToTotal(size_t amount);

    private:
        /*
          Real constructor body.

          Provides common construction for all the variant constructors.
         */
        void init(StringWriter *pW, size_t warnLimit, size_t errorLimit);

        bool warned;
        size_t totalUsed;
        size_t warnLimit;
        size_t errorLimit;
        StringWriter *pWriter;
    };

}
