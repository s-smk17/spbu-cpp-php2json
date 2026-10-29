<?php
// Перечисления (1.3) и прочее
enum Suit
{
    case Hearts;
    case Spades;
}

enum Status: string
{
    case Active = 'active';
    case Blocked = 'blocked';
}

$card = Suit::Hearts;
$status = Status::Active;
$same = $card === Suit::Spades;

$half = 7 / 2;
$whole = 8 / 2;
$frac = 2 ** -1;
$unknown = $whole / $half;
$n = 5;
$maybe = $n / 3;

$a = 1;
$b = &$a;
$b = 2;
$c = $a <=> $b;
$name = $undefined ?? "guest";
$name ??= "x";

echo $a, PHP_EOL;
