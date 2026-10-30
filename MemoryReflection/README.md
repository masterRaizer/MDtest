# memrefl — динамическая рефлексия данных в памяти C++-приложений

Header-only библиотека на C++17 для анализа структур данных в памяти работающего
приложения. Состоит из трёх модулей:

| # | Модуль | Статус |
|---|---|---|
| 1 | **Поиск сигнатур**: адреса функций и глобальных переменных, которые не нужно обновлять вручную после каждого обновления бинарника | готов |
| 2 | **Generic Data Resolver**: числовые ID объектов из памяти → читаемые имена (массивы, хеш-таблицы, словари; UTF-8/UTF-16) | готов |
| 3 | **PropertyFetcher**: значения свойств по строковому пути (`"Hero.Bag.Items[1].Quantity"`) — по описаниям структур или по метаданным рефлексии самой программы | готов |

---

## Модуль 1: поиск сигнатур

После перекомпиляции функции переезжают, а смещения в инструкциях меняются. Сами
инструкции при этом обычно остаются теми же. Сигнатура фиксирует эти неизменные
байты и маскирует меняющиеся, поэтому одна и та же сигнатура находит функцию в
разных версиях программы.

```
mov rax, [rip+disp32]   test rax, rax   jz short
48 8B 05 ?? ?? ?? ??    48 85 C0        74 ??
         └ меняется ┘                      └ меняется
```

### Формат сигнатуры

| Запись | Значение |
|---|---|
| `48 8B 05` | байты в hex, регистр не важен |
| `?` или `??` | любой байт |
| `4?` / `?F` | полубайтовая маска: проверяется одна тетрада (например, любой REX-префикс `4?`) |
| `488B05??` | слитная запись тоже допускается |
| `Pattern::FromCodeStyle("\x48\x8B\x05\x00", "xxx?")` | классический формат «байты + маска» |

Ошибки формата сообщаются с позицией: `bad signature token "XY" at offset 6 ...`.
Конструктор `Pattern(text)` бросает `std::invalid_argument` (для сигнатур в коде),
а `Pattern::TryParse(text, &error)` возвращает `std::nullopt` (для конфигов).

### Как это работает

```
 Pattern("48 8B 05 ?? ?? ?? ?? 48 85 C0")
   ├─ байты:  value & mask  (?? → mask 00, 4? → mask F0)
   └─ якоря:  самые редкие заданные байты — 05 и 85 (48 и 8B в коде встречаются повсюду)

 Регион памяти ──▶ [SSE2] 16 позиций за шаг: совпали ОБА якоря? ──▶ кандидат ──▶ полная проверка ──▶ адрес
                   [без SSE2] memchr по редкому якорю ─────────────▶ кандидат ──▶ полная проверка ──▶ адрес
```

1. **Разбор.** Каждый байт превращается в пару `(value, mask)`, совпадение
   проверяется как `(byte & mask) == value`. Так `??`, `4?` и обычные байты
   обрабатываются одним сравнением, без ветвлений.
2. **Якоря.** Из полностью заданных байтов выбираются два самых редких по
   встроенной таблице частот x86-64 кода. Редкий якорь даёт мало ложных
   кандидатов. Выбор якорей влияет только на скорость, не на результат.
3. **Фильтр SSE2.** За одну итерацию проверяются 64 позиции начала (4 блока по
   16): для каждого блока два невыровненных чтения по смещениям якорей, два
   `pcmpeqb`, `pand`, `pmovmskb`. Если ни в одном блоке нет кандидата, на это
   уходит одно ветвление. Чтения не выходят за границу региона: цикл
   останавливается, когда до конца остаётся меньше блока, и хвост проверяется
   скалярно.
4. **Запасной путь.** Без SSE2 (ARM, `-DMEMREFL_NO_SIMD`) или при единственном
   заданном байте: `memchr` по якорю. В стандартных библиотеках он векторизован.
5. **Проверка кандидата** — полное сравнение по маске. Если в паттерне нет ни
   одного полностью заданного байта (только `4?`-маски), проверяется каждая позиция.

