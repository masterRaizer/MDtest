#pragma once

#include "CoreMinimal.h"
#include "Components/ComboBoxString.h"
#include "InventoryTypes.h"
#include "ItemComboBox.generated.h"

class UItemDatabaseSubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnItemComboSelectionChanged, FName, ItemId, ESelectInfo::Type, SelectionType);

/**
 * ComboBox that fills itself with items from the Item Database.
 * At runtime it uses UItemDatabaseSubsystem and refreshes on every rebuild;
 * in the UMG designer it parses the configured tables directly, so the preview shows real items.
 *
 * To color rows or show icons, bind OnGenerateWidgetEvent and call GetItemDescriptorForOption.
 */
UCLASS(meta = (DisplayName = "Item ComboBox"))
class INVENTORYDATA_API UItemComboBox : public UComboBoxString
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item Source")
	FItemListFilter Filter;

	/** Tokens: {Name}, {Id}, {Rarity}. Empty = format from project settings. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item Source")
	FString LabelFormatOverride;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item Source")
	bool bIncludeEmptyOption = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item Source", meta = (EditCondition = "bIncludeEmptyOption"))
	FString EmptyOptionLabel = TEXT("- None -");

	/** Selected after the first fill. The dropdown lists the IDs from the configured tables. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item Source", meta = (GetOptions = "GetItemIdOptions"))
	FName InitialItemId;

	/** Fires on user or code selection; ItemId is None for the empty option. Not fired while the list is being refilled. */
	UPROPERTY(BlueprintAssignable, Category = "Item Source")
	FOnItemComboSelectionChanged OnItemSelected;

	/** Re-reads items and rebuilds the options, keeping the selected item if it passes the filter. Call after changing Filter. */
	UFUNCTION(BlueprintCallable, Category = "Item Source")
	void RefreshItems();

	UFUNCTION(BlueprintPure, Category = "Item Source")
	FName GetSelectedItemId() const { return GetItemIdForOption(GetSelectedOption()); }

	/** Returns false if the item is not in the current option list. */
	UFUNCTION(BlueprintCallable, Category = "Item Source")
	bool SetSelectedItemId(FName ItemId);

	UFUNCTION(BlueprintPure, Category = "Item Source")
	FName GetItemIdForOption(const FString& Option) const;

	UFUNCTION(BlueprintPure, Category = "Item Source")
	bool GetItemDescriptorForOption(const FString& Option, FItemDescriptor& OutItem) const;

	UFUNCTION()
	TArray<FString> GetItemIdOptions() const;

protected:
	virtual void SynchronizeProperties() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;
#if WITH_EDITOR
	virtual const FText GetPaletteCategory() override;
#endif

private:
	UFUNCTION()
	void HandleSelectionChanged(FString SelectedItem, ESelectInfo::Type SelectionType);

	UFUNCTION()
	void HandleDatabaseRebuilt();

	UItemDatabaseSubsystem* GetDatabase() const;

	TArray<FItemDescriptor> CachedItems;
	TMap<FString, int32> ItemIndexByLabel;
	TMap<FName, FString> LabelById;
	bool bIsRefreshing = false;
	bool bHasAppliedInitialItem = false;
};
