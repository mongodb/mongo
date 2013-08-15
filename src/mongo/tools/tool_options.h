/*
 *    Copyright (C) 2010 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <iosfwd>

#include "mongo/base/status.h"

namespace mongo {

    namespace optionenvironment {
        class OptionSection;
    } // namespace optionenvironment

    namespace moe = mongo::optionenvironment;

    Status addGeneralToolOptions(moe::OptionSection* options);

    Status addRemoteServerToolOptions(moe::OptionSection* options);

    Status addLocalServerToolOptions(moe::OptionSection* options);

    Status addSpecifyDBCollectionToolOptions(moe::OptionSection* options);

    Status addToolFieldOptions(moe::OptionSection* options);

    Status addBSONToolOptions(moe::OptionSection* options);

    Status addMongoDumpOptions(moe::OptionSection* options);

    Status addMongoRestoreOptions(moe::OptionSection* options);

    Status addMongoExportOptions(moe::OptionSection* options);

    Status addMongoImportOptions(moe::OptionSection* options);

    Status addMongoFilesOptions(moe::OptionSection* options);

    Status addMongoOplogOptions(moe::OptionSection* options);

    Status addMongoStatOptions(moe::OptionSection* options);

    Status addMongoTopOptions(moe::OptionSection* options);

    Status addBSONDumpOptions(moe::OptionSection* options);

    void printMongoDumpHelp(const moe::OptionSection options, std::ostream* out);

    void printMongoRestoreHelp(const moe::OptionSection options, std::ostream* out);

    void printMongoExportHelp(const moe::OptionSection options, std::ostream* out);

    void printMongoImportHelp(const moe::OptionSection options, std::ostream* out);

    void printMongoFilesHelp(const moe::OptionSection options, std::ostream* out);

    void printMongoOplogHelp(const moe::OptionSection options, std::ostream* out);

    void printMongoStatHelp(const moe::OptionSection options, std::ostream* out);

    void printMongoTopHelp(const moe::OptionSection options, std::ostream* out);

    void printBSONDumpHelp(const moe::OptionSection options, std::ostream* out);
}
