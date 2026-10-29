"""Сверка выведенных типов глобальных переменных с реальными типами при исполнении в PHP.
   python3 tools/check_types.py tests/*.php      (JSON берётся из examples/)"""
import json, os, subprocess, sys, tempfile

MAP = {"integer": "integer", "double": "real", "string": "string", "boolean": "boolean", "array": "array", "NULL": "null"}
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
bad = checked = 0
for php in sys.argv[1:]:
    js = json.load(open(os.path.join(root, "examples", os.path.splitext(os.path.basename(php))[0] + ".json")))
    declared = {}
    for decl in js["variables-section"]:
        for d in decl["declarators"]:
            t = d["variable-type"]
            declared[d["variable-name"][1:]] = t["type-value"] if t["type"] == "builtin-type" else t["name"]
    src = open(php).read().rstrip()
    if src.endswith("?>"): src += "<?php "
    probe = src + "\n;foreach (get_defined_vars() as $__k => $__v) { if ($__k[0] != '_' && $__k != 'argv' && $__k != 'argc') echo $__k, ' ', is_object($__v) ? get_class($__v) : gettype($__v), \"\\n\"; }\n"
    with tempfile.NamedTemporaryFile("w", suffix=".php", delete=False) as f:
        f.write(probe)
    out = subprocess.run(["php", f.name], capture_output=True, text=True).stdout.splitlines()
    os.unlink(f.name)
    for line in out:
        parts = line.split(" ")
        if len(parts) != 2 or parts[0] not in declared: continue
        name, rt = parts[0], MAP.get(parts[1], parts[1])
        dt = declared[name]
        if dt == "mixed": continue
        checked += 1
        if not (rt == dt or (dt == "integer|real" and rt in ("integer", "real"))):
            bad += 1
            print(f"  MISMATCH {php}: ${name} declared {dt}, runtime {rt}")
print(f"checked {checked} variables, mismatches: {bad}")
sys.exit(1 if bad else 0)
