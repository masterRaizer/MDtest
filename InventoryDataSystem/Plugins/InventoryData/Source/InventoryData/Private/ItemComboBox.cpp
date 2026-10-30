#include "ItemComboBox.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "InventoryDataSettings.h"
#include "ItemDatabaseParser.h"
#include "ItemDatabaseSubsystem.h"

#define LOCTEXT_NAMESPACE "InventoryData"

void UItemComboBox::SynchronizeProperties()
{
	Super::SynchronizeProperties();

	OnSelectionChanged.AddUniqueDynamic(this, &UItemComboBox::HandleSelectionChanged);
	if (UItemDatabaseSubsystem* Database = GetDatabase())
	{
		Database->OnDatabaseRebuilt.AddUniqueDynamic(this, &UItemComboBox::HandleDatabaseRebuilt);
	}

	RefreshItems();
}

void UItemComboBox::ReleaseSlateResources(bool bReleaseChildren)
{
	if (UItemDatabaseSubsystem* Database = GetDatabase())
	{
		Database->OnDatabaseRebuilt.RemoveDynamic(this, &UItemComboBox::HandleDatabaseRebuilt);
	}

	Super::ReleaseSlateResources(bReleaseChildren);
}

#if WITH_EDITOR
const FText UItemComboBox::GetPaletteCategory()
{
	return LOCTEXT("InventoryPaletteCategory", "Inventory");
}
#endif

void UItemComboBox::RefreshItems()
{
	TGuardValue<bool> RefreshGuard(bIsRefreshing, true);

	const FName PreviousId = GetSelectedItemId();
	const UInventoryDataSettings& Settings = *GetDefault<UInventoryDataSettings>();

	if (const UItemDatabaseSubsystem* Database = GetDatabase())
	{
		CachedItems = Database->GetItems();
	}
	else
	{
		// UMG designer / no game instance: read the tables directly.
		CachedItems = FItemDatabaseParser::ParseTables(FItemDatabaseParser::LoadConfiguredTables(Settings), Settings);
	}

	const FString& Format = LabelFormatOverride.IsEmpty() ? Settings.ComboBoxLabelFormat : LabelFormatOverride;
	const TArray<FString> Labels = FItemDatabaseParser::BuildUniqueLabels(CachedItems, Format);

	ClearOptions();
	ItemIndexByLabel.Reset();
	LabelById.Reset();

	if (bIncludeEmptyOption)
	{
		AddOption(EmptyOptionLabel);
	}
	for (int32 Index : FItemDatabaseParser::SelectItems(CachedItems, Filter))
	{
		AddOption(Labels[Index]);
		ItemIndexByLabel.Add(Labels[Index], Index);
		LabelById.Add(CachedItems[Index].ItemId, Labels[Index]);
	}

	FName ItemToSelect = PreviousId;
	if (!bHasAppliedInitialItem || IsDesignTime())
	{
		ItemToSelect = InitialItemId;
		bHasAppliedInitialItem = !IsDesignTime();
	}
	if (!SetSelectedItemId(ItemToSelect) && bIncludeEmptyOption)
	{
		SetSelectedOption(EmptyOptionLabel);
	}

	RefreshOptions();
}

bool UItemComboBox::SetSelectedItemId(FName ItemId)
{
	const FString* Label = LabelById.Find(ItemId);
	if (!Label)
	{
		return false;
	}
	SetSelectedOption(*Label);
	return true;
}

FName UItemComboBox::GetItemIdForOption(const FString& Option) const
{
	const int32* Index = ItemIndexByLabel.Find(Option);
	return Index ? CachedItems[*Index].ItemId : NAME_None;
}

bool UItemComboBox::GetItemDescriptorForOption(const FString& Option, FItemDescriptor& OutItem) const
{
	if (const int32* Index = ItemIndexByLabel.Find(Option))
	{
		OutItem = CachedItems[*Index];
		return true;
	}
	return false;
}

TArray<FString> UItemComboBox::GetItemIdOptions() const
{
	TArray<FString> Options = FItemDatabaseParser::GetConfiguredItemIdOptions();
	Options.Insert(FName(NAME_None).ToString(), 0);
	return Options;
}

void UItemComboBox::HandleSelectionChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
	if (!bIsRefreshing)
	{
		OnItemSelected.Broadcast(GetItemIdForOption(SelectedItem), SelectionType);
	}
}

void UItemComboBox::HandleDatabaseRebuilt()
{
	RefreshItems();
}

UItemDatabaseSubsystem* UItemComboBox::GetDatabase() const
{
	if (IsDesignTime())
	{
		return nullptr;
	}
	const UWorld* World = GetWorld();
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UItemDatabaseSubsystem>() : nullptr;
}

#undef LOCTEXT_NAMESPACE
