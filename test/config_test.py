import config

def parse(s):
    print "Parsing '%s':" % s

    c = config.WT_CONFIG()
    k = config.WT_CONFIG_ITEM()
    v = config.WT_CONFIG_ITEM()

    config.config_init(c, s, len(s))
    ret = config.config_next(c, k, v)
    while ret == 0:
        print " => '%s' = '%s'" % (k.str[:k.len], v.str[:v.len])
        ret = config.config_next(c, k, v)

    # XXX hard-coding WT_NOTFOUND until we fix this
    if ret != -31801:
        print "Last call to config_next failed with %d" % ret

if __name__ == '__main__':
    parse("create")
    parse("create,cachesize=10MB")
    parse('create,cachesize=10MB,path="/foo/bar"')
    parse('columns=(first,second, third)')
    parse('key_format="S", value_format="5sq", columns=(first,second, third)')
    parse('key_columns=(first=S),value_columns=(second="5s", third=q)')
    parse(',,columns=(first=S,second="5s", third=q),,')
    parse('index.country_year=(country,year),key_format=r,colgroup.population=(population),columns=(id,country,year,population),value_format=5sHQ')

    import json
    parse(json.dumps({'hello' : 'world', 'columns' : ('one', 'two', 'three')}))
    
    parse(json.dumps({
        "key_format" : "r",
        "value_format" : "5sHQ",
        "columns" : ("id", "country", "year", "population"),
        "colgroup.population" : ("population",),
        "index.country_year" : ("country","year")
    }))
