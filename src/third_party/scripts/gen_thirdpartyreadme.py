from jinja2 import Environment, FileSystemLoader
import sys
import os
import json
import bisect
import logging
from functools import reduce

SBOM_PATH = "../../../sbom.json"
TEMPLATE_PATH = "README.third_party.md.template"
README_PATH = "../../../README.third_party.md"

logging.basicConfig(level=logging.INFO,
                    format='%(asctime)s - %(levelname)s - %(message)s')


def main():
    test_filepaths()
    sbom = load_sbom()

    component_chart = sbom_to_component_chart(sbom)
    right_pad_chart_values(component_chart)
    component_chart_string = chart_to_string(component_chart)

    component_links_string = sbom_to_component_links_string(sbom)

    wiredtiger_chart = sbom_to_wiredtiger_chart(sbom)
    right_pad_chart_values(wiredtiger_chart)
    wiredtiger_chart_string = chart_to_string(wiredtiger_chart)

    template_data = {
        "component_chart": component_chart_string,
        "component_links": component_links_string,
        "wiredtiger_chart": wiredtiger_chart_string
    }
    create_markdown_with_template(template_data)


def test_filepaths() -> None:
    for filepath in [SBOM_PATH, TEMPLATE_PATH]:
        if not os.path.exists(filepath):
            logging.error("Error: %s does not exist. Exiting.", filepath)
            sys.exit(1)


def load_sbom() -> dict:
    try:
        with open(SBOM_PATH, 'r') as file:
            sbom = json.load(file)
            logging.info("%s JSON data loaded.", SBOM_PATH)
            return sbom
    except json.JSONDecodeError as e:
        logging.error("Error decoding %s JSON: %e Exiting.", SBOM_PATH, e)
        sys.exit(1)


def sbom_to_component_chart(sbom: dict) -> list[list[str]]:
    components = sbom["components"]
    component_chart = []

    for component in components:
        check_component_validity(component)
        name = component["name"]
        license_string = []
        for lic in component["licenses"]:
            for key in ["id", "name"]:
                if key in lic["license"]:
                    license_string.append(lic["license"][key])
        license_string = ", ".join(license_string)
        version = component["version"]
        emits_persisted_data = "unknown"
        for prop in component["properties"]:
            k, v = prop["name"], prop["value"]
            if k == "emits_persisted_data":
                emits_persisted_data = ("", "✗")[v == "true"]
        distributed_in_release_binaries = (
            "", "✗")[component["scope"] == "required"]

        row = [
            item.replace(
                "|",
                "") for item in [
                f"[{name}]",
                license_string,
                version,
                emits_persisted_data,
                distributed_in_release_binaries]]
        bisect.insort(component_chart, row, key=lambda c: c[0].lower())

    component_chart.insert(0,
                           ["Name",
                            "License",
                            "Vendored Version",
                            "Emits persisted data",
                            "Distributed in Release Binaries"])
    return component_chart


def sbom_to_component_links_string(sbom: dict) -> list[list[str]]:
    components = sbom["components"]
    link_list = []

    for component in components:
        check_component_validity(component)
        info_link = get_component_info_link(component)
        bisect.insort(
            link_list,
            f"[{component['name'].replace('|','')}]: {info_link}")

    return "\n".join(link_list)


def sbom_to_wiredtiger_chart(sbom: dict) -> list[list[str]]:
    components = sbom["components"]
    wiredtiger_chart = [["Name"]]

    for component in components:
        check_component_validity(component)
        locations = get_component_locations(component)
        for location in locations:
            if location.startswith("src/third_party/wiredtiger/"):
                bisect.insort(
                    wiredtiger_chart, [
                        component["name"].replace(
                            "|", "")])

    return wiredtiger_chart


def check_component_validity(component) -> None:
    for required_key in ["name", "version", "licenses"]:
        if required_key not in component:
            logging.error(
                "Error: no key %s found in json. Exiting. JSON dump:",
                required_key)
            logging.error(json.dumps(component))
            sys.exit(1)


def get_component_info_link(component) -> str:
    name = component["name"]
    links = []
    for prop in component["properties"]:
        k, v = prop["name"], prop["value"]
        if k == "info_link":
            links.append(v)
    if len(links) != 1:
        logging.warning(
            "Warning: Expected 1 info_link for %s. Got %d:",
            name,
            len(links))
        if len(links) > 1:
            logging.warning(" ".join(links))
            logging.warning("Using first link only.")
        else:
            logging.warning(
                "Falling back to `purl` value: %s",
                component['purl'])
            links.append(component["purl"])
    return links[0]


def get_component_locations(component) -> list[str]:
    if "evidence" not in component or "occurrences" not in component["evidence"]:
        return []
    return [occurence["location"]
            for occurence in component["evidence"]["occurrences"]]


def right_pad_chart_values(chart: list[list[str]]) -> list[list[str]]:
    h, w = len(chart), len(chart[0])
    max_lens = [3 for _ in range(w)]
    for row in chart:
        for c in range(0, w):
            max_lens[c] = max(max_lens[c], len(row[c]))

    for r in range(0, h):
        for c in range(0, w):
            chart[r][c] = chart[r][c].ljust(max_lens[c])
    chart.insert(1, ["-" * max_len for max_len in max_lens])


def chart_to_string(chart: list[list[str]]) -> str:
    chart = [" | ".join(row) for row in chart]
    chart = "\n".join(["| " + row + " |" for row in chart])
    return chart


def create_markdown_with_template(data: str) -> None:
    file_loader = FileSystemLoader('.')
    env = Environment(loader=file_loader)
    template = env.get_template(TEMPLATE_PATH)
    output = template.render(data)

    with open(README_PATH, 'w') as f:
        f.write("[DO NOT MODIFY THIS FILE MANUALLY. It is generated by src/third_party/tools/gen_thirdpartyreadme.py]: #\n\n")
        f.write(output)
        f.write("\n")

    logging.info("Markdown file created successfully.")


if __name__ == "__main__":
    main()
