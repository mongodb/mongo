def is_submodule($d): (.mod == $d.mod) or (.mod | startswith("\($d.mod)."));

def _group_by_vis_map(vis_from):
    map({
        usr: vis_from.usr,
        display_name: vis_from.display_name,
        mod: .[0].mod,
        loc: .[0].loc,
        kind: .[0].kind,
        used_from: [.[].used_from[]] |
            group_by(.mod) |
            map({
                mod: .[0].mod,
                locs: [.[].locs[]] | map(split("\t") | .[1]) | unique,
            }),
    });

def group_by_visibility_parent_non_ns:
    group_by(.vis_from_non_ns.usr) | _group_by_vis_map(.[0].vis_from_non_ns);

def group_by_visibility_parent:
    group_by(.vis_from.usr) | _group_by_vis_map(.[0].vis_from);