**Откуда берётся память.** Сканер работает с регионами `MemoryRegion {base, data, size}`:
`data` указывает, где байты лежат у нас, а `base` — по какому адресу они находятся
в исследуемом процессе. Поэтому один и тот же код ищет:

* в своём процессе — `ScannerForModule("app.exe")`, где `data == base`;
* в копии чужой памяти (`ReadProcessMemory`, дамп): `MemoryRegion::Snapshot(copy, size, remote_base)`;
* в PE-файле на диске: `PeImageRegions(file, size, PeLayout::File)`, адреса как в IDA.

Найденные адреса, `Read<T>()` и `ResolveRelative()` всегда выражены в адресах
исследуемого процесса. Смежные регионы склеиваются, поэтому сигнатура на границе
двух регионов тоже находится.

**Безопасность чтения в своём процессе.** На Windows секции берутся из
PE-заголовков, а затем каждая проверяется через `VirtualQuery`. Страницы
`PAGE_NOACCESS` и `PAGE_GUARD`, которые оставляют упаковщики и защитники,
пропускаются. Без этой проверки сканер мог бы уронить процесс. На Linux
используются сегменты `PT_LOAD` с флагом `PF_R` из `dl_iterate_phdr`.

### API

| Что | Назначение |
|---|---|
| `Pattern(text)` / `Pattern::TryParse(text, &err)` / `Pattern::FromCodeStyle(bytes, mask)` | Создать сигнатуру |
| `SignatureScanner(regions)` | Сканер по набору регионов (неизменяемый, потокобезопасен для чтения) |
| `FindUnique(p)` | Адрес, **только если совпадение одно**. Основной метод: «нашлось два» означает, что сигнатура устарела |
| `FindFirst(p)` / `FindAll(p, max)` / `ForEachMatch(p, cb)` | Первое / все / потоковый перебор совпадений по возрастанию адреса |
| `ResolveRelative(addr, disp_offset, insn_size)` | Цель `call/jmp rel32`, `mov/lea [rip+disp32]` |
| `FindRelativeTarget(p, disp_offset, insn_size)` | `FindUnique` + `ResolveRelative`: адрес глобальной переменной или функции по инструкции, которая к ней обращается |
| `Read<T>(addr)` / `Contains(addr, size)` | Чтение по адресу цели с проверкой границ |
| `ScannerForModule(name, filter)` / `LoadedModuleRegions(...)` | Модуль текущего процесса (`""` — главный exe). `SectionFilter::Code` — код, `All` — код и данные |
| `PeImageRegions(buf, size, PeLayout::File/Mapped, filter, base)` / `ParsePeHeaders` | PE из буфера: файл или дамп |

Перегрузки `FindFirst/FindAll/FindUnique(std::string_view)` разбирают текст при
каждом вызове. Для повторного поиска храните готовый `Pattern`.

### Примеры

**Функция и глобальная переменная в своём процессе:**

```cpp
#include "memrefl/module_regions.hpp"

const memrefl::SignatureScanner code = memrefl::ScannerForModule("app.exe");

// mov rax, [rip+g_Registry]; test rax, rax; jz ...
static const memrefl::Pattern kGetRegistry("48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ??");

if (auto fn = code.FindUnique(kGetRegistry)) {
    auto registry_ptr = code.ResolveRelative(*fn, /*disp_offset=*/3, /*insn_size=*/7);  // &g_Registry
    using GetRegistryFn = void* (*)();
    auto get_registry = reinterpret_cast<GetRegistryFn>(*fn);
} else {
    // 0 совпадений или больше одного: сигнатуру нужно обновить
}
```

**Через место вызова.** Это полезно, когда у самой функции нет уникального начала:

```cpp
// call GetRegistry; mov rbx, rax
auto call = code.FindUnique("E8 ?? ?? ?? ?? 48 8B D8");
auto fn   = call ? code.ResolveRelative(*call, 1, 5) : std::nullopt;
```

**Офлайн: проверить сигнатуры на новой версии файла до запуска:**

