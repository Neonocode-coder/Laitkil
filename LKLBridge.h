/* =============================================================================
 *  LKLBridge - встраивание Laitkil в C/C++ приложения (в стиле Lua C API)
 *
 *  Идея та же, что у Lua: между хостом и скриптом лежит СТЕК значений.
 *    - C-функция, вызванная из скрипта, читает аргументы со стека (индексы 1..n)
 *      и кладёт результат на вершину стека;
 *    - C-код вызывает функции скрипта так: положил аргументы -> lkl_call().
 *
 *  Язык статически типизирован, поэтому у каждого значения на стеке есть
 *  СТАТИЧЕСКИЙ ТИП ("num", "str", "bool", "a_num", "d_str", имя blueprint...).
 *  Хост-функции регистрируются вместе с сигнатурой, и скрипт проверяет вызовы
 *  ещё до запуска - как для обычных функций языка.
 *
 *  Сигнатура: "<типы параметров через пробел> -> <тип результата>"
 *     "num num -> num"      два числа, возвращает число
 *     "str ->"              строка, ничего не возвращает (-> void можно не писать)
 *     "-> num"              без параметров, возвращает число
 *     "str ... ->"          строка и сколько угодно num/str/bool (как print)
 *     "a_num -> d_str"      массив чисел -> словарь строк
 *
 *  Коды возврата функций, которые могут завершиться ошибкой: LKL_OK (0) или
 *  ненулевой код LKL_ERR_*. Текст ошибки - lkl_geterror().

 *  Функции с «особым» возвращаемым значением (не int-статус):
 *    lkl_arraylen / lkl_dictlen   >= 0 - длина; -1 - значение НЕ массив/словарь
 *                                 (или неверный индекс). Это не ошибка API:
 *                                 lkl_geterror() не меняется.
 *    lkl_dicthas / lkl_dictentry  1/0 - результат; -1 - ОШИБКА API (неверный
 *                                 индекс, не словарь); текст - lkl_geterror().
 *    lkl_type                     LKL_TNONE (-1) - неверный индекс;
 *                                 LKL_TNIL (0) - пустая ссылка на объект.
 *    lkl_tonum / lkl_tobool / lkl_tostring при несовпадении типа возвращают
 *                                 0 / 0 / NULL; проверяйте lkl_type заранее.
 *  Операции, СНИМАЮЩИЕ вершину стека (lkl_arrayset, lkl_arrayappend, lkl_dictset,
 *  lkl_setfield, lkl_setglobal), снимают её только при успехе: после ошибки
 *  (например, несовпадение типа) значение остаётся на стеке.
 *
 *  Потоки: одно lkl_State нельзя использовать из двух потоков одновременно.
 *  Минимальный пример:
 *
 *      static int c_add(lkl_State* L) {
 *          lkl_pushnum(L, lkl_tonum(L, 1) + lkl_tonum(L, 2));
 *          return 1;                              // 1 результат на стеке
 *      }
 *      lkl_State* L = lkl_newstate();
 *      lkl_register(L, "add", c_add, "num num -> num");
 *      lkl_dostring(L, "print(add(2, 3))");       // 5
 *      lkl_close(L);
 * ========================================================================== */
#ifndef LKLBRIDGE_H
#define LKLBRIDGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LKL_API
#define LKL_API
#endif

#define LKL_VERSION "1.0"

typedef struct lkl_State lkl_State;

/* C-функция для скрипта. Читает аргументы со стека (1..lkl_gettop), кладёт результат
   и возвращает: 0 - нет результата (для void), 1 - результат на вершине стека,
   отрицательное число - ошибка (сообщение задаётся через lkl_error). */
typedef int (*lkl_CFunction)(lkl_State* L);
typedef void (*lkl_FreeFn)(void* ud);                                   /* освобождение ud */
typedef void (*lkl_WriteFn)(const char* data, size_t len, void* ud);    /* приёмник print/write */

enum {                       /* коды возврата */
  LKL_OK = 0,
  LKL_ERR_SYNTAX = 1,        /* ошибка синтаксиса */
  LKL_ERR_TYPE = 2,          /* ошибка типов/имён (проверка до запуска) */
  LKL_ERR_RUNTIME = 3,       /* ошибка во время выполнения */
  LKL_ERR_FILE = 4,          /* файл не открылся */
  LKL_ERR_API = 5,           /* неверное использование API (индекс, тип, имя) */
  LKL_EXIT = 6               /* скрипт вызвал exit(код); код - lkl_exitcode() */
};

