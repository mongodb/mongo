# Copyright (C) 2025-present MongoDB, Inc.
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

"""The driver for data generation."""

import pathlib
import shlex
import typing
from dataclasses import fields

import datagen.database_instance


class CorrelatedGeneratorFactory:
    def __init__(self, obj_type: type, seed: typing.Any):
        import datagen.faker
        import datagen.util
        import faker

        class Provider(datagen.faker.CorrelatedProvider):
            def generate_type_with(self, factory: datagen.util.CorrelatedDataFactory):
                self.generator.clear()
                return factory.build(obj_type)

        gen = datagen.faker.CorrelatedGenerator()
        gen.random.seed(seed)
        self.fkr = faker.Faker(generator=gen)
        provider = Provider(generator=gen)
        self.fkr.add_provider(provider)
        self.factory = datagen.util.CorrelatedDataFactory(provider, self.fkr)

        # Faker and random do not allow the seed to be retrieved after instantiation,
        # so the spec files can not create uncorrelated Faker objects after the fact that
        # use the same seed. So, we instantiate a global uncorrelated Faker here.
        datagen.util.set_uncorrelated_faker(faker.Faker(seed=seed))

    def make_generator(self) -> typing.Generator:
        """Generates the dictated objects as a generator."""

        while True:
            yield self.fkr.generate_type_with(self.factory)

    def dump_metadata(self, collection_name, size, seed, metadata_path):
        import json

        import datagen.serialize

        # Make sure the output directory exists.
        metadata_path.parent.mkdir(exist_ok=True)

        with open(metadata_path, "w") as metadata_file:
            collection = dict(
                collectionName=collection_name,
                size=size,
                fields=self.factory.statistics,
            )
            json_metadata = json.dumps(
                collection, indent=4, cls=datagen.serialize.StatisticsEncoder
            )
            metadata_file.write("// This is a generated file.\nexport const dbMetadata = ")
            metadata_file.write(json_metadata)
            metadata_file.write(";")


def dump_commands(out_dir: pathlib.Path):
    """Also dump out the commands to the default location."""
    # Append the remaining commands list.
    local_commands = pathlib.Path("commands.sh")
    if local_commands.exists():
        dumped_commands = out_dir / "commands.sh"
        dumped_commands.parent.mkdir(exist_ok=True)
        with dumped_commands.open(mode="a") as fout:
            with local_commands.open(mode="r") as fin:
                fout.write(fin.read())
        local_commands.unlink()


def record_metadata(module, out_dir: pathlib.Path, seed: str | None = None, clean: bool = False):
    """Record the metadata that generated the dataset."""
    import shlex
    import shutil
    import sys

    # Make sure the output directory exists.
    out_dir.mkdir(exist_ok=True)

    # Copy the specification.
    if module:
        module_src = pathlib.Path(module.__file__)
        shutil.copyfile(module_src, out_dir / module_src.name)

    # Do not include the `--dump` flag in the log. It can lead to an infinite loop if running the
    # `dump/commands.sh` file directly.
    args = (arg for arg in sys.argv if arg != "--dump")
    argstring = shlex.join(("python3", *args, *(() if seed is None else ("--seed", seed))))
    commands_sh = pathlib.Path("commands.sh")

    # Log the command.
    with commands_sh.open("w" if clean else "a") as fout:
        fout.write(f"{argstring}\n")


async def upstream(
    database_instance: datagen.database_instance.DatabaseInstance,
    collection_name: str,
    source: typing.Generator,
    count: int,
):
    """Bulk insert generated objects into a collection."""
    import dataclasses

    import datagen.serialize

    tasks = []

    while count:
        num = min(1000, count)  # Batch the inserts.
        tasks.append(
            asyncio.create_task(
                database_instance.insert_many(
                    collection_name,
                    [
                        datagen.serialize.serialize_doc(dataclasses.asdict(next(source)))
                        for _ in range(num)
                    ],
                )
            )
        )
        count -= num

    for task in tasks:
        await task


