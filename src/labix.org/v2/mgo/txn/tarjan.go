package txn

import (
	"labix.org/v2/mgo/bson"
	"sort"
)

func tarjanSort(successors map[bson.ObjectId][]bson.ObjectId) [][]bson.ObjectId {
	// http://en.wikipedia.org/wiki/Tarjan%27s_strongly_connected_components_algorithm
	data := &tarjanData{
		successors: successors,
		nodes:      make([]tarjanNode, 0, len(successors)),
		index:      make(map[bson.ObjectId]int, len(successors)),
	}

	// Sort all nodes to stabilize the logic.
	var all []string
	for id := range successors {
		all = append(all, string(id))
	}
	sort.Strings(all)
	for _, strid := range all {
		id := bson.ObjectId(strid)
		if _, seen := data.index[id]; !seen {
			data.strongConnect(id)
		}
	}
	return data.output
}

type tarjanData struct {
	successors map[bson.ObjectId][]bson.ObjectId
	output     [][]bson.ObjectId

	nodes []tarjanNode
	stack []bson.ObjectId
	index map[bson.ObjectId]int
}

type tarjanNode struct {
	lowlink int
	stacked bool
}

type idList []bson.ObjectId

func (l idList) Len() int           { return len(l) }
func (l idList) Swap(i, j int)      { l[i], l[j] = l[j], l[i] }
func (l idList) Less(i, j int) bool { return l[i] < l[j] }

func (data *tarjanData) strongConnect(id bson.ObjectId) *tarjanNode {
	index := len(data.nodes)
	data.index[id] = index
	data.stack = append(data.stack, id)
	data.nodes = append(data.nodes, tarjanNode{index, true})
	node := &data.nodes[index]

	// Sort to stabilize the algorithm.
	succids := idList(data.successors[id])
	sort.Sort(succids)
	for _, succid := range succids {
		succindex, seen := data.index[succid]
		if !seen {
			succnode := data.strongConnect(succid)
			if succnode.lowlink < node.lowlink {
				node.lowlink = succnode.lowlink
			}
		} else if data.nodes[succindex].stacked {
			// Part of the current strongly-connected component.
			if succindex < node.lowlink {
				node.lowlink = succindex
			}
		}
	}

	if node.lowlink == index {
		// Root node; pop stack and output new
		// strongly-connected component.
		var scc []bson.ObjectId
		i := len(data.stack) - 1
		for {
			stackid := data.stack[i]
			stackindex := data.index[stackid]
			data.nodes[stackindex].stacked = false
			scc = append(scc, stackid)
			if stackindex == index {
				break
			}
			i--
		}
		data.stack = data.stack[:i]
		data.output = append(data.output, scc)
	}

	return node
}
