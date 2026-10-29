<?php
// 1.3 Типы: классы -> struct-type
interface Shape
{
    public function area(): float;
}

abstract class Base implements Shape
{
    const UNITS = "cm";
    protected static int $count = 0;
    public string $label = "shape";

    public function __construct()
    {
        self::$count++;
    }

    public static function getCount(): int
    {
        return self::$count;
    }

    abstract public function area(): float;
}

class Circle extends Base
{
    public function __construct(private float $r)
    {
        parent::__construct();
    }

    public function area(): float
    {
        return M_PI * $this->r ** 2;
    }
}

class Rect extends Base
{
    private float $w;
    private float $h;

    public function __construct(float $w, float $h)
    {
        parent::__construct();
        $this->w = $w;
        $this->h = $h;
    }

    public function area(): float
    {
        return $this->w * $this->h;
    }

    public function describe(): string
    {
        return "Rect {$this->w}x{$this->h} " . self::UNITS;
    }
}

$shapes = [new Circle(1.0), new Rect(2, 3)];
$total = 0.0;
foreach ($shapes as $shape) {
    $total += $shape->area();
}
$rect = new Rect(4, 5);
$area = $rect->area();
$text = $rect->describe();
$isShape = $rect instanceof Shape;
$count = Base::getCount();
$units = Rect::UNITS;
$cls = Rect::class;
echo $total, " ", $text, " ", $count, PHP_EOL;
