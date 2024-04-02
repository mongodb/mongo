/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "timestamp_manager.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "connection_simulator.h"
#include "error_simulator.h"

timestamp_manager::timestamp_manager() {}

/* Get an instance of timestamp_manager class. */
timestamp_manager &
timestamp_manager::get_timestamp_manager()
{
    static timestamp_manager _timestamp_manager_instance;
    return (_timestamp_manager_instance);
}

uint64_t
timestamp_manager::hex_to_decimal(const std::string &hex_ts)
{
    std::stringstream stream;
    uint64_t ts;

    stream << hex_ts;
    stream >> std::hex >> ts;

    return (ts);
}

std::string
timestamp_manager::decimal_to_hex(const uint64_t ts)
{
    std::stringstream stream;
    stream << std::hex << ts;

    return (stream.str());
}

int
timestamp_manager::validate_hex_value(const std::string &ts_string, const std::string &ts)
{
    /* Protect against unexpectedly long hex strings. */
    if (ts_string.length() > 16)
        WT_SIM_RET_MSG(EINVAL, ts + " timestamp too long: " + ts_string);

    /* Check that the timestamp string has valid hexadecimal characters. */
    for (auto &ch : ts_string)
        if (!std::isxdigit(ch))
            WT_SIM_RET_MSG(EINVAL, "Illegal " + ts + " passed: invalid hex value.");

    if (ts_string == "0")
        WT_SIM_RET_MSG(EINVAL, "Illegal " + ts + ": zero not permitted.");

    return (0);
}

/* Remove leading and trailing spaces from a string. */
std::string
timestamp_manager::trim(std::string str)
{
    str.erase(str.find_last_not_of(" ") + 1);
    str.erase(0, str.find_first_not_of(" "));
    return str;
}

/* Parse config string to a config map. */
int
timestamp_manager::parse_config(const std::string &config,
  const std::vector<std::string> &supported_ops, const std::vector<std::string> &unsupported_ops,
  std::map<std::string, std::string> &config_map)
{
    std::istringstream conf(config);
    std::string token;

    if (config.empty())
        return (0);

    /*
     * Convert the config string to a map. For instance if the config string is 'read_timestamp=15'.
     * Then conversion to a map would look like {'read_timestamp' = '15'}.
     */
    while (std::getline(conf, token, ',')) {
        int pos = token.find('=');
        /* Ignore the string if it's empty. This will occur if extra commas are included. */
        if (token != "") {
            if (pos == -1)
                config_map.insert({trim(token), ""});
            else
                config_map.insert({trim(token.substr(0, pos)), trim(token.substr(pos + 1))});
        }
    }

    /* Get rid of the unsupported ops. */
    for (std::string op : unsupported_ops) {
        auto pos = config_map.find(op);
        if (pos != config_map.end())
            config_map.erase(pos);
    }

    /* Ensure that the elements in the config_map also exist in the supported_ops vector. */
    for (const auto &op : config_map)
        if (std::find(supported_ops.begin(), supported_ops.end(), op.first) == supported_ops.end())
            WT_SIM_RET(EINVAL);

    return (0);
}

/*
 * Validate both oldest and stable timestamps.
 * 1) Validation fails if Illegal timestamp value is passed (if less than or equal to 0).
 *    This check is performed by the validate_hex_value function in this file.
 * 2) It is a no-op to set the oldest or stable timestamps behind the global
 *    values. Hence, ignore and continue validating.
 * 3) Validation fails if oldest is greater than the stable timestamp.
 */