enum {                       /* lkl_type */
  LKL_TNONE = -1,            /* неверный индекс */
  LKL_TNIL = 0,              /* неинициализированный объект (объявлен без new) */
  LKL_TNUM, LKL_TBOOL, LKL_TSTR, LKL_TARRAY, LKL_TDICT, LKL_TOBJECT
};

/* ------------------------------ память ----------------------------------- */
/* Память значений управляется подсчётом ссылок. Циклические структуры (массив,
   содержащий сам себя; объект, ссылающийся на себя) не освобождаются, в том числе
   при lkl_close - сборщика циклов нет. */

/* ------------------------------ состояние -------------------------------- */
/* Создаёт независимый интерпретатор (свои глобальные переменные и функции). */
LKL_API lkl_State* lkl_newstate(void);
/* То же, с размером стека виртуальной машины в «регистрах» (по умолчанию 262144). */
LKL_API lkl_State* lkl_newstateex(size_t vm_stack_slots);
LKL_API void lkl_close(lkl_State* L);

/* ------------------------------ запуск кода ------------------------------ */
/* Компилирует и выполняет код. Глобальные переменные, функции и blueprint
   сохраняются между вызовами. Функцию с тем же именем и той же сигнатурой можно
   переопределить новым чанком (горячая перезагрузка). Из хост-функции вызывать
   нельзя (LKL_ERR_API) - но lkl_call можно. */
LKL_API int lkl_dostring(lkl_State* L, const char* source);
LKL_API int lkl_dobuffer(lkl_State* L, const char* source, size_t len);
LKL_API int lkl_dofile(lkl_State* L, const char* path);

/* ------------------------------- ошибки ---------------------------------- */
/* Полный текст последней ошибки, например "Runtime error (line 3): division by zero".
   Указатель действителен до следующего вызова API. */
LKL_API const char* lkl_geterror(lkl_State* L);
LKL_API int lkl_errorline(lkl_State* L);         /* строка скрипта или 0 */
LKL_API int lkl_exitcode(lkl_State* L);          /* код из exit(...) после LKL_EXIT */
/* Для хост-функций: задать сообщение об ошибке. Возвращает -1, пишите так:
       return lkl_error(L, "file not found: %s", name);                      */
LKL_API int lkl_error(lkl_State* L, const char* fmt, ...);

/* ------------------------ хост-функции (C -> скрипт) --------------------- */
/* Регистрирует C-функцию под именем name. Вызывать можно до или после загрузки
   скрипта, но ДО чанка, который её вызывает. Повторная регистрация с той же
   сигнатурой заменяет функцию. Имя может перекрыть встроенную (например print). */
LKL_API int lkl_register(lkl_State* L, const char* name, lkl_CFunction fn, const char* sig);
/* То же с произвольными данными (аналог upvalue в Lua); free_ud вызывается при
   замене функции и в lkl_close. Внутри функции данные: lkl_getuserdata(). */
LKL_API int lkl_registerx(lkl_State* L, const char* name, lkl_CFunction fn, const char* sig,
                          void* ud, lkl_FreeFn free_ud);
LKL_API void* lkl_getuserdata(lkl_State* L);
/* Перенаправляет вывод print/write (по умолчанию - stdout). fn = NULL - вернуть stdout. */
LKL_API void lkl_setwriter(lkl_State* L, lkl_WriteFn fn, void* ud);

/* --------------------------------- стек ---------------------------------- */
/* Индексы как в Lua: 1 - первый элемент (первый аргумент хост-функции),
   -1 - вершина. Внутри хост-функции видны только её аргументы и то, что она
   положила сама. */
LKL_API int lkl_gettop(lkl_State* L);
LKL_API int lkl_settop(lkl_State* L, int idx);       /* только уменьшает стек */
LKL_API int lkl_absindex(lkl_State* L, int idx);     /* отрицательный индекс -> положительный, 0 если неверный */
LKL_API int lkl_pushvalue(lkl_State* L, int idx);    /* копия (ссылка для массивов/словарей/объектов) */
#define lkl_pop(L, n) lkl_settop((L), -(n) - 1)

LKL_API int lkl_type(lkl_State* L, int idx);               /* LKL_T* */
LKL_API const char* lkl_typename(lkl_State* L, int idx);   /* статический тип: "num", "a_str", "Account"... */

LKL_API void lkl_pushnum(lkl_State* L, double v);
LKL_API void lkl_pushbool(lkl_State* L, int v);
LKL_API void lkl_pushstring(lkl_State* L, const char* s);                /* s == NULL -> "" */
LKL_API void lkl_pushlstring(lkl_State* L, const char* s, size_t len);   /* с нулевыми байтами */
/* Новый пустой массив/словарь с типом элементов elem_type: "num", "str", "a_num", "Account"...
   Тип blueprint допустим, только если он уже скомпилирован (загружен чанк с его
   определением); иначе - LKL_ERR_API. */
