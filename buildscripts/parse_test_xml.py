import os
import xml.etree.ElementTree as ET


def parse_test_xml(xml_file: str) -> ET.ElementTree:
    """Parse the test.xml file and return the ElementTree object. Throws with diagnostics if unparsable."""

    # Check if file exists and has content
    if not os.path.exists(xml_file):
        raise Exception(f"Failed to parse {xml_file}: file does not exist")

    with open(xml_file, "r", encoding="utf-8") as f:
        content = f.read().strip()

    if not content:
        raise Exception(f"Failed to parse {xml_file}: file is empty")

    try:
        root = ET.fromstring(content)
        return ET.ElementTree(root)

    except (ET.ParseError, UnicodeDecodeError) as e:
        print(f"Failed to parse {xml_file}: {e}")
        print(content)
        raise
