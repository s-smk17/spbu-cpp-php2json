<?php
// 1.4 Переменные и 1.5 Подпрограммы
$counter = 0;

function factorial(int $n): int
{
    if ($n <= 1) {
        return 1;
    }
    return $n * factorial($n - 1);
}

function greet($name, $greeting = "Hello")
{
    return "$greeting, $name!";
}

function swap(&$a, &$b): void
{
    $tmp = $a;
    $a = $b;
    $b = $tmp;
}

function sumAll(...$nums)
{
    $s = 0;
    foreach ($nums as $x) {
        $s += $x;
    }
    return $s;
}

function nextId(): int
{
    static $id = 0;
    return ++$id;
}

function incrementCounter(): void
{
    global $counter;
    $counter++;
}

function average(float $a, float $b): float
{
    return ($a + $b) / 2;
}

$f = factorial(5);
$g = greet("World");
$p = 1;
$q = 2;
swap($p, $q);
$total = sumAll(1, 2, 3);
incrementCounter();
$id = nextId();
$avg = average(1.5, 2.5);
$hoisted = later(3);

function later($x)
{
    return $x * 2;
}

echo $f, " ", $g, " ", $total, PHP_EOL;
