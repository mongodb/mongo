# Copyright (C) 2022-present MongoDB, Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the Server Side Public License, version 1,
# as published by MongoDB, Inc.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# Server Side Public License for more details.
#
# You should have received a copy of the Server Side Public License
# along with this program. If not, see
# <http://www.mongodb.com/licensing/server-side-public-license>.
#
# As a special exception, the copyright holders give permission to link the
# code of portions of this program with the OpenSSL library under certain
# conditions as described in each individual source file and distribute
# linked combinations including the program with the OpenSSL library. You
# must comply with the Server Side Public License in all respects for
# all of the code used other than as permitted herein. If you modify file(s)
# with this exception, you may extend this exception to your version of the
# file(s), but you are not obligated to do so. If you do not wish to do so,
# delete this exception statement from your version. If you delete this
# exception statement from all source files in the program, then also delete
# it in the license file.
#
"""Data generation entry point."""

import asyncio
import json
import math
import os
import re
from datetime import datetime
from pathlib import Path

import bson
import matplotlib.pyplot as plt
import seaborn as sns
from ce_data_settings import data_generator_config, database_config
from config import CollectionTemplate, FieldTemplate
from data_generator import DataGenerator
from database_instance import DatabaseInstance
from random_generator import DataType

__all__ = []


class CollectionTemplateEncoder(json.JSONEncoder):
    def default(self, o):
        if isinstance(o, CollectionTemplate):
            collections = []
            for card in o.cardinalities:
                name = f"{o.name}_{card}"
                collections.append(
                    dict(
                        collectionName=name,
                        fields=o.fields,
                        compoundIndexes=o.compound_indexes,
                        cardinality=card,
                    )
                )
            return collections
        elif isinstance(o, FieldTemplate):
            return dict(fieldName=o.name, dataType=o.data_type, indexed=o.indexed)
        elif isinstance(o, DataType):
            return o.name.lower()
        # Let the base class default method raise the TypeError
        return super(CollectionTemplateEncoder, self).default(o)


class OidEncoder(json.JSONEncoder):
    cur_oid = -1

    def default(self, o):
        if isinstance(o, bson.objectid.ObjectId):
            # Replace the OID with a consequtive int number as needed by the query generator
            OidEncoder.cur_oid += 1
            return OidEncoder.cur_oid
        if isinstance(o, datetime):
            return str(o)
        return super(OidEncoder, self).default(o)


async def dump_collection(db, dump_path, database_name, coll_name, chunk_size):
    """Dump a collection into separate files each containing at most chunk_size documents."""

    def open_chunk_file(chunk_id):
        chunk_name = f"{coll_name}_{chunk_id}"
        chunk_file_path = Path(dump_path) / f"{chunk_name}"
        print(f"Writing chunk: {chunk_file_path}")
        chunk_file = open(chunk_file_path, "w", encoding="utf-8")
        chunk_file.write("// This is a generated file.\n")
        chunk_file.write(f'{chunk_name} = {{collName: "{coll_name}", collData: [\n')
        return chunk_file, "'" + chunk_name + "'"

    def close_chunk_file(chunk_file):
        if not chunk_file.closed:
            chunk_file.write("]}\n")
            chunk_file.close()

    collection = db[coll_name]
    doc_count = await collection.count_documents({})
    chunk_names = []
    if doc_count == 0:
        return chunk_names

    doc_pos = 0
    chunk_id = 1
    chunk_file, chunk_name = open_chunk_file(chunk_id)
    chunk_names.append(chunk_name)
    async for doc in collection.find({}):
        if doc_pos > 0 and doc_pos % chunk_size == 0:
            # Open a new data file for the next chunk of data.
            close_chunk_file(chunk_file)
            chunk_id += 1
            chunk_file, chunk_name = open_chunk_file(chunk_id)
            chunk_names.append(chunk_name)

        chunk_file.write(json.dumps(doc, cls=OidEncoder))
        doc_pos += 1
        chunk_file.write(",")
        chunk_file.write("\n")
    close_chunk_file(chunk_file)
    return chunk_names