```cpp
std::ifstream in("app_v1.1.exe", std::ios::binary);
std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), {});
memrefl::SignatureScanner scanner(
    memrefl::PeImageRegions(file.data(), file.size(), memrefl::PeLayout::File));
auto fn = scanner.FindUnique(kGetRegistry);  // адрес как в IDA: ImageBase + RVA
```

**Другой процесс:** прочитать секцию в буфер и указать её адрес в цели:

```cpp
std::vector<std::uint8_t> copy(text_size);
ReadProcessMemory(process, reinterpret_cast<LPCVOID>(remote_text), copy.data(), copy.size(), nullptr);
memrefl::SignatureScanner scanner(memrefl::MemoryRegion::Snapshot(copy.data(), copy.size(), remote_text));
// FindUnique/ResolveRelative вернут адреса в адресном пространстве цели
```

Полный пример — `examples/scan_demo.cpp`. В нём одна сигнатура находит функцию
в двух «версиях» бинарника, где сдвинулись и код, и данные.

### Производительность

`examples/scan_benchmark.cpp`: 64 МБ реального машинного кода (секции кода
загруженных модулей, повторённые подряд), x86-64, GCC 13 `-O2`, один поток:

| Сигнатура | Совпадений | Наивный перебор | memrefl | Ускорение |
|---|---:|---:|---:|---:|
| `48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ??` | 437 | 592 МБ/с | 6 731 МБ/с | 11× |
| `48 89 5C 24 ?? 57 48 83 EC ??` | 0 | 587 МБ/с | 6 650 МБ/с | 11× |
| `55 48 89 E5 41 57` | ~28 900 | 986 МБ/с | 6 174 МБ/с | 6× |
| `DE AD BE EF ?? 13 37` (нет в данных) | 0 | 1 155 МБ/с | 6 068 МБ/с | 5× |

На 64 МБ скорость упирается в пропускную способность памяти. Если данные лежат в
кеше (256 КБ), фильтр выдаёт 22–26 ГБ/с. Для модуля размером в десятки мегабайт
поиск одной сигнатуры занимает единицы миллисекунд.

### Ограничения

* Сигнатура описывает конкретную сборку компилятора. Если изменились сами
  инструкции (другие регистры, другая оптимизация), `FindUnique` вернёт
  `nullopt`, и сигнатуру нужно обновить. Это заметно сразу, а не проявляется
  позже как неверный адрес.
* `ResolveRelative` рассчитан на 32-битные относительные смещения (x86/x86-64).
* Модули текущего процесса поддерживаются на Windows и Linux (главная программа
  на Linux — первая запись `dl_iterate_phdr`). Поиск в буферах и разбор PE
  работают на любой little-endian платформе.
* Регионы, переданные сканеру, не должны перекрываться.

---

## Модуль 2: Generic Data Resolver

Внутри программы объекты ссылаются на имена числами: индексом в таблице строк,
хешем имени или упакованным ID с флагами. Resolver переводит такие ID в
читаемые строки для отчётов и аналитики.

```
 ID из памяти ──KeyConverter──▶ LookupKey{value, hash} ──NameTable──▶ запись строки ──StringFormat──▶ UTF-8
   0x0000000B    BitField(1)        {5, 5}                 массив[5]      {char16_t*, len}     "Камера"
```

Каждое звено настраивается независимо, поэтому одни и те же классы подходят для
разных программ. Раскладка таблицы описывается данными (`*Spec`), без
наследования и перекомпиляции.

### Тип маппинга

Тип выбирается спецификацией `MappingSpec = std::variant<ArrayTableSpec, HashTableSpec, DictionarySpec>`
или готовой реализацией `NameTable`:

