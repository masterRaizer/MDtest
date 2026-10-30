# Inventory Data — система данных инвентаря для UE4

Runtime-плагин для Unreal Engine 4.26 / 4.27 (C++, с полной поддержкой Blueprint).

| Задача | Класс |
|---|---|
| Парсинг таблиц предметов (Item Database) | `FItemDatabaseParser`, `UItemDatabaseSubsystem` |
| Авто-заполнение ComboBox названиями и ID | `UItemComboBox`, `UItemDatabaseSubsystem::PopulateComboBox` |
| Маппинг ID → иконка / цвет / редкость | `FItemVisualData`, `UItemDatabaseSubsystem::GetItemVisual` |
| Управление валютой и ресурсами через рефлексию | `UResourceManagerComponent` |

```
Plugins/InventoryData/
├── InventoryData.uplugin
├── Data/SampleItems.csv              — пример таблицы для импорта
└── Source/InventoryData/
    ├── Public/
    │   ├── InventoryTypes.h          — EItemRarity, FItemTableRow, FItemVisualData, FItemDescriptor, FItemListFilter
    │   ├── InventoryDataSettings.h   — Project Settings > Game > Inventory Data
    │   ├── ItemDatabaseParser.h      — чтение строк DataTable через рефлексию
    │   ├── ItemDatabaseSubsystem.h   — runtime-база предметов и маппинг визуала
    │   ├── ItemComboBox.h            — самозаполняющийся ComboBox (UMG)
    │   └── ResourceManagerComponent.h — менеджер ресурсов
    └── Private/ …
```

## Установка

1. Скопируйте `Plugins/InventoryData` в папку `Plugins/` вашего проекта.
2. Перегенерируйте файлы проекта и соберите его.
3. Для UE 4.25 удалите `"DeveloperSettings"` из `InventoryData.Build.cs`.

---

## 1. Item Database и авто-заполнение ComboBox

### Таблица предметов

Создайте DataTable со структурой строки **`FItemTableRow`**. Можно импортировать `Data/SampleItems.csv`
(поменяйте пути к иконкам на свои). Затем добавьте таблицу в
**Project Settings → Game → Inventory Data → Item Tables**. Таблиц может быть несколько: если ID повторяется,
вторая строка пропускается, а в лог пишется предупреждение.

Структура `FItemTableRow` не обязательна. Парсер читает строки **через рефлексию**, поэтому подойдёт
любая структура строки, как C++, так и Blueprint (User Defined Struct). Имена полей задаются в `FieldMapping`:

| Поле маппинга | Какие типы поддерживаются | Если поля нет |
|---|---|---|
| `IdField` | `FName`, `FString`, целое число | берётся имя строки (Row Name) |
| `NameField` | `FText`, `FString`, `FName` | берётся ID |
| `IconField` | `TSoftObjectPtr`, жёсткая ссылка на объект, строковый путь | иконки нет |
| `RarityField` | `EItemRarity`, любой enum, целое число, строка (`"Epic"`) | `Common` |
| `ColorField` | `FLinearColor`, `FColor` | цвет редкости |
| `ColorOverrideField` | `bool` | цвет из `ColorField` используется всегда |

Поле можно отключить, указав для него `None`.

### Виджет `Item ComboBox`

В UMG Designer он находится в палитре, в категории **Inventory**.

- Заполняется сам, в том числе прямо в дизайнере, так что в превью видны настоящие предметы.
- Во время игры берёт данные из `UItemDatabaseSubsystem` и обновляется после каждого `RebuildDatabase`.
- `Filter` задаёт фильтрацию по редкости и строке поиска, а также сортировку по имени.
- `LabelFormatOverride` задаёт формат подписи с токенами `{Name}`, `{Id}`, `{Rarity}`, например `"{Name} ({Rarity})"`.
  Подписи всегда уникальны: при совпадении к подписи добавляется ` (ID)`.
- `InitialItemId` выбирается из выпадающего списка ID в панели Details (через `meta=(GetOptions)`).
- Событие `OnItemSelected(ItemId, SelectionType)` сразу отдаёт **ID**, а не строку.
- `GetSelectedItemId()` / `SetSelectedItemId()` / `GetItemDescriptorForOption()`.

Чтобы раскрасить строки списка или добавить к ним иконки, привяжите `On Generate Widget Event`
и в обработчике вызовите `GetItemDescriptorForOption(Item)`: результат содержит `Visual.Icon` и `Visual.Color`.

### Обычный `ComboBoxString`

```cpp
UItemDatabaseSubsystem* Db = GetGameInstance()->GetSubsystem<UItemDatabaseSubsystem>();

FItemListFilter Filter;
Filter.AllowedRarities = { EItemRarity::Epic, EItemRarity::Legendary };
Db->PopulateComboBox(MyComboBox, Filter);

// в OnSelectionChanged:
const FName ItemId = Db->GetItemIdFromLabel(SelectedItem);
```

### Выпадающий список ID в своих классах (панель Details)

```cpp
UPROPERTY(EditAnywhere, meta = (GetOptions = "GetItemIdOptions"))
FName RewardItemId;

UFUNCTION()
TArray<FString> GetItemIdOptions() const { return FItemDatabaseParser::GetConfiguredItemIdOptions(); }
```

---

## 2. Маппинг ID → визуальные свойства

При каждой сборке базы для каждого предмета заполняется `TMap<FName, FItemVisualData>`:

