#!/usr/bin/env python3
import os
import sys
import platform

from shrub.v2 import ShrubProject, Task, BuildVariant, FunctionCall, TaskGroup
from shrub.v2.command import BuiltInCommand


def main():

    tasks = {
        'windows_tasks': {},
        'linux_x86_64_tasks': {},
        'linux_arm64_tasks': {},
        'macos_tasks': {},
    }

    tasks_prefixes = {
        'windows_tasks': 'build_metrics_msvc',
        'linux_x86_64_tasks': 'build_metrics_x86_64',
        'linux_arm64_tasks': 'build_metrics_arm64',
        'macos_tasks': 'build_metrics_xcode',
    }

    task_group_targets = {
        'dynamic': [
            "install-devcore",
            "install-all-meta generate-libdeps-graph",
        ], "static": [
            "install-devcore",
            "install-all-meta-but-not-unittests",
        ]
    }

    def create_build_metric_task_steps(task_build_flags, task_targets, split_num):

        evg_flags = f"--debug=time,count,memory VARIANT_DIR=metrics BUILD_METRICS_EVG_TASK_ID={os.environ['task_id']} BUILD_METRICS_EVG_BUILD_VARIANT={os.environ['build_variant']}"
        cache_flags = "--cache-dir=$PWD/scons-cache-{split_num} --cache-signature-mode=validate"

        scons_task_steps = [
            f"{evg_flags} --build-metrics=build_metrics_{split_num}.json",
            f"{evg_flags} {cache_flags} --cache-populate --build-metrics=populate_cache_{split_num}.json",
            f"{evg_flags} --clean",
            f"{evg_flags} {cache_flags} --build-metrics=pull_cache_{split_num}.json",
        ]

        task_steps = [
            FunctionCall(
                "scons compile", {
                    "patch_compile_flags": f"{task_build_flags} {step_flags}",
                    "targets": task_targets,
                    "compiling_for_test": "true",
                }) for step_flags in scons_task_steps
        ]
        return task_steps

    def create_build_metric_task_list(task_list, link_model, build_flags):

        tasks[task_list][link_model] = []
        prefix = tasks_prefixes[task_list]
        index = 0
        for index, target in enumerate(task_group_targets[link_model]):
            tasks[task_list][link_model].append(
                Task(f"{prefix}_{link_model}_build_split_{index}_{target.replace(' ', '_')}",
                     create_build_metric_task_steps(build_flags, target, index)))
        tasks[task_list][link_model].append(
            Task(f"{prefix}_{link_model}_build_split_{index+1}_combine_metrics", [
                FunctionCall("combine build metrics"),
                FunctionCall("attach build metrics"),
                FunctionCall("print top N metrics")
            ]))

    #############################
    if sys.platform == 'win32':
        build_flags = '--cache=nolinked'

        create_build_metric_task_list(
            'windows_tasks',
            'static',
            build_flags,
        )

    ##############################
    elif sys.platform == 'darwin':

        for link_model in ['dynamic', 'static']:

            build_flags = f"--link-model={link_model} --force-macos-dynamic-link" + (
                ' --cache=nolinked' if link_model == 'static' else " --cache=all")

            create_build_metric_task_list(
                'macos_tasks',
                link_model,
                build_flags,
            )

    ##############################
    else:
        for toolchain in ['v4']:
            # possibly we want to add clang to the mix here, so leaving as an easy drop in
            for compiler in ['gcc']:
                for link_model in ['dynamic', 'static']:

                    build_flags = (
                        "BUILD_METRICS_BLOATY=/opt/mongodbtoolchain/v4/bin/bloaty " +
                        f"--variables-files=etc/scons/mongodbtoolchain_{toolchain}_{compiler}.vars "
                        + f"--link-model={link_model}" +
                        (' --cache=nolinked' if link_model == 'static' else " --cache=all"))

                    create_build_metric_task_list(
                        'linux_x86_64_tasks',
                        link_model,
                        build_flags,
                    )

                    create_build_metric_task_list(
                        'linux_arm64_tasks',
                        link_model,
                        build_flags,
                    )

    def create_task_group(target_platform, tasks):
        task_group = TaskGroup(
            name=f'build_metrics_{target_platform}_task_group_gen',
            tasks=tasks,
            max_hosts=1,
            setup_group=[
                BuiltInCommand("manifest.load", {}),
                FunctionCall("git get project and add git tag"),
                FunctionCall("set task expansion macros"),
                FunctionCall("f_expansions_write"),
                FunctionCall("kill processes"),
                FunctionCall("cleanup environment"),
                FunctionCall("set up venv"),
                FunctionCall("upload pip requirements"),
                FunctionCall("get all modified patch files"),
                FunctionCall("f_expansions_write"),
                FunctionCall("configure evergreen api credentials"),
                FunctionCall("get buildnumber"),
                FunctionCall("f_expansions_write"),
                FunctionCall("generate compile expansions"),
                FunctionCall("f_expansions_write"),
            ],
            setup_task=[
                FunctionCall("f_expansions_write"),
                FunctionCall("apply compile expansions"),
                FunctionCall("set task expansion macros"),
                FunctionCall("f_expansions_write"),
            ],
            teardown_group=[
                FunctionCall("f_expansions_write"),
                FunctionCall("cleanup environment"),
            ],
            teardown_task=[
                FunctionCall("f_expansions_write"),
                FunctionCall("attach scons logs"),
                FunctionCall("kill processes"),
                FunctionCall("save disk statistics"),
                FunctionCall("save system resource information"),
                FunctionCall("remove files",
                             {'files': ' '.join(['src/build', 'src/scons-cache', '*.tgz'])}),
            ],
            setup_group_can_fail_task=True,
        )
        return task_group

    if sys.platform == 'win32':
        variant = BuildVariant(
            name="enterprise-windows-build-metrics",
            activate=True,
        )
        variant.add_task_group(
            create_task_group('windows', tasks['windows_tasks']['static']),
            ['windows-vsCurrent-xlarge'])
    elif sys.platform == 'darwin':
        variant = BuildVariant(
            name="macos-enterprise-build-metrics",
            activate=True,
        )
        for link_model, tasks in tasks['macos_tasks'].items():
            variant.add_task_group(create_task_group(f'macos_{link_model}', tasks), ['macos-1100'])
    else:
        if platform.machine() == 'x86_64':
            variant = BuildVariant(
                name="enterprise-rhel-80-64-bit-build-metrics",
                activate=True,
            )
            for link_model, tasks in tasks['linux_x86_64_tasks'].items():
                variant.add_task_group(
                    create_task_group(f'linux_X86_64_{link_model}', tasks), ['rhel80-xlarge'])
        else:
            variant = BuildVariant(
                name="enterprise-rhel-80-aarch64-build-metrics",
                activate=True,
            )
            for link_model, tasks in tasks['linux_arm64_tasks'].items():
                variant.add_task_group(
                    create_task_group(f'linux_arm64_{link_model}', tasks),
                    ['amazon2022-arm64-large'])

    project = ShrubProject({variant})
    with open('build_metrics_task_gen.json', 'w') as fout:
        fout.write(project.json())


if __name__ == "__main__":
    main()
