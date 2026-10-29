"""Обратный просмотр: печатает JSON-описание как читаемый псевдокод (для проверки и защиты).
   python3 tools/json2text.py examples/03_functions.json"""
import json, sys

BIN = {"sum": "+", "difference": "-", "product": "*", "quotient": "/", "integer-quotient": "intdiv",
       "modulo": "%", "power": "**", "equals": "==", "less": "<", "less-or-equal": "<=", "greater": ">",
       "greater-or-equal": ">=", "not-equal": "!=", "logical-and": "&&", "logical-or": "||", "logical-xor": "xor",
       "bitwise-and": "&", "bitwise-or": "|", "bitwise-xor": "^", "string-concatenation": ".",
       "three-way-comparison": "<=>", "null-coalescing": "??", "sequence": ",", "instanceof": "instanceof"}
ASG = {"assignment": "=", "assignment-sum": "+=", "assignment-difference": "-=", "assignment-product": "*=",
       "assignment-quotient": "/=", "assignment-modulo": "%=", "assignment-power": "**=",
       "assignment-string-concatenation": ".=", "assignment-null-coalescing": "??=",
       "assignment-bitwise-and": "&=", "assignment-bitwise-or": "|=", "assignment-bitwise-xor": "^=",
       "assignment-bitwise-shift-left": "<<=", "assignment-arithmetic-bitwise-shift-right": ">>="}

def T(t):
    if t is None: return "?"
    if isinstance(t, str): return t
    return t.get("type-value") or t.get("name")

def E(n):
    if n is None: return ""
    t = n["type"]
    if t == "literal-constant": return json.dumps(n["constant-value"], ensure_ascii=False)
    if t == "variable-reference": return n["variable-name"] + ("ˡ" if n["variable-usage"] == "lvalue" else "") + ":" + T(n["variable-type"])
    if t in ("bitwise-shift-left", "arithmetic-bitwise-shift-right"):
        return "(%s %s %s)" % (E(n["argument"]), "<<" if "left" in t else ">>", E(n["amount"]))
    if t == "integer-quotient": return "intdiv(%s, %s)" % (E(n["left"]), E(n["right"]))
    if t in BIN:
        rt = n.get("result-type")
        return "(%s %s %s)<%s>" % (E(n["left"]), BIN[t], E(n["right"]) if isinstance(n["right"], dict) and "type" in n["right"] and n["right"]["type"] not in ("builtin-type","named-type") else T(n["right"]) if t=="instanceof" else E(n["right"]), T(rt))
    if t in ASG:
        if "variable" in n: return "%s %s %s" % (E(n["variable"]), ASG[t], E(n["expression"]))
        if "left" in n: return "%s %s %s" % (E(n["left"]), ASG[t], E(n["right"]))
        return "%s %s %s" % (E(n["argument"]), ASG[t], E(n["amount"]))
    if t in ("prefix-increment", "prefix-decrement"): return ("++" if "inc" in t else "--") + E(n["argument"])
    if t in ("postfix-increment", "postfix-decrement"): return E(n["argument"]) + ("++" if "inc" in t else "--")
    if t == "logical-not": return "!" + E(n["argument"])
    if t == "bitwise-not": return "~" + E(n["argument"])
    if t == "unary-minus": return "-" + E(n["argument"])
    if t == "parentheses": return "(" + E(n["argument"]) + ")"
    if t == "type-cast": return "(%s)%s" % (T(n["result-type"]), E(n["argument"]))
    if t == "conditional-expression": return "(%s ? %s : %s)<%s>" % (E(n["condition"]), E(n["if-true"]), E(n["if-false"]), T(n["result-type"]))
    if t == "function-call":
        pre = ""
        if "object" in n: pre = E(n["object"]) + "->"
        if "class-name" in n: pre = n["class-name"] + "::"
        if "class-expression" in n: pre = E(n["class-expression"]) + "::"
        name = n["function-name"] if n["function-name"] is not None else E(n["function-expression"])
        return "%s%s(%s)<%s>" % (pre, name, ", ".join(E(a) for a in n["arguments"]), T(n["result-type"]))
    if t == "index": return "%s[%s]" % (E(n["array-expression"]), "][".join(E(i) if i else "" for i in n["indices"]))
    if t == "member-access":
        if n.get("static"): return "%s::%s<%s>" % (n["class-name"] if "class-name" in n else E(n["class-expression"]), n["member-name"], T(n["result-type"]))
        return "%s->%s<%s>" % (E(n["object"]), n["member-name"], T(n["result-type"]))
    if t == "array-literal":
        return "[" + ", ".join((E(e["key"]) + " => " if e["key"] else "") + E(e["value"]) for e in n["elements"]) + "]"
    if t == "dynamic-memory-allocation":
        return "new %s(%s)" % (T(n["variable-type"]), ", ".join(E(a) for a in n["constructor-arguments"]))
    if t == "named-argument": return n["name"] + ": " + E(n["value"])
    if t == "spread": return "..." + E(n["argument"])
    if t == "error-suppression": return "@" + E(n["argument"])
    return "<<%s>>" % t

