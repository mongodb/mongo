"""Compatibility names for Python generators with multiple outputs."""

load(
    "//bazel/config:render_template.bzl",
    "render_templates",
    _render_templates_rule = "render_templates_rule",
)

python_genrule_rule = _render_templates_rule

def python_genrule(name, python_file, python_libs = [], tags = [], **kwargs):
    render_templates(
        name = name,
        python_file = python_file,
        python_libs = python_libs,
        tags = tags,
        srcs = kwargs.pop("srcs", []),
        cmd = kwargs.pop("cmd", []),
        **kwargs
    )