int
timestamp_manager::validate_oldest_and_stable_timestamp(
  uint64_t &new_stable_ts, uint64_t &new_oldest_ts, bool &has_oldest, bool &has_stable)
{
    /* No need to validate timestamps if timestamps are not passed in the config. */
    if (!has_oldest && !has_stable)
        return (0);

    connection_simulator *conn = &connection_simulator::get_connection();

    /* If config has oldest timestamp */
    if (has_oldest) {
        /* It is a no-op to set the new oldest timestamps behind the current oldest timestamp. */
        if (new_oldest_ts <= conn->get_oldest_ts())
            has_oldest = false;
    }

    /* If config has stable timestamp */
    if (has_stable) {
        /* It is a no-op to set the new stable timestamps behind the current stable timestamp. */
        if (new_stable_ts <= conn->get_stable_ts())
            has_stable = false;
    }

    /* No need to validate timestamps if stable or/and oldest were behind the global values. */
    if (!has_oldest && !has_stable)
        return (0);

    /* No need to validate timestamps if there is no new and no current oldest timestamp. */
    if (!has_oldest && conn->get_oldest_ts() == 0)
        return (0);

    /* No need to validate timestamps if there is no new and no current stable timestamp. */
    if (!has_stable && conn->get_stable_ts() == 0)
        return (0);

    /*
     * If the oldest timestamp was not passed in the config or was behind the current oldest
     * timestamp, modify the new_oldest_ts to the current oldest timestamp.
     */
    if ((!has_oldest && conn->get_oldest_ts() != 0))
        new_oldest_ts = conn->get_oldest_ts();

    /*
     * If the stable timestamp was not passed in the config or was behind the current stable
     * timestamp, modify the new_stable_ts to the current stable timestamp
     */
    if ((!has_stable && conn->get_stable_ts() != 0))
        new_stable_ts = conn->get_stable_ts();

    /* Validation fails if oldest is greater than the stable timestamp. */
    if (new_oldest_ts > new_stable_ts)
        WT_SIM_RET_MSG(EINVAL,
          "'oldest timestamp' (" + std::to_string(new_oldest_ts) +
            ") must not be later than 'stable timestamp' (" + std::to_string(new_stable_ts) + ")");

    return (0);
}

/*
 * Validate durable timestamp.
 * 1) Validation fails if Illegal timestamp value is passed (if less than or equal to 0).
 *    This check is performed by the validate_hex_value function in this file.
 */
int
timestamp_manager::validate_conn_durable_timestamp(
  const uint64_t &new_durable_ts, const bool &has_durable) const
{
    /* If durable timestamp was not passed in the config, no validation is needed. */
    if (!has_durable)
        return (0);

    return (0);
}

/*
 * Validate the read timestamp. The constraints on the read timestamp are:
 * 1) The read timestamp can only be set before a transaction is prepared.
 * 2) Read timestamps can only be set once.
 * 3) The read timestamp must be greater than or equal to the oldest timestamp unless rounding
 * the read timestamp is enabled.
 */
int
timestamp_manager::validate_read_timestamp(session_simulator *session, const uint64_t read_ts) const
{
    /*
     * The read timestamp can't be set after a transaction is prepared. However, prepared timestamp
     * can be set before the read timestamp.
     */
    if (session->is_txn_prepared())
        WT_SIM_RET_MSG(EINVAL, "Cannot set a read timestamp after a transaction is prepared.");

    /* Read timestamps can't change once set. */
    if (session->is_read_ts_set())
        WT_SIM_RET_MSG(EINVAL, "A read_timestamp may only be set once per transaction.");

    /*
     * We cannot set the read timestamp to be earlier than the oldest timestamp if we're not
     * rounding to the oldest.
     */
    connection_simulator *conn = &connection_simulator::get_connection();
    if (read_ts < conn->get_oldest_ts())
        if (!session->is_round_read_ts_set())
            WT_SIM_RET_MSG(EINVAL,
              "Cannot set read timestamp before the oldest timestamp, unless we round the read "
              "timestamp up to the oldest.");

    return (0);
}

/*
 * Validate the commit timestamp. The constraints on the read timestamp are:
 * For a non-prepared transaction:
 * - The commit_ts cannot be less than the first_commit_timestamp.
 * - The commit_ts cannot be less than the oldest timestamp.
 * - The commit timestamp must be after the stable timestamp.
 * For a prepared transaction:
 * - The commit_ts cannot be less than the prepared_ts unless rounding
 *   the prepare timestamp is enabled.
 * Note: If a prepared timestamp was given in the transaction, then the transaction has to be
 * prepared before commit timestamp is set.
 */
