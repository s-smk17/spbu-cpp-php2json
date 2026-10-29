<?php
// 1.1 Константы и 1.2 Операции
const MAX = 100;
define('GREETING', 'Hello');

$i = 42;
$hex = 0x1F;
$real = 3.14;
$exp = 1.5e3;
$s = 'single \'quoted\'';
$flag = true;
$nothing = null;

// арифметика
$sum = $i + 1;
$diff = $i - $real;
$prod = $i * 2;
$quot = $i / 5;
$iq = intdiv($i, 5);
$mod = $i % 5;
$pow = 2 ** 10;
$neg = -$i;

// инкремент / декремент
$i++;
++$i;
$i--;
--$i;

// сравнение
$eq = $i == 42;
$strictEq = $i === 42;
$lt = $i < MAX;
$le = $i <= MAX;
$gt = $i > 0;
$ge = $i >= 0;
$ne = $i != 0;
$sne = $i !== 0;
$cmp = $i <=> 10;

// логические
$and = $flag && $lt;
$or = $flag || $gt;
$not = !$flag;
$kwAnd = $flag and $lt;

// побитовые
$band = $i & 0xFF;
$bor = $i | 1;
$bxor = $i ^ 3;
$bnot = ~$i;
$shr = $i >> 2;
$shl = $i << 3;

// присваивание и совмещённые
$x = 10;
$x += 5;
$x -= 1;
$x *= 2;
$x /= 4;
$y = 17;
$y %= 5;
$y **= 2;
$y &= 7;
$y |= 8;
$y ^= 1;
$y >>= 1;
$y <<= 2;
$str = GREETING;
$str .= ", world";

// разные
$par = ($i + 1) * 2;
$cast = (int)"123";
$tern = $i > 10 ? "big" : "small";
$short = $nothing ?: "default";
$coalesce = $nothing ?? "fallback";
$concat = "a" . "b" . $i;
$len = strlen($str);

for ($a = 0, $b = 10; $a < $b; $a++, $b--) {
}

echo $sum, PHP_EOL;
