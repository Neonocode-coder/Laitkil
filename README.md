# ⚡ Laitkil

**Статически типизированный, быстрый и удобный интерпретируемый язык программирования для 2D-геймдева и встраивания в C/C++ проекты.**

Laitkil создан как современная и безопасная альтернатива Lua для новых игровых движков и приложений. Он сохраняет простоту и лёгкость встраивания (всего один файл реализации + заголовки C/C++), но избавляет разработчиков от классических проблем динамической типизации: опечаток в именах полей, скрытых `nil`-ошибок в рантайме, путаницы с индексацией с `1` и отсутствия нативного ООП.

---

## 🎯 Почему Laitkil, а не Lua?

| Возможность | Lua | Laitkil |
| :--- | :--- | :--- |
| **Типизация** | Динамическая (ошибки ловятся только при выполнении ветки кода) | **Статическая** с проверкой на этапе компиляции в байткод |
| **Индексация массивов** | С `1` (постоянный источник багов при работе с C/C++) | **С `0`**, как в C, C++, C# и Rust |
| **ООП и структуры данных** | Эмуляция через таблицы и метатаблицы | Нативные **`blueprint`** с конструкторами `init` и модификаторами `access` / `unaccess` |
| **Конечные автоматы (FSM)** | Строки или числа без проверки | Встроенный тип **`states`** с проверкой вариантов на этапе компиляции |
| **Управление памятью** | Сборщик мусора (возможны фризы GC в игровом цикле) | **Детерминированный подсчёт ссылок (RC)** + пул памяти без фризов |
| **Интеграция с C++17** | Ручная работа со стеком через C API | Автоматический биндинг лямбд и функций в одну строку через `LKLBridge.hpp` |
| **Инструменты для 2D-игр** | Базовая математика | Встроенные `get_delta()`, детерминированный рандом по сиду (`set_seed`), работа с JSON и ФС |

---

## ✨ Ключевые особенности языка

* **Высокая производительность VM:** Регистровая виртуальная машина на C++17 с вычисляемыми переходами (*computed goto*), peephole-оптимизатором, супер-инструкциями (`LT_JMPF`, `LOADK_ADD`, `ADD_RK` и др.) и собственным аллокатором с пулом объектов.
* **Нулевые паузы GC:** Память управляется подсчётом ссылок — идеально для стабильных 60/120/240 FPS в 2D-играх.
* **Горячая перезагрузка (Hot Reload):** Функции верхнего уровня можно переопределять на лету загрузкой нового чанка без потери глобального состояния игры.
* **Безопасная модульность:** Директива `paste "file.lkl"` на уровне лексера с защитой от циклических зависимостей.
* **Чистый и выразительный синтаксис:** Никаких точек с запятой, интуитивные циклы `from`, `iter_a`, `iter_d` и паттерн-матчинг через `if ... equals`.

---

## 🚀 Быстрый старт и синтаксис

### 1. Переменные и базовые типы
В Laitkil все переменные строго типизированы. Поддерживаются `num` (64-битный float), `str`, `bool`, массивы `a_<тип>`, словари `d_<тип>` и `json_str`.

```laitkil
num speed = 250.5
str player_name = "Hero"
bool is_alive = true

a_num scores = [100, 250, 500]
d_num inventory = ["potions"=3, "arrows"=25]

## Доступ по индексу и ключу осуществляется через двоеточие ':' ##
print("Первый счёт:", scores:0)
print("Зелий в сумке:", inventory:"potions")

# Добавление и удаление элементов
append scores 1000
prepend scores 50
add scores:1 175
remove_a scores:0
```

### 2. Конечные автоматы (`states`) и `equals`
Для 2D-игр критически важны состояния персонажей, UI и игровых сцен. В Laitkil они встроены в синтаксис языка:

```laitkil
states PlayerState { Idle, Run, Jump, Attack }

PlayerState state = Idle

if state equals {
    Idle {
        print("Персонаж стоит на месте")
    }
    Run or Jump {
        print("Персонаж в движении")
    }
    Attack {
        print("Атака!")
    }
    else {
        print("Неизвестное состояние")
    }
}
```

### 3. Классы (`blueprint`) и инкапсуляция
Вместо метатаблиц используются чертежи (`blueprint`) с явным разделением на публичные (`access`) и приватные (`unaccess`) поля и методы:

```laitkil
blueprint Entity {
    access {
        num x
        num y
        str tag
    }
    unaccess {
        num hp
    }

    init(str tag, num start_x, num start_y, num max_hp) {
        self.tag = tag
        self.x = start_x
        self.y = start_y
        self.hp = max_hp
    }

    access {
        func move(num dx, num dy) {
            self.x += dx
            self.y += dy
        }

        func take_damage(num dmg) -> bool {
            self.hp -= dmg
            give self.hp <= 0
        }
    }
}

new Entity enemy("Goblin", 100, 200, 50)
enemy.move(15, -5)
if enemy.take_damage(60) {
    print(enemy.tag ++ " повержен!")
}
```

### 4. Циклы и функции
```laitkil
func clamp(num val, num min_v, num max_v) -> num {
    if val < min_v { give min_v }
    elif val > max_v { give max_v }
    give val
}

# Цикл со счётчиком: from (переменная = старт) to (конец, шаг)
from (i = 0) to (10, 2) {
    print("Шаг:", i)
}

# Итерация по массиву и словарю
a_str items = ["Меч", "Щит", "Лук"]
iter_a (item, items) {
    print("Предмет:", item)
}

d_num stats = ["STR"=12, "AGI"=18]
iter_d (key, val, stats) {
    print(key ++ ": " ++ to_str(val))
}
```

