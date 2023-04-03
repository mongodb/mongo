#!/usr/bin/env python3
import json
import sys
import glob
import argparse
import statistics

from typing import Dict, List, Any

parser = argparse.ArgumentParser(description='Combine metrics json files into a single file.')
parser.add_argument(
    '--prefix-name', metavar='FILES', action='append', default=[], help=
    'Prefix path to collect json files of the form "{prefix_path}*.json" for combining into a single json: "{prefix_path}.json"'
)
parser.add_argument('unittest_args', nargs='*')
args = parser.parse_args()


def set_lowest(existing: Dict, current: Dict, key: str):
    existing_data = existing.get(key)
    current_data = existing.get(key)

    if existing_data and current_data and existing_data > current_data:
        existing[key] = current_data

    elif not existing_data and current_data:
        existing[key] = current_data


def set_greatest(existing: Dict, current: Dict, key: str):
    existing_data = existing.get(key)
    current_data = current.get(key)

    if existing_data and current_data and existing_data < current_data:
        existing[key] = current_data

    elif not existing_data and current_data:
        existing[key] = current_data


def combine_command_line(existing: Dict, current: Dict, key: str):
    existing_data = existing.get(key)
    current_data = current.get(key)

    if not existing_data:
        existing[key] = current_data
    else:
        existing_data = existing.get(key).split()
        current_data = current.get(key).split()
        for current_arg in current_data:
            if current_arg not in existing_data:
                existing_data.append(current_arg)

        existing[key] = ' '.join(existing_data)


def if_set_should_match(existing: Dict, current: Dict, key: str):
    existing_data = existing.get(key)
    current_data = current.get(key)

    if existing_data and current_data and existing_data != current_data:
        raise Exception(
            f"Expected data to match - existing: {existing_data}, current: {current_data}")

    elif not existing_data and current_data:
        existing[key] = current_data


def recalc_list_indexes(target_list: List):
    index_found = None

    for index, elem in enumerate(target_list):
        if index_found is None and index == 0:
            index_found = elem.get('array_index')

        if (index_found is None
                and elem.get('array_index')) or (index_found is not None
                                                 and elem.get('array_index') is None):
            raise Exception("Attempted to combine list with incompat index keys.")

        if elem.get('array_index') is not None:
            elem['array_index'] = index


def extend_list(existing: Dict, current: Dict, key: str):
    existing_data = existing.get(key)
    current_data = current.get(key)

    if existing_data and current_data:
        existing_data.extend(current_data)

    elif not existing_data and current_data:
        existing[key] = current_data

    recalc_list_indexes(existing[key])


def extend_list_no_dups(existing: Dict, current: Dict, key: str, list_unqiue_key: str):
    extend_list(existing, current, key)
    unique_list = {}
    for elem in existing[key]:
        if elem.get('array_index') is not None:
            elem['array_index'] = -1
        if elem[list_unqiue_key] not in unique_list:
            unique_list[elem[list_unqiue_key]] = elem
        elif unique_list[elem[list_unqiue_key]] != elem:
            if sys.platform == 'win32':
                # build metrics performs a clean and pull from cachse and windows does not produce the same output
                # with the same input (non deterministic), so we can not make these garuntees and or perform
                # this check.
                pass
            else:
                raise Exception(
                    f"Expected data to match - existing: {unique_list[elem[list_unqiue_key]]}, current: {elem}"
                )

    existing[key] = list(unique_list.values())

    recalc_list_indexes(existing[key])


def combine_system_memory(existing: Dict, current: Dict):

    extend_list(existing, current, 'mem_over_time')
    set_greatest(existing, current, 'max')
    existing['arithmetic_mean'] = statistics.mean(
        [mem['memory'] for mem in existing['mem_over_time']])
    set_lowest(existing, current, 'start_mem')


def combine_artifact_metrics(existing: Dict, current: Dict):
    extend_list_no_dups(existing, current, 'artifacts', 'name')
    existing['total_artifact_size'] = sum([artifact['size'] for artifact in existing['artifacts']])
    existing['num_artifacts'] = len(existing['artifacts'])