int
timestamp_manager::validate_commit_timestamp(session_simulator *session, uint64_t commit_ts)
{

    if (!session->has_prepare_timestamp()) {
        if (session->is_commit_ts_set()) {
            /*
             * We cannot set the commit timestamp to be earlier than the first commit timestamp when
             * setting the commit timestamp multiple times within a transaction.
             */
            uint64_t first_commit_ts = session->get_first_commit_timestamp();
            if (commit_ts < first_commit_ts)
                WT_SIM_RET_MSG(EINVAL,
                  "commit timestamp (" + std::to_string(commit_ts) +
                    ") older than the first commit timestamp (" + std::to_string(first_commit_ts) +
                    ") for this transaction");

            commit_ts = first_commit_ts;
        }

        /* The commit timestamp should not be less than the oldest timestamp. */
        connection_simulator *conn = &connection_simulator::get_connection();
        if (conn->has_oldest_ts()) {
            uint64_t oldest_ts = conn->get_oldest_ts();
            if (commit_ts < oldest_ts)
                WT_SIM_RET_MSG(EINVAL,
                  "commit timestamp (" + std::to_string(commit_ts) +
                    ") is less than the oldest timestamp (" + std::to_string(oldest_ts) + ")");
        }

        /* The commit timestamp must be after the stable timestamp. */
        if (conn->has_stable_ts()) {
            uint64_t stable_ts = conn->get_stable_ts();
            if (commit_ts <= stable_ts)
                WT_SIM_RET_MSG(EINVAL,
                  "commit timestamp (" + std::to_string(commit_ts) +
                    ") must be after the stable timestamp (" + std::to_string(stable_ts) + ")");
        }

        /* The commit timestamp must be greater than the latest active read timestamp. */
        uint64_t latest_active_read = conn->get_latest_active_read();
        if (latest_active_read >= commit_ts)
            WT_SIM_RET_MSG(EINVAL,
              "commit timestamp (" + std::to_string(commit_ts) +
                ") must be after all active read timestamps (" +
                std::to_string(latest_active_read) + ")");
    } else {
        uint64_t prepare_ts = session->get_prepare_timestamp();
        /*
         * For a prepared transaction, the commit timestamp should not be less than the prepare
         * timestamp. Also, the commit timestamp cannot be set before the transaction has actually
         * been prepared.
         *
         * If the commit timestamp is less than the oldest timestamp and the transaction is
         * configured to roundup timestamps of a prepared transaction, then we will roundup the
         * commit timestamp to the prepare timestamp of the transaction.
         */
        if (session->has_prepare_timestamp())
            if (!session->is_round_prepare_ts_set() && commit_ts < prepare_ts)
                WT_SIM_RET_MSG(EINVAL,
                  "commit timestamp (" + std::to_string(commit_ts) +
                    ") is less than the prepare timestamp (" + std::to_string(prepare_ts) +
                    ") for this transaction.");

        if (!session->is_txn_prepared())
            WT_SIM_RET_MSG(EINVAL,
              "commit timestamp (" + std::to_string(commit_ts) +
                ") must not be set before transaction is prepared");
    }

    return (0);
}

/*
 * Validate the prepare timestamp. The constraints on the prepare timestamp are:
 * - Cannot set the prepared timestamp if the transaction is already prepared.
 * - Cannot set prepared timestamp more than once.
 * - Commit timestamp should not have been set before the prepare timestamp.
 * - Prepare timestamp must be greater than the latest active read timestamp.
 * - Prepare timestamp cannot be less than the stable timestamp unless rounding
 *   the prepare timestamp is enabled.
 */