---

## 🎮 Встроенная библиотека для 2D-геймдева

Laitkil поставляется с богатой стандартной библиотекой, готовой к работе «из коробки»:

* **Игровой цикл и время:**
  * `get_delta()` — возвращает `deltaTime` в секундах с прошлого кадра.
  * `clock()`, `get_unix_time()` — точные таймеры.
* **Математика и RNG:**
  * `abs`, `floor`, `ceil`, `round`, `sqrt`, `sin`, `cos`, `tan`, `atan`, `exp`, `log`, `pow`, `min`, `max`, `pi()`.
  * Обычный рандом: `random()`, `random_int(min, max)`, `random_choice(arr)`.
  * **Детерминированный рандом** (для процедурной генерации уровней и реплеев): `set_seed(s)`, `random_seed()`, `random_int_seed(min, max)`, `random_choice_seed(arr)`.
* **Строки (с полной поддержкой UTF-8):**
  * `len`, `upper`, `lower`, `trim`, `substr`, `find`, `replace`, `starts_with`, `ends_with`, `contains`, `split`, `join`, `repeat`, `symbol`, `code_of`.
* **Коллекции:**
  * `has`, `keys`, `values`, `copy`, `reverse`, `sort`, `slice`, `index_of`.
* **Файловая система и пути (для сохранений и конфигов):**
  * `read_file`, `write_file`, `append_file`, `file_exists`, `remove_file`.
  * `dir_exists`, `make_dir`, `remove_dir`, `list_dir`, `current_dir`.
  * `join_path`, `file_name`, `file_ext`, `parent_path`.
* **Типизированный парсинг структур (`json_str`):**
  * `parse_json(raw)`, `ex_dict(j)`, `ex_array(j)`, `ex_str(j)`.

---

## 🔌 Встраивание в C++ и C (`LKLBridge`)

Архитектура интеграции вдохновлена Lua C API, но избавлена от бойлерплейта. Вся виртуальная машина компилируется из одного файла (`Laitkil_Interpreter.cpp`).

### Вариант 1: Современный C++17 API (`LKLBridge.hpp`)
Автоматический вывод сигнатур из лямбд и функций C++, конвертация `std::vector` и `std::map` в массивы и словари Laitkil:

```cpp
#include "LKLBridge.hpp"
#include <iostream>

int main() {
    try {
        lkl::State lk;

        // Регистрируем функции движка в одну строку
        lk.bind("draw_sprite", [](const std::string& tex, double x, double y) {
            std::cout << "Draw " << tex << " at (" << x << ", " << y << ")\n";
        });

        lk.bind("check_collision", [](double x1, double y1, double x2, double y2) -> bool {
            return std::abs(x1 - x2) < 32.0 && std::abs(y1 - y2) < 32.0;
        });

        // Загружаем игровой скрипт
        lk.doString(R"(
            func update(num dt) {
                num x = 100 + 50 * dt
                draw_sprite("player.png", x, 200)
            }
        )");

        // Вызываем функцию скрипта из игрового цикла C++
        lk.call("update", 0.016);

    } catch (const lkl::Error& e) {
        std::cerr << "Laitkil Error: " << e.what() << "\n";
    }
    return 0;
}
```

### Вариант 2: Чистый C API (`LKLBridge.h`)
Для интеграции с C, Rust, Zig или собственными биндингами:

```c
#include "LKLBridge.h"
#include <stdio.h>

static int host_add(lkl_State* L) {
    double a = lkl_tonum(L, 1);
    double b = lkl_tonum(L, 2);
    lkl_pushnum(L, a + b);
    return 1;
}

int main(void) {
    lkl_State* L = lkl_newstate();
    lkl_register(L, "add", host_add, "num num -> num");
    
    if (lkl_dostring(L, "print(add(10, 32))") != LKL_OK) {
        fprintf(stderr, "%s\n", lkl_geterror(L));
    }
    
    lkl_close(L);
    return 0;
}
```

---

## 🛠️ Сборка и запуск

### Сборка CLI-интерпретатора
Требуется любой компилятор с поддержкой **C++17** (GCC, Clang, MSVC):

```bash
# Linux / macOS
g++ -O3 -std=c++17 Laitkil_Interpreter.cpp -o laitkil

# Windows (MinGW / Clang)
g++ -O3 -std=c++17 Laitkil_Interpreter.cpp -o laitkil.exe
```

### Запуск скриптов и дизассемблер байткода
```bash
# Выполнение скрипта
./laitkil game.lkl

# Просмотр скомпилированного регистрового байткода
./laitkil --dis game.lkl
```

### Встраивание в ваш проект
Чтобы использовать Laitkil как библиотеку без функции `main()`, просто определите макрос `LKL_NO_MAIN` при компиляции `Laitkil_Interpreter.cpp` и подключите `LKLBridge.h` или `LKLBridge.hpp`.

---

## 📄 Лицензия

Этот проект распространяется под лицензией MIT — вы можете свободно использовать Laitkil в коммерческих и открытых играх и движках.