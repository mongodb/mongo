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

#include "simulator_interface.h"

#include <cassert>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

void
print_border_msg(const std::string &msg, const std::string &color)
{
    const int count = msg.length() + 2;
    const std::string dash(count, '-');

    std::cout << color << "+" + dash + "+" << RESET << std::endl;
    std::cout << color << "| " << msg << " |" << RESET << std::endl;
    std::cout << color << "+" + dash + "+" << RESET << std::endl;
}

void
print_options(const std::vector<std::string> &options)
{
    std::cout << std::endl;
    for (int i = 0; i < options.size(); i++)
        std::cout << i + 1 << ": " << options[i] << std::endl;
}

int
choose_num(int min, int max, const std::string &cli_str)
{
    std::string text_line;
    int choice;

    while (true) {
        std::cout << "\n" << cli_str << " ";
        std::getline(std::cin, text_line);

        /* Extract number from the text. */
        std::istringstream text_stream(text_line);
        text_stream >> choice;

        /* Validate the number of choice. */
        if (choice >= min && choice <= max)
            return choice;
        else
            print_border_msg(
              "Choose a number between " + std::to_string(min) + " and " + std::to_string(max),
              RED);
    }
}

const std::string
get_input(const std::string &input_name)
{
    std::string input;
    std::cout << input_name;
    std::getline(std::cin, input);
    std::cout << std::endl;
    return (input);
}

session_simulator *
get_session(
  const std::map<std::string, session_simulator *> &session_map, const std::string &session_to_use)
{
    /* session_to_use should not be empty. */
    assert(!session_to_use.empty());

    /* session_id should exist in the map. */
    assert(session_map.find(session_to_use) != session_map.end());

    /* Get the session from the session map. */
    session_simulator *session = session_map.at(session_to_use);
    assert(session != nullptr);
    return (session);
}

void
interface_session_management(connection_simulator *conn,
  std::map<std::string, session_simulator *> &session_map, std::string &session_in_use)
{
    std::string config;
    std::vector<std::string> options;
    options.push_back("Use session");
    options.push_back("Create session");
    options.push_back("<- go back");

    do {
        try {
            print_border_msg("Listing sessions: ", WHITE);
            for (const auto &session : session_map)
                std::cout << session.first << std::endl;

            print_options(options);

            int choice = choose_num(1, options.size(), "Session management >>");

            switch (choice) {
            case 1: {
                std::string session_selected = get_input("Which session: ");
                if (session_map.find(session_selected) == session_map.end())
                    throw "Session selected (" + session_selected + ") not in the list";
                session_in_use = session_selected;
                return;
            }
            case 2: {
                session_simulator *session = conn->open_session();
                std::string session_name = "Session" + std::to_string((session_map.size() + 1));
                session_map.insert(
                  std::pair<std::string, session_simulator *>(session_name, session));
                break;
            }
            case 3:
            default:
                return;
            }
        } catch (const std::string &exception_str) {
            print_border_msg("exception: " + exception_str, RED);
        }

    } while (true);
}

void
interface_set_timestamp(connection_simulator *conn)
{
    std::string config;
    std::vector<std::string> options;
    options.push_back("Oldest timestamp");
    options.push_back("Stable timestamp");
    options.push_back("All durable timestamp");
    options.push_back("<- go back");

    do {
        try {
            print_options(options);

            int choice = choose_num(1, options.size(), "Set timestamp >>");

            switch (choice) {
            case 1:
                config = "oldest_timestamp=";
                break;
            case 2:
                config = "stable_timestamp=";
                break;
            case 3:
                config = "durable_timestamp=";
                break;
            case 4:
            default:
                return;
            }

            config += get_input("Enter timestamp (in hex): ");

            int ret = conn->set_timestamp(config);

            if (ret != 0)
                throw "'set_timestamp' failed with ret value: '" + std::to_string(ret) +
                  "', and config: '" + config + "'";

            print_border_msg("- Global timestamp set - " + config, GREEN);
        } catch (const std::string &exception_str) {
            print_border_msg("exception: " + exception_str, RED);
        }

    } while (true);
}

