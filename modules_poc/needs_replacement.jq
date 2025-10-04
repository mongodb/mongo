include "util" {search: "./"};

map(. as $decl | select((.visibility | test("^needs_replacement")) and
    any(.used_from[]; is_submodule($decl) | not))) |
group_by_visibility_parent_non_ns
