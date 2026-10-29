<?php
// 1.6 Операторы
$n = 10;

if ($n > 5) {
    echo "greater\n";
} elseif ($n == 5) {
    echo "equal\n";
} else {
    echo "less\n";
}

if ($n % 2 == 0) echo "even\n";

// while — предусловие
$k = 0;
while ($k < 3) {
    $k++;
}

// do-while — постусловие
do {
    $k--;
} while ($k > 0);

// for
$total = 0;
for ($i = 1; $i <= $n; $i++) {
    if ($i == 3) {
        continue;
    }
    if ($i > 8) {
        break;
    }
    $total += $i;
}

// вложенные циклы и break 2
for ($i = 0; $i < 5; $i++) {
    for ($j = 0; $j < 5; $j++) {
        if ($i * $j > 6) {
            break 2;
        }
    }
}

// foreach
$arr = [1, 2, 3];
$assoc = ['a' => 1, 'b' => 2];
foreach ($arr as $v) {
    echo $v;
}
foreach ($assoc as $key => $value) {
    echo "$key = $value\n";
}

// switch
switch ($n) {
    case 1:
    case 2:
        echo "small";
        break;
    case 10:
        echo "ten";
        break;
    default:
        echo "other";
}

// goto и метка
$attempt = 0;
retry:
$attempt++;
if ($attempt < 3) {
    goto retry;
}

// индексация
$matrix = [[1, 2], [3, 4]];
$cell = $matrix[1][0];
$arr[] = 4;
$arr[0] = 100;

echo "done", PHP_EOL;
