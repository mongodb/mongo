"""
extract_schema.py: Read an existing dataset and extract metadata into a .schema file
"""

import argparse  # noqa: I001
import asyncio
import json
import sys
from collections import defaultdict
from pymongo import AsyncMongoClient

from bson.objectid import ObjectId

# Used to silence a circular import situation in the main data_generator code
import datagen.util
import datagen.serialize
import datagen.statistics


async def main():
    parser = argparse.ArgumentParser(
        prog=sys.argv[0],
        description="Extract metadata from an existing dataset into a .schema file.",
    )

    parser.add_argument("--uri", default="mongodb://localhost", help="MongoDB connection string.")
    parser.add_argument("--db", "-d", required=True, help="Database name.")
    parser.add_argument(
        "--collection",
        "-c",
        help="Name of the collection to process. If not provided, metadata for all fields of all collections will be joined together.",
    )

    parser.add_argument("--sample-size", default=10000, help="Sample size to use.")

    args = parser.parse_args()

    client = AsyncMongoClient(args.uri)

    db = client[args.db]

    collection_names = (
        [args.collection] if args.collection else list(await db.list_collection_names())
    )
    print(f"The following collections will be dumped: {collection_names}", file=sys.stderr)

    collections = [db.get_collection(name) for name in collection_names]

    field_stats = defaultdict(datagen.statistics.FieldStatistic)
    for collection in collections:
        cursor = await collection.aggregate([{"$sample": {"size": args.sample_size}}])
        async for doc in cursor:
            for field, value in doc.items():
                if not isinstance(value, ObjectId):
                    field_stats[field].register(value)

    output = dict(
        collectionName=collection_names[0]
        if len(collection_names) == 1
        else "MULTIPLE_COLLECTIONS",
        fields=field_stats,
    )

    json_metadata = json.dumps(output, indent=4, cls=datagen.serialize.StatisticsEncoder)
    print(
        "// This file was generated using the extract_schema.py script.\nexport const dbMetadata = "
    )
    print(json_metadata)
    print(";")


if __name__ == "__main__":
    asyncio.run(main())
