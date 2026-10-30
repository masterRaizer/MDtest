#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "InventoryTypes.h"
#include "ItemDatabaseSubsystem.generated.h"

class UComboBoxString;
class UDataTable;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnItemDatabaseRebuilt);

/**
 * Runtime item database. Parses the DataTables from project settings (plus tables registered at runtime,
 * e.g. DLC) and keeps the ItemId -> visual data mapping.
 */
UCLASS()
class INVENTORYDATA_API UItemDatabaseSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Re-parses every table. Called automatically on startup and, in the editor, when a table or the settings change. */
	UFUNCTION(BlueprintCallable, Category = "Inventory|Database")
	void RebuildDatabase();

	UFUNCTION(BlueprintCallable, Category = "Inventory|Database")
	void RegisterItemTable(UDataTable* Table);

	UFUNCTION(BlueprintCallable, Category = "Inventory|Database")
	void UnregisterItemTable(UDataTable* Table);

	// --- Lookup ---

	UFUNCTION(BlueprintPure, Category = "Inventory|Database")
	bool IsValidItem(FName ItemId) const { return ItemIndexById.Contains(ItemId); }

	UFUNCTION(BlueprintPure, Category = "Inventory|Database")
	bool FindItem(FName ItemId, FItemDescriptor& OutItem) const;

	UFUNCTION(BlueprintPure, Category = "Inventory|Database")
	TArray<FItemDescriptor> GetAllItems() const { return Items; }

	UFUNCTION(BlueprintPure, Category = "Inventory|Database")
	TArray<FName> GetAllItemIds() const;

	UFUNCTION(BlueprintPure, Category = "Inventory|Database")
	TArray<FItemDescriptor> FilterItems(const FItemListFilter& Filter) const;

	const TArray<FItemDescriptor>& GetItems() const { return Items; }

	/** Typed access to the full source row, e.g. FindItemRow<FItemTableRow>(Id)->MaxStack. */
	template <typename RowType>
	const RowType* FindItemRow(FName ItemId) const
	{
		const int32* Index = ItemIndexById.Find(ItemId);
		const FItemDescriptor* Item = Index ? &Items[*Index] : nullptr;
		return Item && Item->SourceTable ? Item->SourceTable->FindRow<RowType>(Item->RowName, TEXT("FindItemRow"), false) : nullptr;
	}

	// --- Visual mapping ---

	UFUNCTION(BlueprintPure, Category = "Inventory|Visual")
	bool GetItemVisual(FName ItemId, FItemVisualData& OutVisual) const;

	UFUNCTION(BlueprintPure, Category = "Inventory|Visual")
	FLinearColor GetItemColor(FName ItemId) const;

	UFUNCTION(BlueprintPure, Category = "Inventory|Visual")
	FLinearColor GetRarityColor(EItemRarity Rarity) const;

	UFUNCTION(BlueprintPure, Category = "Inventory|Visual")
	TMap<FName, FItemVisualData> GetVisualMap() const { return VisualById; }

	/** Synchronous load. Prefer RequestItemIconAsync (C++) or the "Async Load Asset" node in Blueprint for lists. */
	UFUNCTION(BlueprintCallable, Category = "Inventory|Visual")
	UTexture2D* LoadItemIcon(FName ItemId) const;

	/** Streams the icon in; OnLoaded receives nullptr if the item or icon does not exist. */
	void RequestItemIconAsync(FName ItemId, TFunction<void(UTexture2D*)> OnLoaded) const;

	// --- ComboBox ---

	/** Fills any UComboBoxString with item labels, keeping the current selection if it is still present. Returns the option count. */
	UFUNCTION(BlueprintCallable, Category = "Inventory|UI")
	int32 PopulateComboBox(UComboBoxString* ComboBox, const FItemListFilter& Filter);

	/** Item ID for a label produced by PopulateComboBox (e.g. from OnSelectionChanged). None if unknown. */
	UFUNCTION(BlueprintPure, Category = "Inventory|UI")
	FName GetItemIdFromLabel(const FString& Label) const;

	UFUNCTION(BlueprintPure, Category = "Inventory|UI")
	FString GetItemLabel(FName ItemId) const;

	UPROPERTY(BlueprintAssignable, Category = "Inventory|Database")
	FOnItemDatabaseRebuilt OnDatabaseRebuilt;

private:
	void BindTableChangeDelegates();
	void UnbindTableChangeDelegates();
	void HandleTableChanged();
#if WITH_EDITOR
	void HandleSettingsChanged(UObject* Settings, struct FPropertyChangedEvent& Event);
#endif

	/** Tables parsed in the last rebuild; held here so they are not garbage collected. */
	UPROPERTY(Transient)
	TArray<UDataTable*> ActiveTables;

	UPROPERTY(Transient)
	TArray<UDataTable*> RuntimeTables;

	TArray<FItemDescriptor> Items;
	TMap<FName, int32> ItemIndexById;
	TMap<FName, FItemVisualData> VisualById;
	TMap<FString, FName> IdByLabel;
	TMap<FName, FString> LabelById;
};
