#include "ItemDatabaseSubsystem.h"

#include "Components/ComboBoxString.h"
#include "Engine/AssetManager.h"
#include "Engine/DataTable.h"
#include "InventoryDataSettings.h"
#include "ItemDatabaseParser.h"

void UItemDatabaseSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

#if WITH_EDITOR
	GetMutableDefault<UInventoryDataSettings>()->OnSettingChanged().AddUObject(this, &UItemDatabaseSubsystem::HandleSettingsChanged);
#endif

	RebuildDatabase();
}

void UItemDatabaseSubsystem::Deinitialize()
{
	UnbindTableChangeDelegates();
#if WITH_EDITOR
	GetMutableDefault<UInventoryDataSettings>()->OnSettingChanged().RemoveAll(this);
#endif

	Super::Deinitialize();
}

void UItemDatabaseSubsystem::RebuildDatabase()
{
	const UInventoryDataSettings& Settings = *GetDefault<UInventoryDataSettings>();

	UnbindTableChangeDelegates();
	ActiveTables = FItemDatabaseParser::LoadConfiguredTables(Settings);
	for (UDataTable* Table : RuntimeTables)
	{
		if (Table)
		{
			ActiveTables.AddUnique(Table);
		}
	}

	TArray<FString> Warnings;
	Items = FItemDatabaseParser::ParseTables(ActiveTables, Settings, &Warnings);
	for (const FString& Warning : Warnings)
	{
		UE_LOG(LogInventoryData, Warning, TEXT("%s"), *Warning);
	}

	const TArray<FString> Labels = FItemDatabaseParser::BuildUniqueLabels(Items, Settings.ComboBoxLabelFormat);
	ItemIndexById.Reset();
	VisualById.Reset();
	IdByLabel.Reset();
	LabelById.Reset();
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		const FItemDescriptor& Item = Items[Index];
		ItemIndexById.Add(Item.ItemId, Index);
		VisualById.Add(Item.ItemId, Item.Visual);
		IdByLabel.Add(Labels[Index], Item.ItemId);
		LabelById.Add(Item.ItemId, Labels[Index]);
	}

	BindTableChangeDelegates();

	UE_LOG(LogInventoryData, Log, TEXT("Item database built: %d items from %d tables"), Items.Num(), ActiveTables.Num());
	OnDatabaseRebuilt.Broadcast();
}

void UItemDatabaseSubsystem::RegisterItemTable(UDataTable* Table)
{
	if (Table && !RuntimeTables.Contains(Table))
	{
		RuntimeTables.Add(Table);
		RebuildDatabase();
	}
}

void UItemDatabaseSubsystem::UnregisterItemTable(UDataTable* Table)
{
	if (RuntimeTables.Remove(Table) > 0)
	{
		RebuildDatabase();
	}
}

bool UItemDatabaseSubsystem::FindItem(FName ItemId, FItemDescriptor& OutItem) const
{
	if (const int32* Index = ItemIndexById.Find(ItemId))
	{
		OutItem = Items[*Index];
		return true;
	}
	return false;
}

TArray<FName> UItemDatabaseSubsystem::GetAllItemIds() const
{
	TArray<FName> Ids;
	Ids.Reserve(Items.Num());
	for (const FItemDescriptor& Item : Items)
	{
		Ids.Add(Item.ItemId);
	}
	return Ids;
}

TArray<FItemDescriptor> UItemDatabaseSubsystem::FilterItems(const FItemListFilter& Filter) const
{
	TArray<FItemDescriptor> Result;
	for (int32 Index : FItemDatabaseParser::SelectItems(Items, Filter))
	{
		Result.Add(Items[Index]);
	}
	return Result;
}

bool UItemDatabaseSubsystem::GetItemVisual(FName ItemId, FItemVisualData& OutVisual) const
{
	if (const FItemVisualData* Visual = VisualById.Find(ItemId))
	{
		OutVisual = *Visual;
		return true;
	}
	return false;
}

FLinearColor UItemDatabaseSubsystem::GetItemColor(FName ItemId) const
{
	const FItemVisualData* Visual = VisualById.Find(ItemId);
	return Visual ? Visual->Color : FLinearColor::White;
}

FLinearColor UItemDatabaseSubsystem::GetRarityColor(EItemRarity Rarity) const
{
	return GetDefault<UInventoryDataSettings>()->GetRarityColor(Rarity);
}

UTexture2D* UItemDatabaseSubsystem::LoadItemIcon(FName ItemId) const
{
	const FItemVisualData* Visual = VisualById.Find(ItemId);
	return Visual ? Visual->Icon.LoadSynchronous() : nullptr;
}

void UItemDatabaseSubsystem::RequestItemIconAsync(FName ItemId, TFunction<void(UTexture2D*)> OnLoaded) const
{
	const FItemVisualData* Visual = VisualById.Find(ItemId);
	if (!Visual || Visual->Icon.IsNull())
	{
		OnLoaded(nullptr);
		return;
	}
	if (UTexture2D* Loaded = Visual->Icon.Get())
	{
		OnLoaded(Loaded);
		return;
	}

	TSoftObjectPtr<UTexture2D> Icon = Visual->Icon;
	UAssetManager::GetStreamableManager().RequestAsyncLoad(Icon.ToSoftObjectPath(),
		FStreamableDelegate::CreateLambda([Icon, Callback = MoveTemp(OnLoaded)]()
		{
			Callback(Icon.Get());
		}));
}

int32 UItemDatabaseSubsystem::PopulateComboBox(UComboBoxString* ComboBox, const FItemListFilter& Filter)
{
	if (!ComboBox)
	{
		return 0;
	}

	const FString PreviousSelection = ComboBox->GetSelectedOption();
	ComboBox->ClearOptions();
	for (int32 Index : FItemDatabaseParser::SelectItems(Items, Filter))
	{
		ComboBox->AddOption(LabelById.FindChecked(Items[Index].ItemId));
	}
	if (ComboBox->FindOptionIndex(PreviousSelection) != INDEX_NONE)
	{
		ComboBox->SetSelectedOption(PreviousSelection);
	}
	ComboBox->RefreshOptions();
	return ComboBox->GetOptionCount();
}

FName UItemDatabaseSubsystem::GetItemIdFromLabel(const FString& Label) const
{
	const FName* Id = IdByLabel.Find(Label);
	return Id ? *Id : NAME_None;
}

FString UItemDatabaseSubsystem::GetItemLabel(FName ItemId) const
{
	const FString* Label = LabelById.Find(ItemId);
	return Label ? *Label : FString();
}

void UItemDatabaseSubsystem::BindTableChangeDelegates()
{
#if WITH_EDITOR
	// Live-reload while editing tables during PIE.
	for (UDataTable* Table : ActiveTables)
	{
		Table->OnDataTableChanged().AddUObject(this, &UItemDatabaseSubsystem::HandleTableChanged);
	}
#endif
}

void UItemDatabaseSubsystem::UnbindTableChangeDelegates()
{
#if WITH_EDITOR
	for (UDataTable* Table : ActiveTables)
	{
		if (Table)
		{
			Table->OnDataTableChanged().RemoveAll(this);
		}
	}
#endif
}

void UItemDatabaseSubsystem::HandleTableChanged()
{
	RebuildDatabase();
}

#if WITH_EDITOR
void UItemDatabaseSubsystem::HandleSettingsChanged(UObject* Settings, FPropertyChangedEvent& Event)
{
	RebuildDatabase();
}
#endif