def D(d):  # variables-declaration
    flags = [k for k in ("global", "static", "const") if d[k]]
    parts = []
    for x in d["declarators"]:
        s = ("&" if x["reference"] else "") + x["variable-name"] + ":" + T(x["variable-type"])
        if x["constructor-arguments"]: s += " = " + ", ".join(E(a) for a in x["constructor-arguments"])
        parts.append(s)
    vis = d.get("visibility", "")
    return "%s[%s] %s" % ((vis + " ") if vis else "", " ".join(flags), ", ".join(parts))

def S(n, ind):
    p = "  " * ind
    if n is None: return [p + "<null>"]
    t = n["type"]
    if t == "compound-statement":
        out = [p + "{"]
        for s in n["statements"]: out += S(s, ind + 1)
        return out + [p + "}"]
    if t == "expression-statement": return [p + E(n["expression"]) + ";"]
    if t == "conditional-statement":
        out = [p + "if " + E(n["condition"])] + S(n["if-true"], ind + 1)
        if n["if-false"] is not None: out += [p + "else"] + S(n["if-false"], ind + 1)
        return out
    if t == "loop-statement":
        hdr = "loop begin=%s end=%s index=%s init=%s step=%s" % (E(n["begin-condition"]), E(n["end-condition"]), E(n["loop-index"]), E(n["loop-initialization"]), E(n["loop-pass-to-next-iteration"]))
        if "foreach-collection" in n: hdr += " foreach(%s as %s)" % (E(n["foreach-collection"]), E(n["foreach-key"]))
        return [p + hdr] + S(n["loop-body"], ind + 1)
    if t == "choose-statement":
        out = [p + "switch %s pass=%s" % (E(n["expression-to-analyze"]), n["pass-control"])]
        for v in n["variants"]:
            lbl = v["condition-label"]
            out += [p + "  case " + (lbl if isinstance(lbl, str) else E(lbl)) + ":"] + S(v["variant-body"], ind + 2)
        return out
    if t == "break-statement": return [p + "break %s;" % n["label"]]
    if t == "continue-statement": return [p + "continue %s;" % n["label"]]
    if t == "return-statement": return [p + "return %s; last=%s" % (E(n["expression"]), n["the-last-statement"])]
    if t == "goto-statement": return [p + "goto " + n["target-label"] + ";"]
    if t == "label": return [p + n["label-name"] + ":"] + S(n["statement"], ind)
    return [p + "<<%s>>" % t]

def SUB(n, ind):
    p = "  " * ind
    out = [p + "SUBROUTINE %s(%s): %s %s" % (n["subroutine-name"], ", ".join(D(a) for a in n["arguments-declarators"]), T(n["result-type"]),
                                              " ".join(k + "=" + str(n[k]) for k in ("visibility", "static", "abstract") if k in n))]
    for c in n["constant-section"]: out.append(p + "  CONST " + D(c))
    for ty in n["type-section"]:
        if ty["type"] == "enumeration-type":
            out.append(p + "  ENUM %s {%s}" % (ty["name"], ", ".join("%s=%s" % (v["name"], E(v)) for v in ty["values"]))); continue
        out.append(p + "  TYPE %s %s : %s" % (ty["kind"], ty["name"], ", ".join(T(b) for b in ty["base-structs"])))
        for f in ty["fields"]: out.append(p + "    FIELD " + D(f))
        for m in ty["methods"]: out += SUB(m, ind + 2)
    for v in n["variables-section"]: out.append(p + "  VARS " + D(v))
    for s in n["subroutines-section"]: out += SUB(s, ind + 1)
    if n["subroutine-body"] is not None: out += S(n["subroutine-body"], ind + 1)
    return out

print("\n".join(SUB(json.load(open(sys.argv[1])), 0)))