int
timestamp_manager::validate_prepare_timestamp(session_simulator *session, uint64_t prepare_ts) const
{
    /* Cannot set the prepared timestamp if the transaction is already prepared. */
    if (session->is_txn_prepared())
        WT_SIM_RET_MSG(
          EINVAL, "Cannot set the prepared timestamp if the transaction is already prepared");

    /* A prepared timestamp should not have been set at this point. */
    if (session->has_prepare_timestamp())
        WT_SIM_RET_MSG(EINVAL, "Prepare timestamp is already set");

    /* Commit timestamp should not have been set before the prepare timestamp. */
    if (session->is_commit_ts_set())
        WT_SIM_RET_MSG(
          EINVAL, "Commit timestamp should not have been set before the prepare timestamp");

    connection_simulator *conn = &connection_simulator::get_connection();
    /* The prepare timestamp must be greater than the latest active read timestamp. */
    uint64_t latest_active_read = conn->get_latest_active_read();
    if (latest_active_read >= prepare_ts)
        WT_SIM_RET_MSG(EINVAL,
          "prepare timestamp (" + std::to_string(prepare_ts) +
            ") must be after all active read timestamps (" + std::to_string(latest_active_read) +
            ")");

    /*
     * Prepare timestamp cannot be less than the stable timestamp unless rounding the prepare
     * timestamp is enabled.
     */
    uint64_t stable_ts = conn->get_stable_ts();
    if (prepare_ts <= stable_ts)
        if (!session->is_round_prepare_ts_set())
            WT_SIM_RET_MSG(EINVAL,
              "prepare timestamp (" + std::to_string(prepare_ts) +
                ") is less than the stable timestamp (" + std::to_string(stable_ts) +
                ") for this transaction.");

    return (0);
}

/*
 * Validate the durable timestamp. The constraints on the durable timestamp are:
 * - Durable timestamp should not be specified for non-prepared transaction.
 * - Commit timestamp is required before setting a durable timestamp.
 * - The durable timestamp should not be less than the oldest timestamp.
 * - The durable timestamp must be after the stable timestamp.
 * - The durable timestamp should not be less than the commit timestamp.
 */
int
timestamp_manager::validate_session_durable_timestamp(
  session_simulator *session, uint64_t durable_ts)
{
    /* Durable timestamp should not be specified for non-prepared transaction. */
    if (!session->is_txn_prepared())
        WT_SIM_RET_MSG(
          EINVAL, "durable timestamp should not be specified for non-prepared transaction");

    /* Commit timestamp is required before setting a durable timestamp. */
    if (!session->is_commit_ts_set())
        WT_SIM_RET_MSG(EINVAL, "commit timestamp is required before setting a durable timestamp");

    /* The durable timestamp should not be less than the oldest timestamp. */
    connection_simulator *conn = &connection_simulator::get_connection();
    if (conn->has_oldest_ts()) {
        uint64_t oldest_ts = conn->get_oldest_ts();
        if (durable_ts < oldest_ts)
            WT_SIM_RET_MSG(EINVAL,
              "commit timestamp (" + std::to_string(durable_ts) +
                ") is less than the oldest timestamp (" + std::to_string(oldest_ts) + ")");
    }

    /* The durable timestamp must be after the stable timestamp. */
    if (conn->has_stable_ts()) {
        uint64_t stable_ts = conn->get_stable_ts();
        if (durable_ts <= stable_ts)
            WT_SIM_RET_MSG(EINVAL,
              "commit timestamp (" + std::to_string(durable_ts) +
                ") must be after the stable timestamp (" + std::to_string(stable_ts) + ")");
    }

    /* The durable timestamp should not be less than the commit timestamp. */
    uint64_t commit_ts = session->get_commit_timestamp();
    if (durable_ts < commit_ts)
        WT_SIM_RET_MSG(EINVAL,
          "durable timestamp (" + std::to_string(durable_ts) +
            ") is less than the commit timestamp (" + std::to_string(commit_ts) +
            ") in this transaction");

    return (0);
}