| Таблица | Где живёт | Ключ | Варианты раскладки |
|---|---|---|---|
| `ArrayNameTable` (`ArrayTableSpec`) | память цели | индекс | плоский массив или блоки (`chunk_capacity`); элемент — указатель на запись или сама запись (`element_is_pointer`, `element_size`, `element_offset`); число элементов известно, читается из поля или не ограничено |
| `HashNameTable` (`HashTableSpec`) | память цели | `value` для сравнения, `hash` для корзины | цепочки (`next_offset`) или линейное пробирование (`slot_size`, `empty_key`, `deleted_key`); корзина `hash % n` или `hash & (n-1)` |
| `DictionaryNameTable` (`DictionarySpec`) | у нас | хеш имени | имена хешируются как в программе (FNV-1/FNV-1a/CRC32, байты UTF-8/UTF-16/Latin-1, без учёта регистра); можно задать явные пары `ключ → имя` |
| `FallbackNameTable` | — | — | цепочка таблиц: например, живая таблица процесса, затем словарь известных имён |

Адреса таблиц задаются через `PointerPath`. Это либо адрес, либо цепочка
указателей (`PointerPath(g_registry, {0})`, то есть `*g_registry`), которая
перечитывается при каждом обращении. Если массив в живом процессе
перевыделили, resolver прочитает его новый адрес и не обратится к
освобождённой памяти. Ширина указателей цели (`pointer_size`) задаётся явно,
поэтому 64-битный инструмент читает и 32-битную цель.

### Конвертер ключей

| Конвертер | Когда | Результат |
|---|---|---|
| `KeyConverter::Direct()` | ID — индекс или уже хеш имени | `value = hash = id` |
| `KeyConverter::BitField(shift, mask, bias)` | индекс упакован с флагами или нумерация с 1 | `value = hash = ((id >> shift) & mask) + bias` |
| `KeyConverter::HashedId(algorithm, bytes)` | хеш-таблица хеширует числовой ключ перед выбором корзины | `value = id`, `hash = H(байты id)` |
| `KeyConverter::Custom(fn)` | любая своя логика; `nullopt` отклоняет ID | что вернёт `fn` |
| `a.Then(b)` | композиция | `b` применяется к `value` от `a` |

### Строки и кодировки

`StringFormat` описывает, как прочитать строку из записи, на которую указывает
таблица:

| Раскладка в памяти | Настройка |
|---|---|
| `const char*` / `const wchar_t*` с нулём в конце | `encoding`, остальное по умолчанию |
| `{uint16 len; char chars[]}` | `length_size = 2`, `chars_offset = 2` |
| `{const char16_t* data; int32 length}` (длина может включать `\0`) | `chars_by_pointer = true`, `length_offset = 8`, `length_size = 4`, `encoding = Utf16LE` |
| `uint16 header`: бит 0 — «широкая» строка, длина в битах 6..15 | `length_size = 2`, `length_shift = 6`, `utf16_flag_mask = 1`, `chars_offset = 2` |
| что-то особенное | свой `StringDecoder` вместо `StringFormat` |

Все строки на выходе — корректный UTF-8. Источник может быть в одной из трёх
кодировок:

* **UTF-8** проверяется по таблице 3-7 стандарта Unicode: overlong-формы,
  суррогаты и значения больше U+10FFFF не принимаются. Каждая битая
  последовательность заменяется одним U+FFFD по правилу «максимальной части»,
  как в браузерах и ICU.
* **UTF-16LE**: суррогатные пары собираются, одиночный суррогат заменяется на U+FFFD.
* **Latin-1** («ANSI»): байт — это кодовая точка U+00..U+FF.

Так мусор из памяти не ломает CSV, JSON и консоль. Для Windows API есть
`Utf8ToWide`. `std::codecvt` не используется: в C++17 он устарел.

### Чтение памяти

| Читатель | Назначение |
|---|---|
| `ProcessMemoryReader::Self()` | свой процесс, **безопасно**: `ReadProcessMemory` / `process_vm_readv` возвращают ошибку на невалидном адресе, а не роняют процесс |
| `ProcessMemoryReader(handle)` / `(pid)` | другой процесс: нужен `PROCESS_VM_READ` на Windows или ptrace-доступ на Linux (учитывайте `kernel.yama.ptrace_scope`) |
| `RegionReader` | снимки и дампы (`MemoryRegion` из модуля 1), офлайн-анализ, тесты |
| свой `MemoryReader` | например, чтение по сети или из файла ядра |

