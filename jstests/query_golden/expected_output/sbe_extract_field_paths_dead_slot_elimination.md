## 1. dead path with a separate input
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : null,
			"dead" : {
				"$push" : "$b.x"
			},
			"live" : {
				"$push" : "$a.x"
			}
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"live" : 1
		}
	}
]
```
### Options
```json
{ "hint" : { "$natural" : 1 }, "allowDiskUse" : true }
```
### Without blueprint rewrites
Results:
```text
{ "live" : [ 2, [ 4, 6 ] ] }
```
SBE stages:
```text
[4] project [s12 = newBsonObj("live", s9)] 
[3] project [s11 = null] 
[3] group [] [] [s7 = _internalPush::accumulate(s7, s6), s9 = _internalPush::accumulate(s9, s5)] spillSlots[s8, s10] mergingExprs[_internalPush::merge(s8), _internalPush::merge(s10)] 
[3] extract_field_paths inputs[s3 = Get(a)/Id, s4 = Get(b)/Id] outputs[s5 = Get(a)/Traverse/Get(x)/Id, s6 = Get(b)/Traverse/Get(x)/Id] 
[1] scan generic [s1 = record, s2 = recordId] [s3 = a, s4 = b] @"UUID" 
```
### With blueprint rewrites
Results:
```text
{ "live" : [ 2, [ 4, 6 ] ] }
```
SBE stages:
```text
[4] project [s12 = newBsonObj("live", s9)] 
[3] group [] [] [s9 = _internalPush::accumulate(s9, s5)] spillSlots[s10] mergingExprs[_internalPush::merge(s10)] 
[3] extract_field_paths inputs[s3 = Get(a)/Id] outputs[s5 = Get(a)/Traverse/Get(x)/Id] 
[1] scan generic [s1 = record, s2 = recordId] [s3 = a] @"UUID" 
```

## 2. dead path sharing a live input
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : null,
			"dead" : {
				"$push" : "$a.y"
			},
			"live" : {
				"$push" : "$a.x"
			}
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"live" : 1
		}
	}
]
```
### Options
```json
{ "hint" : { "$natural" : 1 }, "allowDiskUse" : true }
```
### Without blueprint rewrites
Results:
```text
{ "live" : [ 2, [ 4, 6 ] ] }
```
SBE stages:
```text
[4] project [s11 = newBsonObj("live", s8)] 
[3] project [s10 = null] 
[3] group [] [] [s6 = _internalPush::accumulate(s6, s5), s8 = _internalPush::accumulate(s8, s4)] spillSlots[s7, s9] mergingExprs[_internalPush::merge(s7), _internalPush::merge(s9)] 
[3] extract_field_paths inputs[s3 = Get(a)/Id] outputs[s4 = Get(a)/Traverse/Get(x)/Id, s5 = Get(a)/Traverse/Get(y)/Id] 
[1] scan generic [s1 = record, s2 = recordId] [s3 = a] @"UUID" 
```
### With blueprint rewrites
Results:
```text
{ "live" : [ 2, [ 4, 6 ] ] }
```
SBE stages:
```text
[4] project [s11 = newBsonObj("live", s8)] 
[3] group [] [] [s8 = _internalPush::accumulate(s8, s4)] spillSlots[s9] mergingExprs[_internalPush::merge(s9)] 
[3] extract_field_paths inputs[s3 = Get(a)/Id] outputs[s4 = Get(a)/Traverse/Get(x)/Id] 
[1] scan generic [s1 = record, s2 = recordId] [s3 = a] @"UUID" 
```

## 3. all paths dead
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : null,
			"dead" : {
				"$push" : "$b.x"
			},
			"alsoDead" : {
				"$push" : "$a.x"
			}
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"ok" : {
				"$literal" : 1
			}
		}
	}
]
```
### Options
```json
{ "hint" : { "$natural" : 1 }, "allowDiskUse" : true }
```
### Without blueprint rewrites
Results:
```text
{ "ok" : 1 }
```
SBE stages:
```text
[4] project [s13 = newBsonObj("ok", s12)] 
[4] project [s12 = 1L] 
[3] project [s11 = null] 
[3] group [] [] [s7 = _internalPush::accumulate(s7, s6), s9 = _internalPush::accumulate(s9, s5)] spillSlots[s8, s10] mergingExprs[_internalPush::merge(s8), _internalPush::merge(s10)] 
[3] extract_field_paths inputs[s3 = Get(a)/Id, s4 = Get(b)/Id] outputs[s5 = Get(a)/Traverse/Get(x)/Id, s6 = Get(b)/Traverse/Get(x)/Id] 
[1] scan generic [s1 = record, s2 = recordId] [s3 = a, s4 = b] @"UUID" 
```
### With blueprint rewrites
Results:
```text
{ "ok" : 1 }
```
SBE stages:
```text
[4] project [s13 = newBsonObj("ok", s12)] 
[4] project [s12 = 1L] 
[3] group [] [] [] spillSlots[] mergingExprs[] 
[1] scan generic [s1 = record, s2 = recordId] @"UUID" 
```

