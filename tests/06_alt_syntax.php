<!DOCTYPE html>
<html>
<body>
<?php $items = ["one", "two", "three"]; ?>
<ul>
<?php foreach ($items as $i => $item): ?>
    <li><?= $i + 1 ?>. <?= $item ?></li>
<?php endforeach; ?>
</ul>
<?php if (count($items) > 2): ?>
    <p>Many items</p>
<?php elseif (count($items) == 0): ?>
    <p>Empty</p>
<?php else: ?>
    <p>Few items</p>
<?php endif; ?>
<?php
$n = 3;
while ($n > 0):
    $n--;
endwhile;
for ($k = 0; $k < 2; $k++):
    echo $k;
endfor;
switch ($n):
    case 0:
        echo "zero";
        break;
endswitch;
?>
</body>
</html>