LKL_API int lkl_pusharray(lkl_State* L, const char* elem_type);
LKL_API int lkl_pushdict(lkl_State* L, const char* elem_type);
/* Пустая ссылка на объект blueprint (как "Account ghost" в скрипте); blueprint
   должен быть уже скомпилирован. */
LKL_API int lkl_pushnull(lkl_State* L, const char* blueprint);

LKL_API double lkl_tonum(lkl_State* L, int idx);                 /* 0 если не число */
LKL_API int lkl_tobool(lkl_State* L, int idx);                   /* 0 если не bool */
/* Указатель на байты строки (всегда с '\0' на конце); действителен, пока строка на стеке. */
LKL_API const char* lkl_tostring(lkl_State* L, int idx, size_t* len);

/* ---------------------------- массивы (индекс с 0) ----------------------- */
LKL_API long lkl_arraylen(lkl_State* L, int idx);                /* длина; -1 - не массив (НЕ ошибка API) */
LKL_API int lkl_arrayget(lkl_State* L, int idx, size_t i);       /* кладёт элемент на стек */
LKL_API int lkl_arrayset(lkl_State* L, int idx, size_t i);       /* снимает вершину; i == длина - добавить */
LKL_API int lkl_arrayappend(lkl_State* L, int idx);              /* снимает вершину */
LKL_API int lkl_arrayremove(lkl_State* L, int idx, size_t i);
LKL_API int lkl_arrayclear(lkl_State* L, int idx);

/* -------------------------------- словари -------------------------------- */
LKL_API long lkl_dictlen(lkl_State* L, int idx);                 /* длина; -1 - не словарь (НЕ ошибка API) */
LKL_API int lkl_dicthas(lkl_State* L, int idx, const char* key); /* 1/0, -1 - ошибка */
LKL_API int lkl_dictget(lkl_State* L, int idx, const char* key); /* кладёт значение; ошибка, если ключа нет */
LKL_API int lkl_dictset(lkl_State* L, int idx, const char* key); /* снимает вершину */
LKL_API int lkl_dictremove(lkl_State* L, int idx, const char* key);
LKL_API int lkl_dictclear(lkl_State* L, int idx);
/* Обход в порядке добавления: кладёт КЛЮЧ и ЗНАЧЕНИЕ i-й пары; 1 - есть, 0 - конец, -1 - ошибка. */
LKL_API int lkl_dictentry(lkl_State* L, int idx, size_t i);

/* --------------------- объекты blueprint (публичные члены) --------------- */
LKL_API int lkl_newobject(lkl_State* L, const char* blueprint);  /* поля - по умолчанию, как у new */
LKL_API int lkl_getfield(lkl_State* L, int idx, const char* name);
LKL_API int lkl_setfield(lkl_State* L, int idx, const char* name);   /* снимает вершину */

/* ------------------------ глобальные переменные скрипта ------------------ */
LKL_API int lkl_getglobal(lkl_State* L, const char* name);       /* кладёт значение */
/* Снимает вершину и записывает в глобальную переменную; если её ещё нет - создаёт
   (скрипты увидят её как уже объявленную хостом с этим типом; при конфликте типов
   скрипт получит "global 'x' was pre-declared by the host as '<тип>'"). Тип обязан совпасть. */
LKL_API int lkl_setglobal(lkl_State* L, const char* name);

/* ------------------------ вызов скрипта из C (C -> скрипт) --------------- */
/* Вызывает функцию верхнего уровня. На стеке: аргументы по порядку (последний на
   вершине). Все nargs аргументов снимаются в любом случае, в том числе при ошибке
   (если на стеке их меньше nargs - снимается всё, что есть); результат (если
   функция не void) кладётся на стек только при успехе. Типы аргументов проверяются
   точно. */
LKL_API int lkl_call(lkl_State* L, const char* func, int nargs);
/* Вызывает публичный метод. На стеке: объект, затем nargs аргументов. Снимаются и
   объект, и аргументы (и при ошибке тоже) - чтобы вызвать несколько методов, перед
   каждым делайте lkl_pushvalue(L, <индекс объекта>). */
LKL_API int lkl_callmethod(lkl_State* L, const char* method, int nargs);

#ifdef __cplusplus
}
#endif
#endif /* LKLBRIDGE_H */
