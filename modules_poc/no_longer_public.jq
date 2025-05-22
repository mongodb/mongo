include "util" {search: "./"};

map(. as $decl | select((.visibility | test("^(use|needs)_replacement")) and
    (any(.used_from[]; is_submodule($decl) | not) | not))) |
group_by_visibility_parent
