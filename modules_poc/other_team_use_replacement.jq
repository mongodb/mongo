include "util" {search: "./"};

map(. as $decl | select((.visibility | test("^use_replacement")) and
    any(.used_from[]; (is_submodule($decl) | not) and .mod != "__NONE__"))) |
group_by_visibility_parent_non_ns
