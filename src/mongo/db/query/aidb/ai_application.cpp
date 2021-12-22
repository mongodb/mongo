/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/query/aidb/ai_application.h"

#include "mongo/db/query/aidb/ai_command.h"
#include "mongo/db/query/aidb/ai_database.h"

namespace mongo::ai {
void Application::run() {
    setUp();
    std::cout << " Welcome to AI DB!" << std::endl;
    run(operationContext());
}

void Application::run(OperationContext* opCtx) {
    Database::setCurrentDbName(opCtx, "test");
    help();

    std::string commandString{};

    while (std::cout << getPrompt(opCtx), getline(std::cin, commandString)) {
        if (commandString.empty()) {
            continue;
        } else if (commandString == "quit") {
            return;
        } else if (commandString == "help") {
            help();
            continue;
        }

        auto status = Command::execute(opCtx, commandString);
        std::cout << "---" << std::endl << status << std::endl;
    }
}

void Application::help() {
    std::cout << "List of commands:" << std::endl;
    std::cout << "\thelp" << std::endl;
    std::cout << "\tquit" << std::endl;
    Command::help(std::cout);
}

std::string Application::getPrompt(OperationContext* opCtx) {
    return Database::getCurrentDbName(opCtx) + "> ";
}

}  // namespace mongo::ai
