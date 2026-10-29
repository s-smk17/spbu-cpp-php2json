<?php
// Строки: экранирование, подстановки, heredoc / nowdoc
$name = "Сергей";
$items = ['apple' => 3, 'pear' => 5];
$list = [10, 20, 30];

$plain = "Just text\twith tab\n";
$simple = "Hello, $name!";
$arrayKey = "Apples: $items[apple]";
$arrayIdx = "Second: $list[1]";
$complex = "Pears: {$items['pear']}, total: {$list[0]}";
$escaped = "Price: \$5, quote: \", unicode: \u{263A}, hex: \x41";

$heredoc = <<<EOT
    Name: $name
    Count: {$list[2]}
    EOT;

$nowdoc = <<<'EOT'
Raw $name, no {$interpolation}
EOT;

$joined = implode(", ", $list) . "!";
echo $simple, PHP_EOL, $heredoc, PHP_EOL;
