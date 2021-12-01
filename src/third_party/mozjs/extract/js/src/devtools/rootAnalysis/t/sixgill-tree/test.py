import re

test.compile("source.cpp")
test.computeGCTypes()
body = test.process_body(test.load_db_entry("src_body", re.compile(r'root_arg'))[0])

# Rendering positive and negative integers
marker1 = body.assignment_line('MARKER1')
equal(body.edge_from_line(marker1 + 2)['Exp'][1]['String'], '1')
equal(body.edge_from_line(marker1 + 3)['Exp'][1]['String'], '-1')

equal(body.edge_from_point(body.assignment_point('u1'))['Exp'][1]['String'], '1')
equal(body.edge_from_point(body.assignment_point('u2'))['Exp'][1]['String'], '4294967295')

assert('obj' in body['Variables'])
assert('random' in body['Variables'])
assert('other1' in body['Variables'])
assert('other2' in body['Variables'])

# Test function annotations
js_GC = test.process_body(test.load_db_entry("src_body", re.compile(r'js_GC'))[0])
annotations = js_GC['Variables']['void js_GC()']['Annotation']
assert(annotations)
found_call_tag = False
for annotation in annotations:
    (annType, value) = annotation['Name']
    if annType == 'Tag' and value == 'GC Call':
        found_call_tag = True
assert(found_call_tag)

# Test type annotations

# js::gc::Cell first
cell = test.load_db_entry("src_comp", 'js::gc::Cell')[0]
assert(cell['Kind'] == 'Struct')
annotations = cell['Annotation']
assert(len(annotations) == 1)
(tag, value) = annotations[0]['Name']
assert(tag == 'Tag')
assert(value == 'GC Thing')

# Check JSObject inheritance.
JSObject = test.load_db_entry("src_comp", 'JSObject')[0]
bases = [ b['Base'] for b in JSObject['CSUBaseClass'] ]
assert('js::gc::Cell' in bases)
assert('Bogon' in bases)
assert(len(bases) == 2)

# Check type analysis
gctypes = test.load_gcTypes()
assert('js::gc::Cell' in gctypes['GCThings'])
assert('JustACell' in gctypes['GCThings'])
assert('JSObject' in gctypes['GCThings'])
assert('SpecialObject' in gctypes['GCThings'])
assert('UnrootedPointer' in gctypes['GCPointers'])
assert('Bogon' not in gctypes['GCThings'])
assert('Bogon' not in gctypes['GCPointers'])
assert('ErrorResult' not in gctypes['GCPointers'])
assert('OkContainer' not in gctypes['GCPointers'])
assert('class Rooted<JSObject*>' not in gctypes['GCPointers'])