Строки с завершающим нулём читаются блоками, которые не пересекают границу
4 КБ-страницы. Поэтому строка в самом конце последней доступной страницы
читается без ошибки. Длина строки (`max_chars`), число элементов
(`max_count`), длина цепочек и число проб (`max_chain`) ограничены сверху. Даже
повреждённая или зацикленная таблица даёт `nullopt`, а не зависание или
падение.

### Resolver

| Метод | Назначение |
|---|---|
| `Resolve(id)` | `std::optional<std::string>` — имя или «нет имени» |
| `Label(id)` / `Labels(ids)` | всегда строка: имя или `<unknown:0x1A2B>` (формат настраивается через `fallback_label`) |
| `ForEachName(visitor)` | выгрузить всю таблицу; ключи — ключи таблицы |
| `ExportDictionary()` | снимок таблицы в локальный словарь: имена остаются доступны после завершения процесса |
| `stats()` | `lookups = cache_hits + resolved + unresolved`, отдельно `rejected` конвертером |
| `ClearCache()` | сброс кеша |

Найденные имена кешируются. Промахи по умолчанию не кешируются, потому что в
живом процессе имя может появиться позже; это включается через
`ResolverOptions::cache_misses`. `Resolve` можно вызывать из нескольких
потоков: память читается без блокировки, а кеш защищён `std::mutex`. Под ним
выполняются только поиск в хеш-таблице и копирование строки. `std::shared_mutex`
здесь не используется: в сборке MinGW под Wine 4-поточный тест с ним изредка
получал «не найдено» для известных ID, а тот же шаблон доступа без memrefl
зависал. С `std::mutex` тест прошёл 15 запусков подряд.

### Пример

```cpp
#include "memrefl/data_resolver.hpp"

auto reader = std::make_shared<memrefl::ProcessMemoryReader>(memrefl::ProcessMemoryReader::Self());

// Типы: глобальный указатель g_TypeRegistry -> {const char** names; uint32 count},
// ID = (индекс << 1) | флаг. type_registry_address = &g_TypeRegistry (например, из модуля 1).
memrefl::ArrayTableSpec types;
types.array = memrefl::PointerPath(type_registry_address, {0, 0});   // *(*g_TypeRegistry).names
types.count_field = memrefl::PointerPath(type_registry_address, {8});  // &(*g_TypeRegistry).count
memrefl::GenericDataResolver type_names(reader, types, memrefl::KeyConverter::BitField(1, ~0ull));

// Свойства: ID = CRC32 имени в нижнем регистре; таблицы в памяти нет, есть список имён.
memrefl::DictionarySpec known;
known.hashing = {memrefl::HashAlgorithm::Crc32, memrefl::Encoding::Utf8, /*ascii_lowercase=*/true};
known.names = LoadLines("property_names.txt");
memrefl::GenericDataResolver property_names(nullptr, known);

for (const Object& object : objects)
    report << type_names.Label(object.type_id) << ',' << property_names.Label(object.property_id) << '\n';
```

Полный пример — `examples/resolver_demo.cpp`. Он разбирает живые структуры
собственного процесса: массив UTF-8 имён с упакованными ID и хеш-таблицу
UTF-16 имён с цепочками. Для отсутствующих свойств используется словарь, для
неизвестных ID — подпись `<unknown:…>`.

### Ограничения

* Раскладку таблицы (смещения, ширину ключей) нужно знать заранее. Её
  определяют при исследовании программы, а сами адреса таблиц удобно находить
  сигнатурами из модуля 1.
* `ProcessMemoryReader` делает системный вызов на каждое чтение. Для
  многократного разбора больших таблиц выгодно один раз снять
  `ExportDictionary()` или читать снимок через `RegionReader`.
* Чтение живого процесса не атомарно: таблица может меняться во время чтения.
  Защита от мусора и циклов гарантирует только отсутствие падений и зависаний,
  но не согласованность снимка.
* Разбор рассчитан на little-endian цели (x86, x86-64, ARM).

---

## Модуль 3: PropertyFetcher — свойства по строковому пути