async def dump_collections_to_json(db, dump_path, database_name, collections):
    chunk_size = 100  # number of documents per chunk file
    print(f"Dumping all collections into chunks of size {chunk_size}.")
    all_chunk_names = []
    for coll_name in collections:
        coll_chunk_names = await dump_collection(
            db, dump_path, database_name, coll_name, chunk_size
        )
        all_chunk_names.extend(coll_chunk_names)

    # Generate a JS file that loads all chunk files
    load_file = open(Path(dump_path) / f"{database_name}.data", "w")
    load_file.write("// This is a generated file.\n")
    # Create an array named 'chunkNames' with all chunk file names to be loaded.
    load_file.write(f'const chunkNames = [{",".join(all_chunk_names)}];')


async def generate_histograms(coll_template, coll, dump_path):
    """
    Generate one histogram per each collection field.

    This is slow - enable only when needed to review the generated histograms visually.
    """
    doc_count = await coll.count_documents({})
    for field in coll_template.fields:
        field_data = []
        if re.match("^mixeddata_.*", field.name):
            continue
        async for doc in coll.find({field.name: {"$exists": True}}, {"_id": 0, field.name: 1}):
            field_val = doc[field.name]
            if isinstance(field_val, str):
                field_val = re.escape(field_val)
            if isinstance(field_val, list):
                # TODO Currently array data breaks distplot, so we skip arrays
                continue
            field_data.append(field_val)
        if len(field_data) > 0:
            fig_file_name = f"{dump_path}/{coll.name}_{field.name}.png"
            print(f"Generating histogram {fig_file_name}")
            hist = sns.displot(
                data=field_data, kind="hist", bins=round(math.sqrt(doc_count))
            ).figure
            hist.savefig(fig_file_name)
            plt.close(hist)


async def main():
    """Entry point function."""
    script_directory = os.path.abspath(os.path.dirname(__file__))
    os.chdir(script_directory)

    # 1. Database Instance provides connectivity to a MongoDB instance, it loads data optionally
    # from the dump on creating and stores data optionally to the dump on closing.
    with DatabaseInstance(database_config) as database_instance:
        # 2. Generate random data and populate collections with it.
        old_db_collections = await database_instance.database.list_collection_names()
        for coll_name in old_db_collections:
            collection = database_instance.database[coll_name]
            collection.drop()

        generator = DataGenerator(database_instance, data_generator_config)
        await generator.populate_collections()

        # 3. Export all collections in the database into json files.
        db_collections = await database_instance.database.list_collection_names()
        # TODO: This is an alternative way to export the data. It is better than what is implemented,
        # but cannot be used until we find a way to call 'mongoimport' from the corresponding JS test.
        #
        # for coll_name in db_collections:
        # subprocess.run([
        #     'mongoexport', f'--db={database_config.database_name}', f'--collection={coll_name}',
        #     f'--out={coll_name}.dat'
        # ], cwd=database_config.dump_path, check=True)
        await dump_collections_to_json(
            database_instance.database,
            database_config.dump_path,
            database_config.database_name,
            db_collections,
        )

        # 4. Export the collection templates used to create the test collections into JSON file
        with open(
            Path(database_config.dump_path) / f"{database_config.database_name}.schema", "w"
        ) as metadata_file:
            collections = []
            for coll_template in data_generator_config.collection_templates:
                for card in coll_template.cardinalities:
                    name = f"{coll_template.name}_{card}"
                    collections.append(
                        dict(
                            collectionName=name,
                            fields=coll_template.fields,
                            compound_indexes=coll_template.compound_indexes,
                            cardinality=card,
                        )
                    )
                    # Uncomment this to generate histograms in PNG format
                    # await generate_histograms(coll_template, database_instance.database[name], database_config.dump_path)
            json_metadata = json.dumps(collections, indent=4, cls=CollectionTemplateEncoder)
            metadata_file.write("// This is a generated file.\nconst dbMetadata = ")
            metadata_file.write(json_metadata)
            metadata_file.write(";")

    print("DONE!")


if __name__ == "__main__":
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    asyncio.run(main())
