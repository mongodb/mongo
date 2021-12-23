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

#pragma once

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo::ai {

class Command {
public:
    template <typename T>
    struct Register {
        Register() {
            auto ptr = std::make_unique<T>();
            Command::_registeredCommands.insert_or_assign(ptr->name(), std::move(ptr));
        }
    };

    static Command* getCommand(const std::string& name);
    static Status execute(OperationContext* opCtx, const std::string& commandString);
    static void help(std::ostream& os);

    virtual std::string name() const = 0;
    virtual std::string help() const = 0;
    std::string usage() const {
        return "Usage: " + help();
    }
    virtual Status execute(OperationContext* opCtx, std::istream& commandStream) = 0;

    virtual ~Command() = default;

protected:
    static std::vector<std::string> split(std::istream& is, size_t numParts);

private:
    static stdx::unordered_map<std::string, std::unique_ptr<Command>> _registeredCommands;
};

class GenerateCommand : public Command {
public:
    std::string name() const override {
        return "generate";
    }
    std::string help() const override {
        return "generate <collection_name> <number_of_documents>";
    }

    Status execute(OperationContext* opCtx, std::istream& commandStream) override;
};

class DemoCommand : public Command {
public:
    std::string name() const override {
        return "demo";
    }
    std::string help() const override {
        return "demo <collection_name> <number_of_documents>";
    }

    Status execute(OperationContext* opCtx, std::istream& commandStream) override;
};

class UseCommand : public Command {
public:
    std::string name() const override {
        return "use";
    }
    std::string help() const override {
        return "use <database>";
    }

    Status execute(OperationContext* opCtx, std::istream& commandStream) override;
};

class ShowCommand : public Command {
public:
    std::string name() const override {
        return "show";
    }
    std::string help() const override {
        return "show databases|collections";
    }

    Status execute(OperationContext* opCtx, std::istream& commandStream) override;
};

class GetCommand : public Command {
public:
    std::string name() const override {
        return "get";
    }
    std::string help() const override {
        return "get <collection> <record_id>";
    }

    Status execute(OperationContext* opCtx, std::istream& commandStream) override;
};

class CollectionInfoCommand : public Command {
public:
    std::string name() const override {
        return "collectionInfo";
    }
    std::string help() const override {
        return "collectionInfo <collection>";
    }

    Status execute(OperationContext* opCtx, std::istream& commandStream) override;
};

class IndexScanCommand : public Command {
public:
    std::string name() const override {
        return "indexScan";
    }
    std::string help() const override {
        return "indexScan <collection> <indexName> <query>";
    }

    Status execute(OperationContext* opCtx, std::istream& commandStream) override;
};

class CollectionScanCommand : public Command {
public:
    std::string name() const override {
        return "collectionScan";
    }
    std::string help() const override {
        return "collectionScan <collection> <query>";
    }

    Status execute(OperationContext* opCtx, std::istream& commandStream) override;
};

class CreateCollectionCommand : public Command {
public:
    std::string name() const override {
        return "createCollection";
    }
    std::string help() const override {
        return "createCollection <collectionName>";
    }

    Status execute(OperationContext* opCtx, std::istream& commandStream) override;
};

class CreateIndexCommand : public Command {
public:
    std::string name() const override {
        return "createIndex";
    }
    std::string help() const override {
        return "createIndex <collectionName> <indexName> <pattern>";
    }

    Status execute(OperationContext* opCtx, std::istream& commandStream) override;
};
}  // namespace mongo::ai
