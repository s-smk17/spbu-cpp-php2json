# -*- coding: utf-8 -*-
"""Проверка соответствия JSON схеме из методички (concepts.pdf)."""
import json, sys
from collections import Counter

BIN = ["result-type", "left", "right"]
UN = ["result-type", "argument"]
ASG = ["assignment-type", "variable", "expression"]
SHIFT = ["result-type", "amount", "argument"]
SCHEMA = {
    "literal-constant": ["constant-type", "constant-value"],
    **{t: BIN for t in ["sum", "product", "difference", "quotient", "integer-quotient", "modulo", "power",
                        "equals", "less", "less-or-equal", "greater", "greater-or-equal", "not-equal",
                        "logical-and", "logical-or", "bitwise-and", "bitwise-or", "bitwise-xor",
                        "sequence", "string-concatenation",
                        "assignment-bitwise-and", "assignment-bitwise-or", "assignment-bitwise-xor"]},
    **{t: UN for t in ["prefix-increment", "postfix-increment", "prefix-decrement", "postfix-decrement",
                       "logical-not", "bitwise-not", "dereference", "address", "parentheses", "type-cast"]},
    **{t: SHIFT for t in ["arithmetic-bitwise-shift-right", "logic-bitwise-shift-right", "bitwise-shift-left",
                          "assignment-arithmetic-bitwise-shift-right", "assignment-logic-bitwise-shift-right",
                          "assignment-bitwise-shift-left"]},
    **{t: ASG for t in ["assignment", "assignment-sum", "assignment-product", "assignment-difference",
                        "assignment-quotient", "assignment-integer-quotient", "assignment-modulo",
                        "assignment-power"]},
    "dynamic-memory-allocation": ["array-flag", "address-hint", "variable-type", "constructor-name", "constructor-arguments"],
    "conditional-expression": ["result-type", "condition", "if-true", "if-false"],
    "function-call": ["function-name", "result-type", "arguments"],
    "index": ["array-expression", "result-type", "indices"],
    "builtin-type": ["type-value"],
    "enumeration-type": ["name", "values"],
    "struct-type": ["name", "base-structs", "fields", "methods", "friends"],
    "variables-declaration": ["global", "static", "const", "declarators"],
    "declarator": ["variable-type", "variable-name", "reference", "constructor-name", "constructor-arguments"],
    "variable-reference": ["variable-name", "variable-type", "variable-usage"],
    "subroutine-declaration": ["subroutine-name", "result-type", "arguments-declarators", "constant-section",
                               "type-section", "variables-section", "subroutines-section", "subroutine-body"],
    "return-statement": ["expression", "the-last-statement"],
    "expression-statement": ["expression"],
    "compound-statement": ["statements"],
    "conditional-statement": ["condition", "if-true", "if-false"],
    "choose-statement": ["pass-control", "expression-to-analyze", "variants"],
    "choose-variant": ["condition-label", "variant-body"],
    "loop-statement": ["begin-condition", "end-condition", "loop-index", "loop-initialization",
                       "loop-pass-to-next-iteration", "loop-body"],
    "continue-statement": ["label"], "break-statement": ["label"],
    "goto-statement": ["target-label"], "label": ["label-name", "statement"],
}
problems, used, ext_types, ext_keys = [], Counter(), Counter(), Counter()

def walk(n, path):
    if isinstance(n, list):
        for i, x in enumerate(n): walk(x, f"{path}[{i}]")
        return
    if not isinstance(n, dict): return
    t = n.get("type")
    if t in SCHEMA:
        used[t] += 1
        req = SCHEMA[t]
        missing = [k for k in req if k not in n]
        if missing: problems.append(f"{path}: {t} missing {missing}")
        for k in n:
            if k != "type" and k not in req: ext_keys[f"{t}.{k}"] += 1
    else:
        ext_types[t] += 1
    if t == "variable-reference" and n["variable-usage"] not in ("lvalue", "rvalue"):
        problems.append(f"{path}: bad variable-usage")
    if t == "return-statement" and not isinstance(n["the-last-statement"], bool):
        problems.append(f"{path}: the-last-statement must be bool")
    for k, v in n.items(): walk(v, f"{path}.{k}")

for f in sys.argv[1:]:
    walk(json.load(open(f)), f.split("/")[-1])
print("PROBLEMS:", problems or "none")
print("schema node types used: %d of %d" % (len(used), len(SCHEMA)))
print("unused schema types:", sorted(set(SCHEMA) - set(used)))
print("extension node types:", dict(ext_types))
print("extension keys:", dict(ext_keys))