void
interface_conn_query_timestamp(connection_simulator *conn)
{
    std::string config;
    std::vector<std::string> options;
    options.push_back("Oldest timestamp");
    options.push_back("Stable timestamp");
    options.push_back("All durable timestamp");
    options.push_back("<- go back");

    do {
        try {
            print_options(options);

            int choice = choose_num(1, options.size(), "[Conn] query timestamp >>");

            switch (choice) {
            case 1:
                config = "get=oldest";
                break;
            case 2:
                config = "get=stable";
                break;
            case 3:
                config = "get=all_durable";
                break;
            case 4:
            default:
                return;
            }

            std::string hex_ts;
            bool ts_supported;
            int ret = conn->query_timestamp(config, hex_ts, ts_supported);

            if (ret != 0)
                throw "'query_timestamp' failed with ret value: '" + std::to_string(ret) +
                  "', and config: '" + config + "'";

            if (hex_ts == "0")
                hex_ts = "not specified";

            print_border_msg("- Timestamp queried - " + config + ": " + hex_ts, GREEN);
        } catch (const std::string &exception_str) {
            print_border_msg("exception: " + exception_str, RED);
        }

    } while (true);
}

void
interface_begin_transaction(session_simulator *session)
{
    std::string config = "";
    std::vector<std::string> options;
    options.push_back("Read timestamp");
    options.push_back("Roundup read timestamps");
    options.push_back("Roundup prepare timestamps");
    options.push_back("Run operation");
    options.push_back("<- go back");

    do {
        try {
            print_options(options);

            int choice = choose_num(1, options.size(), "Begin transaction >>");

            switch (choice) {
            case 1:
                config += "read_timestamp=";
                config += get_input("Enter read timestamp (in hex): ");
                break;
            case 2:
                config += "roundup_timestamps=(read=true)";
                break;
            case 3:
                config += "roundup_timestamps=(prepare=true)";
                break;
            case 4: {
                int ret = session->begin_transaction(config);

                if (ret != 0)
                    throw "begin_transaction failed with return value: " + std::to_string(ret);

                print_border_msg("- Transaction started - " + config, GREEN);
                return;
            }
            case 5:
            default:
                return;
            }
            config += ",";
        } catch (const std::string &exception_str) {
            config = "";
            print_border_msg("exception: " + exception_str, RED);
            return;
        }
    } while (true);
}

void
interface_commit_transaction(session_simulator *session)
{
    std::string config = "";
    std::vector<std::string> options;
    options.push_back("Commit timestamp");
    options.push_back("Durable timestamps");
    options.push_back("Run operation");
    options.push_back("<- go back");

    do {
        try {
            print_options(options);

            int choice = choose_num(1, options.size(), "Commit transaction >>");

            switch (choice) {
            case 1:
                config += "commit_timestamp=";
                config += get_input("Enter commit timestamp (in hex): ");
                break;
            case 2:
                config += "durable_timestamp=";
                config += get_input("Enter durable timestamp (in hex): ");
                break;
            case 3: {
                int ret = session->commit_transaction(config);

                if (ret != 0)
                    throw "commit_transaction failed with return value: " + std::to_string(ret);

                print_border_msg("- Transaction committed - " + config, GREEN);
                return;
            }
            case 4:
            default:
                return;
            }
            config += ",";
        } catch (const std::string &exception_str) {
            config = "";
            print_border_msg("exception: " + exception_str, RED);
            return;
        }
    } while (true);
}

