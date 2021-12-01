// const cfg = loadCFG(scriptArgs[0]);
// dump_CFG(cfg);

function loadCFG(filename) {
  const data = os.file.readFile(filename);
  return JSON.parse(data);
}

function dump_CFG(cfg) {
  for (const body of cfg)
    dump_body(body);
}

function dump_body(body, src, dst) {
  const {BlockId,Command,DefineVariable,Index,Location,PEdge,PPoint,Version} = body;

  const [mangled, unmangled] = splitFunction(BlockId.Variable.Name[0]);
  print(`${unmangled} at ${Location[0].CacheString}:${Location[0].Line}`);

  if (src === undefined) {
    for (const def of DefineVariable)
      print(str_definition(def));
    print("");
  }

  for (const edge of PEdge) {
    if (src === undefined || edge.Index[0] == src) {
      if (dst == undefined || edge.Index[1] == dst)
        print(str_edge(edge, body));
    }
  }
}

function str_definition(def) {
  const {Type, Variable} = def;
  return `define ${str_Variable(Variable)} : ${str_Type(Type)}`;
}

function badFormat(what, val) {
  printErr("Bad format of " + what + ": " + JSON.stringify(val, null, 4));
  printErr((new Error).stack);
}

function str_Variable(variable) {
  if (variable.Kind == 'Return')
    return '<returnval>';
  else if (variable.Kind == 'This')
    return 'this';

  try {
    return variable.Name[1];
  } catch(e) {
    badFormat("variable", variable);
  }
}

function str_Type(type) {
  try {
    const {Kind, Type, Name, TypeFunctionArguments} = type;
    if (Kind == 'Pointer')
      return str_Type(Type) + "*";
    else if (Kind == 'CSU') {
      return Name;
    }

    return Kind;
  } catch(e) {
    badFormat("type", type);
  }
}

var OpCodeNames = {
  'LessEqual': ['<=', '>'],
  'LessThan': ['<', '>='],
  'GreaterEqual': ['>=', '<'],
  'Greater': ['>', '<='],
  'Plus': '+',
  'Minus': '-',
};

function opcode_name(opcode, invert) {
  if (opcode in OpCodeNames) {
    const name = OpCodeNames[opcode];
    if (invert === undefined)
      return name;
    return name[invert ? 1 : 0];
  } else {
    if (invert === undefined)
      return opcode;
    return (invert ? '!' : '') + opcode;
  }
}

function str_value(val, env, options) {
  const {Kind, Variable, String, Exp} = val;
  if (Kind == 'Var')
    return str_Variable(Variable);
  else if (Kind == 'Drf') {
    // Suppress the vtable lookup dereference
    if (Exp[0].Kind == 'Fld' && "FieldInstanceFunction" in Exp[0].Field)
      return str_value(Exp[0], env);
    const exp = str_value(Exp[0], env);
    if (options && options.noderef)
      return exp;
    return "*" + exp;
  } else if (Kind == 'Fld') {
    const {Exp, Field} = val;
    const name = Field.Name[0];
    if ("FieldInstanceFunction" in Field) {
      return Field.FieldCSU.Type.Name + "::" + name;
    }
    const container = str_value(Exp[0]);
    if (container.startsWith("*"))
      return container.substring(1) + "->" + name;
    return container + "." + name;
  } else if (Kind == 'Empty') {
    return '<unknown>';
  } else if (Kind == 'Binop') {
    const {OpCode} = val;
    const op = opcode_name(OpCode);
    return `${str_value(Exp[0], env)} ${op} ${str_value(Exp[1], env)}`;
  } else if (Kind == 'Unop') {
    const exp = str_value(Exp[0], env);
    const {OpCode} = val;
    if (OpCode == 'LogicalNot')
      return `not ${exp}`;
    return `${OpCode}(${exp})`;
  } else if (Kind == 'Index') {
    const index = str_value(Exp[1], env);
    if (Exp[0].Kind == 'Drf')
      return `${str_value(Exp[0], env, {noderef:true})}[${index}]`;
    else
      return `&${str_value(Exp[0], env)}[${index}]`;
  } else if (Kind == 'NullTest') {
    return `nullptr == ${str_value(Exp[0], env)}`;
  } else if (Kind == "String") {
    return '"' + String + '"';
  } else if (String !== undefined) {
    return String;
  }
  badFormat("value", val);
}