```cpp
fetcher.Get<int>(hero, "Health");                               // Result<int>
fetcher.Get<std::string>(world, "Hero.Bag.Items[1].DisplayName");
```

PropertyFetcher соединяет модули:

```
 ObjectRef{root, type}       путь "Hero.Bag.Items[1].Quantity"
      │                                 │
      ▼                                 ▼
 тип объекта ──────▶ схема ──▶ сегмент за сегментом: смещение, переход по указателю,
 (имя схемы или       │          проверка индекса массива
  TypeInfo в памяти)  │                 │
      ▲               │                 ▼
 модуль 2: ID имён ───┘           PointerPath ──▶ адрес ──▶ значение с проверкой типа
 типов и свойств                  (модуль 2)               (Result<T>)
      ▲
 модуль 1: адрес глобальной переменной (корень) по сигнатуре
```

1. **Тип объекта** задаётся именем статической схемы. Если имя не задано, тип
   читается из памяти через `ReflectionLoader`: объект → указатель на
   описание типа → ID имени → `GenericDataResolver`.
2. **Сегменты пути** ищутся в схеме с учётом родителей и превращаются в шаги
   `PointerPath`: смещение поля, переход по указателю (`StructPointer`, данные
   динамического массива). Индекс сверяется с длиной массива в памяти.
3. **Значение** читается с проверкой границ памяти. Целые расширяются со знаком,
   ID имён разрешаются в строки, строки приводятся к UTF-8. `Get<T>`
   проверяет, что значение помещается в `T`.

Корень объекта задаётся через `PointerPath`. Это может быть адрес или цепочка
от глобальной переменной, найденной сигнатурой: `ObjectRef{PointerPath(&g_World, {0})}`.
Если программа пересоздаст объект, путь останется верным.

### Откуда берутся схемы

**Статически** — по результатам исследования или из конфига:

```cpp
auto schemas = std::make_shared<memrefl::SchemaRegistry>();
schemas->Add(memrefl::TypeSchema("Actor")
                 .Add("Id", memrefl::PropertyType::UInt32, 0x00)
                 .Add(memrefl::BoolProperty("IsActive", 0x04, /*mask=*/0x01)));
schemas->Add(memrefl::TypeSchema("Character", /*parent=*/"Actor")
                 .Add("Health", memrefl::PropertyType::Int32, 0x10)
                 .Add("Defense", memrefl::PropertyType::Float, 0x14)
                 .Add(memrefl::NameProperty("Class", 0x1C))                       // ID -> строка
                 .Add(memrefl::StructProperty("Stats", 0x50, "Stats"))            // структура по значению
                 .Add(memrefl::ObjectProperty("Inventory", 0x68, "Inventory")));  // указатель
// Динамический массив {Item** data; int32 count}: count через 8 байт после data.
memrefl::PropertyDesc items = memrefl::DynamicArrayProperty("Items", 0, memrefl::PropertyType::StructPointer, 8);
items.struct_type = "Item";
```

**Из метаданных рефлексии программы.** Многие программы (движки, фреймворки
с метаобъектами, кодогенерация для сериализации) хранят описания своих типов
в памяти. `ReflectionLayout` описывает, как они устроены:

| Где | Поле `ReflectionLayout` |
|---|---|
| объект → его тип | `object_type_offset` |
| тип: ID имени, родитель, список свойств | `type_name_offset/size`, `super_offset`, `list_kind` (связный список или массив указателей), `list_offset`, `list_count_offset` |
| дескриптор свойства: ID имени, смещение, тег, вложенный тип | `property_name_offset`, `property_offset_offset`, `property_tag_offset`, `property_inner_type_offset`, `next_offset` |
| тег → тип значения | `tags[tag] = PropertyDesc-прототип` или свой `classify(reader, descriptor)` |

Работа загрузчика:

* имена типов и свойств разрешаются через `GenericDataResolver`;
* свойства родителей входят в схему первыми; свойство потомка с тем же именем
  заменяет родительское;
* неизвестный тег даёт свойство типа `Unknown`: оно видно в `Dump()`, но не читается;
* для указателя без статического типа берётся **фактический** тип объекта,
  поэтому доступны свойства наследника.