void
interface_prepare_transaction(session_simulator *session)
{
    std::string config = "";
    std::vector<std::string> options;
    options.push_back("Prepare timestamp");
    options.push_back("Run operation");
    options.push_back("<- go back");

    do {
        try {
            print_options(options);

            int choice = choose_num(1, options.size(), "Commit transaction >>");

            switch (choice) {
            case 1:
                config += "prepare_timestamp=";
                config += get_input("Enter prepare timestamp (in hex): ");
                break;
            case 2: {
                int ret = session->prepare_transaction(config);

                if (ret != 0)
                    throw "prepare_transaction failed with return value: " + std::to_string(ret);

                print_border_msg("- Transaction prepared - " + config, GREEN);
                return;
            }
            case 3:
            default:
                return;
            }
            config += ",";
        } catch (const std::string &exception_str) {
            config = "";
            print_border_msg("exception: " + exception_str, RED);
            return;
        }
    } while (true);
}

void
interface_rollback_transaction(session_simulator *session)
{
    int ret = session->rollback_transaction();

    try {
        if (ret != 0)
            throw "rollback_transaction failed with return value: " + std::to_string(ret);

        print_border_msg("- Transaction rolled back - ", GREEN);
    } catch (const std::string &exception_str) {
        print_border_msg("exception: " + exception_str, RED);
        return;
    }
}

void
interface_timestamp_transaction(session_simulator *session)
{
    std::string config = "";
    std::vector<std::string> options;
    options.push_back("Commit timestamp");
    options.push_back("Durable timestamps");
    options.push_back("Prepare timestamps");
    options.push_back("Read timestamps");
    options.push_back("Run operation");
    options.push_back("<- go back");

    do {
        try {
            print_options(options);

            int choice = choose_num(1, options.size(), "Commit transaction >>");

            switch (choice) {
            case 1:
                config += "commit_timestamp=";
                config += get_input("Enter commit timestamp (in hex): ");
                break;
            case 2:
                config += "durable_timestamp=";
                config += get_input("Enter durable timestamp (in hex): ");
                break;
            case 3:
                config += "prepare_timestamp=";
                config += get_input("Enter prepare timestamp (in hex): ");
                break;
            case 4:
                config += "read_timestamp=";
                config += get_input("Enter read timestamp (in hex): ");
                break;
            case 5: {
                int ret = session->timestamp_transaction(config);

                if (ret != 0)
                    throw "timestamp_transaction failed with return value: " + std::to_string(ret);

                print_border_msg("- Timestamp set- " + config, GREEN);
                return;
            }
            case 6:
            default:
                return;
            }
            config += ",";
        } catch (const std::string &exception_str) {
            config = "";
            print_border_msg("exception: " + exception_str, RED);
            return;
        }
    } while (true);
}

void
interface_session_query_timestamp(session_simulator *session)
{
    std::string config;
    std::vector<std::string> options;
    options.push_back("Commit timestamp");
    options.push_back("First commit timestamp");
    options.push_back("Prepare timestamp");
    options.push_back("Read timestamp");
    options.push_back("<- go back");

    do {
        try {
            print_options(options);

            int choice = choose_num(1, options.size(), "[Session] query timestamp >>");

            switch (choice) {
            case 1:
                config = "get=commit";
                break;
            case 2:
                config = "get=first_commit";
                break;
            case 3:
                config = "get=prepare";
                break;
            case 4:
                config = "get=read";
                break;
            case 5:
            default:
                return;
            }

            std::string hex_ts;
            int ret = session->query_timestamp(config, hex_ts);

            if (ret != 0)
                throw "'query_timestamp' failed with ret value: '" + std::to_string(ret) +
                  "', and config: '" + config + "'";

            if (hex_ts == "0")
                hex_ts = "not specified";

            print_border_msg("- Timestamp queried - " + config + ": " + hex_ts, GREEN);
        } catch (const std::string &exception_str) {
            print_border_msg("exception: " + exception_str, RED);
        }

    } while (true);
}

