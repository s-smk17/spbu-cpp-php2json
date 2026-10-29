# php2json — транслятор PHP → JSON

Принимает программу на PHP и выдаёт её описание в JSON по схеме из `concepts.pdf`
(константы, операции, типы, переменные, подпрограммы, операторы).

## Сборка и запуск

```bash
cmake -S . -B build && cmake --build build      # или: g++ -std=c++17 -O2 src/*.cpp -o php2json
./build/php2json tests/03_functions.php          # JSON в stdout
./build/php2json prog.php -o prog.json           # JSON в файл
./build/php2json --short-types prog.php          # типы строками: "integer" вместо {"type":"builtin-type",...}
cat prog.php | ./build/php2json                  # чтение из stdin
./run_tests.sh                                   # собрать, прогнать tests/, проверить схему и типы
```

Требуется компилятор C++17 (g++ ≥ 8, clang ≥ 7, MSVC 2019). Внешних библиотек нет.
При ошибке программа печатает `файл: line N:M: сообщение` и завершается с кодом 1.

## Структура

| Файл | Что делает |
|---|---|
| `src/json.hpp` | дерево JSON и его печать (своя мини-библиотека) |
| `src/lexer.*` | лексический анализ: `<?php ?>`, комментарии, числа, строки, heredoc, операторы |
| `src/parser.*` | рекурсивный спуск + построение JSON + таблица символов + вывод типов |
| `src/main.cpp` | командная строка |
| `tests/*.php` | тестовые программы (все проходят `php -l`) |
| `examples/*.json` | результат трансляции тестов |
| `tools/check_schema.py` | проверяет, что каждый узел содержит поля из схемы, и выводит список расширений |
| `tools/check_types.py` | исполняет тесты настоящим PHP и сверяет реальные типы переменных с выведенными |
| `tools/json2text.py` | печатает JSON как псевдокод — удобно показывать, что структура верна |

## Пример

```php
<?php
$a = 2;
echo $a + 1;
```

```jsonc
{ "type": "subroutine-declaration", "subroutine-name": "main", "result-type": "void",
  "variables-section": [ { "type": "variables-declaration", "global": true, ...,
      "declarators": [ { "type": "declarator", "variable-type": "integer", "variable-name": "$a", ... } ] } ],
  "subroutine-body": { "type": "compound-statement", "statements": [
      { "type": "expression-statement", "expression": { "type": "assignment", "assignment-type": "integer",
          "variable": { "type": "variable-reference", "variable-name": "$a", "variable-usage": "lvalue", ... },
          "expression": { "type": "literal-constant", "constant-type": "integer", "constant-value": 2 } } },
      { "type": "expression-statement", "expression": { "type": "function-call", "function-name": "echo",
          "arguments": [ { "type": "sum", "result-type": "integer", "left": ..., "right": ... } ] } } ] } }
```

## Что поддерживается

Литералы всех видов, переменные, константы (`const`, `define`), все арифметические, логические, побитовые
операции, сравнения, присваивания и совмещённые присваивания, `++/--`, `?:`, `??`, приведения типов,
строки с подстановками (`"$a"`, `"{$a['k']}"`, `"${a}"`), heredoc/nowdoc, массивы, индексация,
`if/elseif/else`, `while`, `do-while`, `for`, `foreach`, `switch`, `break N`, `continue N`, `goto`/метки,
альтернативный синтаксис (`if: … endif;`), HTML вне тегов, функции (типы параметров, значения по умолчанию,
`&`-параметры, `...$rest`, `static`, `global`), классы и интерфейсы (наследование, свойства, константы,
методы, `static`, `self/parent`, продвижение свойств в конструкторе), `new`, `->`, `?->`, `::`, `instanceof`, `enum`.

Не поддерживаются (выдаётся понятная ошибка): анонимные функции и `fn`, `match`, `try/catch/throw`,
трейты, `list()`/деструктуризация, `yield`.

Транслятор также выполняет проверки, которые PHP делает при компиляции: метки `goto`, уровни `break N`,
`$this`, неассоциативные сравнения, константные выражения, типы значений по умолчанию, `return` в `void`.
Не выполняется только вычисление константных выражений (`int $x = !0` PHP отвергает, транслятор — нет).