Схемы кешируются по адресу типа. Циклы в цепочке родителей и в списке свойств
обрываются, а число свойств ограничено `max_properties`.

Оба способа совмещаются. Статическая схема может содержать
`ObjectProperty("Hero", 0)` без типа, и дальше путь пойдёт по метаданным.

### Синтаксис пути и типы

Путь — это имена через точку, у массивов индекс в скобках: `Stats.Resist[2]`,
`Inventory.Items[1].Name`. Имя может содержать любые символы, кроме `.`, `[` и
`]`. Массивы массивов не поддерживаются.

| `PropertyType` | Значение (`PropertyValue`) |
|---|---|
| `Bool` (с `bool_mask` для битовых полей) | `bool` |
| `Int8..Int64` | `int64_t`, со знаковым расширением |
| `UInt8..UInt64` | `uint64_t` |
| `Float`, `Double` | `double` |
| `String` (любой `StringFormat` из модуля 2) | `std::string` (UTF-8) |
| `Name` (ID, `name_id_size`) | `std::string` через resolver (`SetNameResolver`) |
| `Pointer`, `StructPointer` | `Address` |
| `Struct` | `Address` начала структуры |
| `Array` | `Address` элементов: inline (`inline_count`) или `{T* data; ...; count}` (`count_offset`) |

### API

| Метод | Назначение |
|---|---|
| `Get<T>(obj, path)` | `Result<T>`. Для `T` целого типа значение должно помещаться в `T`, иначе ошибка `ValueOutOfRange`. Вещественный `T` принимает и целые. `std::string` и `Address` — только при точном совпадении вида значения |
| `Fetch(obj, path)` | `Result<PropertyValue>`; `FormatValue()` превращает значение в текст для отчёта |
| `Locate(obj, path)` → `Read(location)` | Путь разбирается один раз, затем значение перечитывается по готовому `PointerPath`. Подходит для частого опроса |
| `Count(obj, array_path)` | Длина массива |
| `TypeName(obj)` | Имя типа: из `ObjectRef` или из метаданных |
| `Dump(obj)` | Все свойства со значениями, родительские первыми. Ошибка одного свойства не прерывает выгрузку |

Ошибки не бросаются, а возвращаются в `Result<T>`: `ok()`, `value_or()`,
`error().code` и `error().message`. Сообщение содержит путь, например
`'Hero.Bag.Items[2]': index out of range, count is 2` или
`type 'Character' has no property 'Helth'; did you mean 'Health'?`.
`value()` на ошибке бросает `BadResultAccess`.

| Код | Когда |
|---|---|
| `InvalidPath` | синтаксис пути; поле у скаляра; индекс у не-массива |
| `UnknownType` / `UnknownProperty` | нет схемы / нет свойства (с подсказкой) |
| `NullPointer` / `UnreadableMemory` | нулевой указатель на пути / память недоступна |
| `IndexOutOfRange` / `ValueOutOfRange` | индекс за длиной / значение не помещается в `T` или мусорная длина |
| `TypeMismatch` / `UnsupportedType` | `Get<T>` с несовместимым `T` / тип `Unknown`, нет resolver'а |
| `NameUnresolved` | ID имени нет в таблице |

### Пример: все три модуля вместе

```cpp
auto reader = std::make_shared<memrefl::ProcessMemoryReader>(memrefl::ProcessMemoryReader::Self());

// Модуль 1: &g_World по инструкции "mov rax, [rip + g_World]; test rax, rax; jz ...".
auto code = memrefl::ScannerForModule("app.exe");
auto world_global = code.FindRelativeTarget(memrefl::Pattern("48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ??"), 3, 7);

// Модуль 2: таблица имён программы.
auto names = std::make_shared<memrefl::GenericDataResolver>(reader, names_table_spec);

// Модуль 3: схемы из метаданных программы.
memrefl::PropertyFetcher fetcher(reader);
fetcher.SetReflection(std::make_shared<memrefl::ReflectionLoader>(reader, names, layout))
       .SetNameResolver(names);

const memrefl::ObjectRef world{memrefl::PointerPath(*world_global, {0})};  // *g_World, тип из памяти
int health   = fetcher.Get<int>(world, "Hero.Health").value_or(0);
auto potions = fetcher.Get<int>(world, "Hero.Bag.Items[1].Quantity");
if (!potions) log(potions.error().message);
```