void
print_rules()
{
    bool exit = false;
    std::vector<std::string> options;
    options.push_back("oldest and stable timestamp");
    options.push_back("commit timestamp");
    options.push_back("prepare timestamp");
    options.push_back("durable timestamp");
    options.push_back("read timestamp");
    options.push_back("<- go back");

    do {
        print_options(options);

        int choice = choose_num(1, options.size(), "Choose timestamp >>");

        switch (choice) {
        case 1:
            print_border_msg("Timestamp value should be greater than 0.", WHITE);
            print_border_msg(
              "It is a no-op to set the oldest or stable timestamps behind the global values.",
              WHITE);
            print_border_msg("Oldest must not be greater than the stable timestamp", WHITE);
            break;
        case 2:
            print_border_msg(
              "The commit_ts cannot be less than the first_commit_timestamp.", WHITE);
            print_border_msg("The commit_ts cannot be less than the oldest timestamp.", WHITE);
            print_border_msg("The commit timestamp must be after the stable timestamp.", WHITE);
            print_border_msg("The commit_ts cannot be less than the prepared_ts", WHITE);
            break;
        case 3:
            print_border_msg(
              "Cannot set the prepared timestamp if the transaction is already prepared.", WHITE);
            print_border_msg("Cannot set prepared timestamp more than once.", WHITE);
            print_border_msg(
              "Commit timestamp should not have been set before the prepare timestamp.", WHITE);
            print_border_msg(
              "Prepare timestamp must be greater than the latest active read timestamp.", WHITE);
            print_border_msg("Prepare timestamp cannot be less than the stable timestamp", WHITE);
            break;
        case 4:
            print_border_msg(
              "Durable timestamp should not be specified for non-prepared transaction.", WHITE);
            print_border_msg(
              "Commit timestamp is required before setting a durable timestamp.", WHITE);
            print_border_msg(
              "The durable timestamp should not be less than the oldest timestamp.", WHITE);
            print_border_msg("The durable timestamp must be after the stable timestamp.", WHITE);
            print_border_msg(
              "The durable timestamp should not be less than the commit timestamp.", WHITE);
            break;
        case 5:
            print_border_msg(
              "The read timestamp can only be set before a transaction is prepared.", WHITE);
            print_border_msg("Read timestamps can only be set once.", WHITE);
            print_border_msg(
              "The read timestamp must be greater than or equal to the oldest timestamp.", WHITE);
            break;
        case 6:
        default:
            exit = true;
        }
    } while (!exit);
}

int
main(int argc, char *argv[])
{
    connection_simulator *conn = &connection_simulator::get_connection();
    session_simulator *session = conn->open_session();

    std::map<std::string, session_simulator *> session_map;
    std::string session_in_use = "Session1";
    session_map.insert(std::pair<std::string, session_simulator *>(session_in_use, session));

    bool exit = false;
    std::vector<std::string> options;
    options.push_back("Session Management");
    options.push_back("[Conn] set_timestamp()");
    options.push_back("[Conn] query_timestamp()");
    options.push_back("[Session] begin_transaction()");
    options.push_back("[Session] commit_transaction()");
    options.push_back("[Session] prepare_transaction()");
    options.push_back("[Session] rollback_transaction()");
    options.push_back("[Session] timestamp_transaction()");
    options.push_back("[Session] query_timestamp()");
    options.push_back("Print rules for timestamps");
    options.push_back("Exit");

    do {
        session = get_session(session_map, session_in_use);

        print_border_msg("Session in use: " + session_in_use, GREEN);

        print_options(options);

        int choice = choose_num(1, options.size(), "timestamp_simulator >>");

        switch (choice) {
        case 1:
            interface_session_management(conn, session_map, session_in_use);
            break;
        case 2:
            interface_set_timestamp(conn);
            break;
        case 3:
            interface_conn_query_timestamp(conn);
            break;
        case 4:
            interface_begin_transaction(session);
            break;
        case 5:
            interface_commit_transaction(session);
            break;
        case 6:
            interface_prepare_transaction(session);
            break;
        case 7:
            interface_rollback_transaction(session);
            break;
        case 8:
            interface_timestamp_transaction(session);
            break;
        case 9:
            interface_session_query_timestamp(session);
            break;
        case 10:
            print_rules();
            break;
        case 11:
        default:
            exit = true;
        }
    } while (!exit);

    return (0);
}