def combine_cache_metrics(existing: Dict, current: Dict):
    extend_list_no_dups(existing, current, 'cache_artifacts', 'name')
    existing['push_time'] += current['push_time']
    existing['pull_time'] += current['pull_time']
    existing['cache_size'] += sum([cache['size'] for cache in existing['cache_artifacts']])


def combine_scons_metrics(existing: Dict, current: Dict):
    try:
        set_greatest(existing['memory'], current['memory'], 'pre_read')
        set_greatest(existing['memory'], current['memory'], 'post_read')
        set_greatest(existing['memory'], current['memory'], 'pre_build')
        set_greatest(existing['memory'], current['memory'], 'post_build')
    except KeyError:
        if sys.platform == 'darwin':
            # MacOS has known memory reporting issues, although this is not directly related to scons which does not use
            # psutil for this case, I think both use underlying OS calls to determine the memory: https://github.com/giampaolo/psutil/issues/1908
            pass

    existing['time']['total'] += current['time']['total']
    existing['time']['sconscript_exec'] += current['time']['sconscript_exec']
    existing['time']['scons_exec'] += current['time']['scons_exec']
    existing['time']['command_exec'] += current['time']['command_exec']

    for new_item in current['counts']:
        found_new_item = False
        for existing_item in existing['counts']:
            if existing_item['item_name'] == new_item['item_name']:
                found_new_item = True
                set_greatest(existing_item, new_item, 'pre_read')
                set_greatest(existing_item, new_item, 'post_read')
                set_greatest(existing_item, new_item, 'pre_build')
                set_greatest(existing_item, new_item, 'post_build')
                break
        if not found_new_item:
            existing['counts'].append(new_item)


for prefix_name in args.prefix_name:

    combined_json: Dict[str, Any] = {'combined_files': []}

    json_files = glob.glob(f'{prefix_name}*.json')
    for json_file in json_files:
        if json_file.endswith('chrome-tracer.json'):
            continue

        with open(json_file) as fjson:
            combined_json['combined_files'].append(json_file)
            current_json = json.load(fjson)

            set_lowest(combined_json, current_json, 'start_time')
            set_greatest(combined_json, current_json, 'end_time')
            if_set_should_match(combined_json, current_json, 'evg_id')
            if_set_should_match(combined_json, current_json, 'variant')
            combine_command_line(combined_json, current_json, 'scons_command')

            ###########################
            # system_memory
            if 'system_memory' not in combined_json:
                combined_json['system_memory'] = current_json.get('system_memory', {})
            else:
                combine_system_memory(combined_json['system_memory'], current_json['system_memory'])

            ############################
            # artifact_metrics
            if 'artifact_metrics' not in combined_json:
                combined_json['artifact_metrics'] = current_json.get('artifact_metrics', {})
            else:
                combine_artifact_metrics(combined_json['artifact_metrics'],
                                         current_json['artifact_metrics'])

            ############################
            # build_tasks
            if 'build_tasks' not in combined_json:
                combined_json['build_tasks'] = current_json.get('build_tasks', [])
            else:
                extend_list(combined_json, current_json, 'build_tasks')

            ############################
            # cache_metrics
            if 'cache_metrics' not in combined_json:
                combined_json['cache_metrics'] = current_json.get('cache_metrics', {})
            else:
                combine_cache_metrics(combined_json['cache_metrics'], current_json['cache_metrics'])

            ############################
            # libdeps_metrics
            if 'libdeps_metrics' in combined_json and current_json.get('libdeps_metrics'):
                raise Exception("found a second libdeps_metrics dataset in {json_file}")
            if 'libdeps_metrics' not in combined_json and current_json.get('libdeps_metrics'):
                combined_json['libdeps_metrics'] = current_json.get('libdeps_metrics')

            ############################
            # scons_metrics
            if 'scons_metrics' not in combined_json:
                combined_json['scons_metrics'] = current_json.get('scons_metrics', {})
            else:
                combine_scons_metrics(combined_json['scons_metrics'], current_json['scons_metrics'])

    with open(f'{prefix_name}.json', 'w') as out:
        json.dump(combined_json, out, indent=4, sort_keys=True)