Полный рабочий пример — `examples/property_demo.cpp`. В нём «приложение» со
своей таблицей имён, описаниями типов (`TypeInfo`/`PropInfo`) и кодом,
обращающимся к `g_World`. Инструмент находит `g_World` сигнатурой, разрешает
имена и читает свойства по путям. Затем приложение меняет свои данные, и
значение перечитывается по сохранённому `Locate()`.

### Ограничения

* Только чтение: PropertyFetcher не изменяет память цели.
* Раскладку (смещения, теги, формат контейнеров) нужно знать заранее. Динамический
  массив описывается как `{T* data; ...; count}` с данными в первом поле
  контейнера; другие контейнеры — через `Pointer` + ручной `PointerPath`.
* Индексы динамических массивов в `Locate()` проверяются на момент вызова. Если
  массив потом укоротился, `Read()` по сохранённому пути не узнает об этом.
* Для схем из метаданных `owner_type` — тип объекта, а не родитель, где
  свойство объявлено: родительские свойства копируются в схему потомка.
* Чтение живого процесса не атомарно (см. модуль 2).

---

## Сборка и тесты

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build -j && ctest --test-dir build --output-on-failure
```

```bash
./build/scan_demo && ./build/scan_benchmark && ./build/resolver_demo && ./build/property_demo
```

* `signature_scanner_tests` и `signature_scanner_tests_scalar` — модуль 1 с SSE2 и
  без него. Главный тест сравнивает сканер с наивным эталоном на 4000 случайных
  буферах и паттернах: маски по краям, полубайтовые маски, размеры меньше и
  больше SIMD-блока. Кроме того, проверяется разбор синтетических PE32/PE32+, в
  том числе с повреждёнными заголовками, и поиск в собственном процессе.
* `data_resolver_tests` — модуль 2:
  * кодировки (битые UTF-8, одиночные суррогаты, Latin-1) и эталонные значения хешей;
  * все раскладки строк, массивов и хеш-таблиц, включая 32-битные указатели,
    перевыделение массива, зацикленные цепочки и «надгробия»;
  * кеш, статистика и одновременные `Resolve` из 4 потоков;
  * безопасное чтение собственного процесса: мусорный указатель в таблице и
    строка у границы недоступной страницы.
* `property_fetcher_tests` — модуль 3:
  * разбор путей и `Result`, преобразования `Get<T>` с проверкой диапазона;
  * все типы свойств на побайтовой раскладке: знаковое расширение, битовые поля,
    UTF-16 строки, имена, вложенные структуры, массивы inline и динамические,
    32-битные указатели;
  * наследование, в том числе с циклом в описаниях, и ошибки с подсказками;
  * `Locate`/`Read` после замены объекта и корень через глобальный указатель;
  * метаданные рефлексии: связный список и массив дескрипторов, наследники,
    переопределения, неизвестные теги, циклы, свой `classify`;
  * параллельные запросы из 4 потоков и чтение реальных структур собственного
    процесса с мусорным указателем.

Санитайзеры:

```bash
cmake -S . -B build-asan -DMEMREFL_SANITIZE=address && cmake --build build-asan -j && ctest --test-dir build-asan
```

```bash
cmake -S . -B build-tsan -DMEMREFL_SANITIZE=thread && cmake --build build-tsan -j && ctest --test-dir build-tsan
```

Под ASan тест сканирования собственного модуля пропускается. Он намеренно читает
`.rodata` целиком, включая «красные зоны» ASan вокруг глобальных переменных.
Под Wine пропускаются проверки границы недоступной страницы: Wine, в отличие от
Windows, позволяет `ReadProcessMemory` читать страницы `PAGE_NOACCESS`.