function str_thiscall_Exp(exp) {
  return exp.Kind == 'Drf' ? str_value(exp.Exp[0]) + "->" : str_value(exp) + ".";
}

function stripcsu(s) {
    return s.replace("class ", "").replace("struct ", "").replace("union ");
}

function str_call(prefix, edge, env) {
  const {Exp, Type, PEdgeCallArguments, PEdgeCallInstance} = edge;
  const {Kind, Type:cType, TypeFunctionArguments, TypeFunctionCSU} = Type;

  if (Kind == 'Function') {
    const params = PEdgeCallArguments ? PEdgeCallArguments.Exp : [];
    const strParams = params.map(str_value);

    let func;
    let comment = "";
    let assign_exp;
    if (PEdgeCallInstance) {
      const csu = TypeFunctionCSU.Type.Name;
      const method = str_value(Exp[0], env);

      // Heuristic to only display the csu for constructors
      if (csu.includes(method)) {
        func = stripcsu(csu) + "::" + method;
      } else {
        func = method;
        comment = "# " + csu + "::" + method + "\n";
      }

      const {Exp: thisExp} = PEdgeCallInstance;
      func = str_thiscall_Exp(thisExp) + func;
    } else {
      func = str_value(Exp[0]);
    }
    assign_exp = Exp[1];

    let assign = "";
    if (assign_exp) {
      assign = str_value(assign_exp) + " := ";
    }
    return `${comment}${prefix} Call ${assign}${func}(${strParams.join(", ")})`;
  }

  print(JSON.stringify(edge, null, 4));
  throw "unhandled format error";
}

function str_assign(prefix, edge) {
  const {Exp} = edge;
  const [lhs, rhs] = Exp;
  return `${prefix} Assign ${str_value(lhs)} := ${str_value(rhs)}`;
}

function str_loop(prefix, edge) {
  const {BlockId: {Loop}} = edge;
  return `${prefix} Loop ${Loop}`;
}

function str_assume(prefix, edge) {
  const {Exp, PEdgeAssumeNonZero} = edge;
  const cmp = PEdgeAssumeNonZero ? "" : "!";

  const {Exp: aExp, Kind, OpCode} = Exp[0];
  if (Kind == 'Binop') {
    const [lhs, rhs] = aExp;
    const op = opcode_name(OpCode, !PEdgeAssumeNonZero);
    return `${prefix} Assume ${str_value(lhs)} ${op} ${str_value(rhs)}`;
  } else if (Kind == 'Unop') {
    return `${prefix} Assume ${cmp}${OpCode} ${str_value(aExp[0])}`;
  } else if (Kind == 'NullTest') {
    return `${prefix} Assume nullptr ${cmp}== ${str_value(aExp[0])}`;
  } else if (Kind == 'Drf') {
    return `${prefix} Assume ${cmp}${str_value(Exp[0])}`;
  }

  print(JSON.stringify(edge, null, 4));
  throw "unhandled format error";
}

function str_edge(edge, env) {
  const {Index, Kind} = edge;
  const [src, dst] = Index;
  const prefix = `[${src},${dst}]`;

  if (Kind == "Call")
    return str_call(prefix, edge, env);
  if (Kind == 'Assign')
    return str_assign(prefix, edge);
  if (Kind == 'Assume')
    return str_assume(prefix, edge);
  if (Kind == 'Loop')
    return str_loop(prefix, edge);

  print(JSON.stringify(edge, null, 4));
  throw "unhandled edge type";
}

function str(unknown) {
  if ("Name" in unknown) {
    return str_Variable(unknown);
  } else if ("Index" in unknown) {
    // Note: Variable also has .Index, with a different meaning.
    return str_edge(unknown);
  } else if ("Kind" in unknown) {
    if ("BlockId" in unknown)
      return str_Variable(unknown);
    return str_value(unknown);
  } else if ("Type" in unknown) {
    return str_Type(unknown);
  }
  return "unknown";
}

function jdump(x) {
  print(JSON.stringify(x, null, 4));
  quit(0);
}
