#!/usr/bin/env python
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

from collections import defaultdict

# Simplified version of the body info.
class Body(dict):
    def __init__(self, body):
        self['BlockIdKind'] = body['BlockId']['Kind']
        if 'Variable' in body['BlockId']:
            self['BlockName'] = body['BlockId']['Variable']['Name'][0].split("$")[-1]
        loc = body['Location']
        self['LineRange'] = (loc[0]['Line'], loc[1]['Line'])
        self['Filename'] = loc[0]['CacheString']
        self['Edges'] = body.get('PEdge', [])
        self['Points'] = { i: p['Location']['Line'] for i, p in enumerate(body['PPoint'], 1) }
        self['Index'] = body['Index']
        self['Variables'] = { x['Variable']['Name'][0].split("$")[-1]: x['Type'] for x in body['DefineVariable'] }

        # Indexes
        self['Line2Points'] = defaultdict(list)
        for point, line in self['Points'].items():
            self['Line2Points'][line].append(point)
        self['SrcPoint2Edges'] = defaultdict(list)
        for edge in self['Edges']:
            src, dst = edge['Index']
            self['SrcPoint2Edges'][src].append(edge)
        self['Line2Edges'] = defaultdict(list)
        for (src, edges) in self['SrcPoint2Edges'].items():
            line = self['Points'][src]
            self['Line2Edges'][line].extend(edges)

    def edges_from_line(self, line):
        return self['Line2Edges'][line]

    def edge_from_line(self, line):
        edges = self.edges_from_line(line)
        assert(len(edges) == 1)
        return edges[0]

    def edges_from_point(self, point):
        return self['SrcPoint2Edges'][point]

    def edge_from_point(self, point):
        edges = self.edges_from_point(point)
        assert(len(edges) == 1)
        return edges[0]

    def assignment_point(self, varname):
        for edge in self['Edges']:
            if edge['Kind'] != 'Assign':
                continue
            dst = edge['Exp'][0]
            if dst['Kind'] != 'Var':
                continue
            if dst['Variable']['Name'][0] == varname:
                return edge['Index'][0]
        raise Exception("assignment to variable %s not found" % varname)

    def assignment_line(self, varname):
        return self['Points'][self.assignment_point(varname)]