async def main():
    """The main loop function."""
    import argparse
    import importlib
    import random
    import sys

    parser = argparse.ArgumentParser(
        prog=sys.argv[0],
        description="Generate datasets and workloads for Query tests and benchmarks",
    )

    # Generator settings
    parser.add_argument("--uri", default="mongodb://localhost", help="MongoDB connection string")
    parser.add_argument("--db", "-d", required=True, help="Name of the database ")
    parser.add_argument(
        "--restore",
        choices=[mode.name for mode in datagen.database_instance.RestoreMode],
        default=datagen.database_instance.RestoreMode.NEVER.name,
        type=str,
        help="Restore the collection before inserting.",
    )
    parser.add_argument("--drop", action="store_true", help="Drop the collection before inserting.")
    parser.add_argument("--dump", nargs="?", const="", help="Dump the collection after inserting.")
    parser.add_argument("--analyze", action="store_true", help="""
                        Run the 'analyze' command against each field of the collection.
                        Analyze is not preserved across restarts, or when dumping or restoring.
                        """)
    parser.add_argument("--indices", action="append", help="An index set to load.")
    parser.add_argument("--restore-args", type=str, help="Parameters to pass to mongorestore.")
    parser.add_argument(
        "--out",
        "-o",
        default=pathlib.Path("out"),
        type=pathlib.Path,
        help="Path where output files are stored. Forwarded to `mongodump`.",
    )

    # Path to the document schema
    parser.add_argument("module", type=str, help="Module containing the schema to use.")
    parser.add_argument("spec", type=str, help="Name of the schema to generate.")

    # Collection specification
    parser.add_argument(
        "--collection-name",
        "-c",
        help="Name of the collection. Defaults to the name of the schema.",
    )
    parser.add_argument(
        "--size",
        required=True,
        type=int,
        help="Number of objects to generate. Set to 0 to skip data generation.",
    )
    parser.add_argument("--seed", type=str, help="The seed to use.")

    args = parser.parse_args()
    module = importlib.import_module(args.module)
    spec = getattr(module, args.spec)

    restore_mode = datagen.database_instance.RestoreMode[args.restore]
    restore_additional_args = [] if args.restore_args is None else shlex.split(args.restore_args)
    dump_additional_args = ["--out", args.out] + (
        [] if args.dump is None else shlex.split(args.dump)
    )
    database_config = datagen.database_instance.DatabaseConfig(
        args.uri, args.db, restore_mode, restore_additional_args
    )

    collection_name = args.spec if args.collection_name is None else args.collection_name

    metadata_path = args.out / f"{collection_name}.schema"

    # 1. Database Instance provides connectivity to a MongoDB instance, it loads data optionally
    # from the dump on creating and stores data optionally to the dump on closing.
    with datagen.database_instance.DatabaseInstance(database_config) as database_instance:
        if args.drop:
            await database_instance.drop_collection(collection_name)

        seed = None

        # 2. Create and insert the documents.
        if args.size:
            # Generate 1024 bits of randomness as the initial seed if one was not already provided.
            seed = args.seed if args.seed else f"{random.getrandbits(1024)}"
            generator_factory = CorrelatedGeneratorFactory(spec, seed)
            generator = generator_factory.make_generator()
            await upstream(database_instance, collection_name, generator, args.size)
            generator_factory.dump_metadata(collection_name, args.size, seed, metadata_path)

        # 3. Create indices after documents.
        indices = args.indices if args.indices else ()
        for index_set_name in indices:
            if hasattr(module, index_set_name):
                index_set = getattr(module, index_set_name)
                indices = index_set() if callable(index_set) else index_set
                await database_instance.database.get_collection(collection_name).create_indexes(
                    indices
                )
            else:
                raise RuntimeError(f"Module {module} does not define index set {index_set_name}.")

        # 4. Only record things if the dataset is somehow actually changed.
        if any((args.size, args.indices, args.drop, args.restore)):
            # Only record the seed additionally if it wasn't already passed in.
            record_metadata(
                module if args.size else None,
                out_dir=args.out,
                seed=None if args.seed else seed,
                clean=args.drop,
            )

        # 5. Dump data if a dump was requested.
        if args.dump is not None:
            database_instance.dump(dump_additional_args)
            dump_commands(args.out)

        # 6. Run 'analyze' on each field
        if args.analyze is not None:
            for field in fields(spec):
                await field.type.analyze(database_instance, collection_name, field.name)

if __name__ == "__main__":
    import asyncio

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
