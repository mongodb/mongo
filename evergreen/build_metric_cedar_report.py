import json
import sys
import argparse

parser = argparse.ArgumentParser(description='Print top n metrics from build metrics json files.')
parser.add_argument('--build-metrics', metavar='FILE', type=str, default='build_metrics.json',
                    help='Path to build metrics input json.')
parser.add_argument('--cache-pull-metrics', metavar='FILE', type=str, default='pull_cache.json',
                    help='Path to build metrics for cache pull input json.')
parser.add_argument('--cache-push-metrics', metavar='FILE', type=str, default='populate_cache.json',
                    help='Path to build metrics for cache push input json.')
args = parser.parse_args()

clean_build_metrics_json = args.build_metrics
populate_cache_metrics_json = args.cache_push_metrics
pull_cache_metrics_json = args.cache_pull_metrics
cedar_report = []

def single_metric_test(test_name, metric_name, value):
    return {
        "info": {
            "test_name": test_name,
        },
        "metrics": [
            {
                "name": metric_name,
                "value": round(value, 2)
            },
        ]
    }

with open(clean_build_metrics_json) as f:
    aggregated_build_tasks = {}
    build_metrics = json.load(f)
    for task in build_metrics['build_tasks']:
        outputs_key = ' '.join(task['outputs'])
        if outputs_key in aggregated_build_tasks:
            aggregated_build_tasks[outputs_key]['mem_usage'] += task['mem_usage']
            aggregated_build_tasks[outputs_key]['time'] += (task['end_time'] - task['start_time'])
        else:
            aggregated_build_tasks[outputs_key] = {
                'mem_usage': task['mem_usage'],
                'time': task['end_time'] - task['start_time'],
            }

    for output_files in aggregated_build_tasks:

        cedar_report.append({
            "info": {
                "test_name": output_files,
            },
            "metrics": [
                {
                    "name": "seconds",
                    "value": round(aggregated_build_tasks[output_files]['time'] / (10.0**9.0), 2)
                },
                {
                    "name": "MBs",
                    "value": round(aggregated_build_tasks[output_files]['mem_usage'] / 1024.0 / 1024.0, 2)
                },
            ]
        })

    cedar_report.append(single_metric_test("SCons memory usage", "MBs", build_metrics['scons_metrics']['memory']['post_build'] / 1024.0 / 1024.0))
    cedar_report.append(single_metric_test("System Memory Peak", "MBs", build_metrics['system_memory']['max'] / 1024.0 / 1024.0))
    cedar_report.append(single_metric_test("Total Build time", "seconds", build_metrics['scons_metrics']['time']['total']))
    cedar_report.append(single_metric_test("Total Build output size", "MBs", build_metrics['artifact_metrics']['total_artifact_size'] / 1024.0 / 1024.0))

    mongod_metrics = None
    for artifact in build_metrics['artifact_metrics']['artifacts']:
        if artifact['name'] == 'build/metrics/mongo/db/mongod':
            mongod_metrics = artifact
            break

    if mongod_metrics and mongod_metrics.get('bin_metrics'):
        cedar_report.append(single_metric_test("Mongod debug info size", "MBs", mongod_metrics['bin_metrics']['debug']['filesize'] / 1024.0 / 1024.0))

with open(populate_cache_metrics_json) as f:

    build_metrics = json.load(f)
    cedar_report.append({
        "info": {
            "test_name": "cache_push_time",
        },
        "metrics": [
            {
                "name": "seconds",
                "value": build_metrics["cache_metrics"]['push_time'] / (10.0**9.0)
            },
        ]
    })

with open(pull_cache_metrics_json) as f:

    build_metrics = json.load(f)
    cedar_report.append({
        "info": {
            "test_name": "cache_pull_time",
        },
        "metrics": [
            {
                "name": "seconds",
                "value": build_metrics["cache_metrics"]['pull_time'] / (10.0**9.0)
            },
        ]
    })

with open("build_metrics_cedar_report.json", "w") as fh:
    json.dump(cedar_report, fh)


