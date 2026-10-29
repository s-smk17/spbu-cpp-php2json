#!/usr/bin/env bash
# Сборка, прогон всех тестов, проверка результата на соответствие схеме и (если есть php) типов.
set -e
cd "$(dirname "$0")"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build > /dev/null
for f in tests/*.php; do
    ./build/php2json "$f" -o "examples/$(basename "$f" .php).json"
    echo "ok   $f"
done
python3 tools/check_schema.py examples/*.json
if command -v php > /dev/null; then
    for f in tests/*.php; do php -l "$f" > /dev/null; done && echo "php -l: all tests are valid PHP"
    python3 tools/check_types.py tests/*.php
fi