```cpp
USTRUCT(BlueprintType)
struct FItemVisualData
{
    TSoftObjectPtr<UTexture2D> Icon;  // мягкая ссылка: иконка не грузится, пока не понадобится
    FLinearColor Color;               // собственный цвет строки или цвет редкости
    EItemRarity Rarity;
};
```

Цвета редкостей задаются в **Project Settings → Inventory Data → Rarity Colors**.

```cpp
FItemVisualData Visual;
if (Db->GetItemVisual(TEXT("Gem_Sapphire"), Visual))
{
    SlotBorder->SetBrushColor(Visual.Color);
    Db->RequestItemIconAsync(TEXT("Gem_Sapphire"), [this](UTexture2D* Icon)
    {
        if (Icon) { SlotImage->SetBrushFromTexture(Icon); }
    });
}

// Полная строка таблицы, если нужны поля помимо визуала:
if (const FItemTableRow* Row = Db->FindItemRow<FItemTableRow>(TEXT("Potion_Health")))
{
    const int32 MaxStack = Row->MaxStack;
}
```

Другие функции: `FindItem`, `IsValidItem`, `GetAllItemIds`, `FilterItems`, `GetVisualMap`, `GetItemColor`,
`GetRarityColor`, `LoadItemIcon`. Таблицы, загруженные во время игры (DLC, моды), подключаются через
`RegisterItemTable` / `UnregisterItemTable`. В редакторе база пересобирается сама, если во время PIE
изменить таблицу или настройки плагина.

---

## 3. Менеджер ресурсов (`UResourceManagerComponent`)

Компонент читает и меняет числовые `UPROPERTY` **по имени**, через `FProperty` / `FNumericProperty`.
Поддерживаются `int8…int64`, `uint8…uint64`, `float`, `double`. Enum-свойства исключены. Члены вложенных
структур адресуются через точку: `"Wallet.Gold"`.

### Где хранятся ресурсы

1. **В самом компоненте** (по умолчанию). Создайте Blueprint-наследника `ResourceManagerComponent`
   и добавьте в него переменные `Gold` (Integer64), `Wood` (Integer), `Mana` (Float).
2. **В владельце** (`bUseOwnerAsSource = true`), например в PlayerState:

   ```cpp
   UCLASS()
   class AMyPlayerState : public APlayerState
   {
       GENERATED_BODY()
   public:
       UPROPERTY(Replicated, BlueprintReadOnly, Category = "Resources") int64 Gold = 0;
       UPROPERTY(Replicated, BlueprintReadOnly, Category = "Resources") int32 Wood = 0;
       UPROPERTY(BlueprintReadOnly, Category = "Resources") FMyWallet Wallet; // Wallet.Gems
   };
   ```
3. **В любом объекте**: `SetResourceSource(SomeObject)`.

### API

| Blueprint | C++ (double) | Что делает |
|---|---|---|
| `GetResourceValue` / `…Int64` | `TryGetValue` | читает значение |
| `SetResourceValue` / `…Int64` | `SetValue` | записывает значение с учётом лимитов |
| `AddResource` / `…Int64` | `ModifyValue` | прибавляет (или отнимает) |
| `TrySpendResource` / `…Int64` | `TrySpend` | списывает, только если хватает всей суммы |
| `TrySpendResources(TMap)` | — | списывает несколько ресурсов «всё или ничего» (крафт, магазин) |
| `CanAfford`, `HasResource`, `GetResourceNames` | — | проверки и список доступных ресурсов |

Каждая операция возвращает `EResourceOpResult`: `Success`, `Clamped`, `NotFound`, `Insufficient`,
`InvalidAmount` или `NoAuthority`. После изменения вызывается событие `OnResourceChanged(Name, Old, New)`.

```cpp
UResourceManagerComponent* Res = PlayerState->FindComponentByClass<UResourceManagerComponent>();
Res->AddResourceInt64(TEXT("Gold"), 250);

TMap<FName, float> Recipe = { { TEXT("Wood"), 10.f }, { TEXT("Wallet.Gems"), 2.f } };
if (Res->TrySpendResources(Recipe) == EResourceOpResult::Success)
{
    // выдать предмет
}
```

### Ограничения и безопасность

- **Лимиты.** `DefaultLimits` (по умолчанию значение не может опуститься ниже 0) и `ResourceLimits` для
  отдельных ресурсов (`Min` / `Max`). Значения также зажимаются в диапазон типа: в `int32` переполнения не будет.
  Если отключить `bClampMin`, ресурс может уходить в минус (долг).
- **Доступ.** Если задан `AllowedResources`, работают только перечисленные имена. Если список пуст, доступно
  любое `BlueprintVisible` числовое свойство, кроме объявленных в Engine / CoreUObject и в самом плагине.
  Так через компонент нельзя изменить внутренние поля движка.
- **Сеть.** При `bRequireAuthority = true` клиент получает `NoAuthority`: менять ресурсы можно только на
  сервере, а значения приходят клиентам обычной репликацией свойств источника. `OnResourceChanged`
  вызывается там, где произошло изменение. На клиенте для этого используйте `ReplicatedUsing`.
- **Точность.** Целые значения точны до 2^53. Для больших сумм валюты используйте Int64-варианты функций:
  у float всего около 7 значащих цифр. Изменения целочисленных ресурсов округляются до целого.

---

## Пример настройки в `Config/DefaultGame.ini`

```ini
[/Script/InventoryData.InventoryDataSettings]
+ItemTables=/Game/Data/DT_Items.DT_Items
ComboBoxLabelFormat={Name} [{Id}]
```
