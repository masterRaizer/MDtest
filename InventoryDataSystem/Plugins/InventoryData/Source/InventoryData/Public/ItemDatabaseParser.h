#pragma once

#include "CoreMinimal.h"
#include "InventoryTypes.h"

class UDataTable;
class UInventoryDataSettings;

/**
 * Stateless reader of item DataTables. Reads rows through the reflection system,
 * so it works with any row struct (C++ or Blueprint) given a field mapping.
 * Has no world dependency: usable at runtime, in the editor and in the UMG designer.
 */
class INVENTORYDATA_API FItemDatabaseParser
{
public:
	/** Appends the items of one table to OutItems. IDs already in InOutSeenIds are skipped. Returns the number added. */
	static int32 ParseTable(UDataTable& Table, const UInventoryDataSettings& Settings,
		TArray<FItemDescriptor>& OutItems, TSet<FName>& InOutSeenIds, TArray<FString>* OutWarnings = nullptr);

	static TArray<FItemDescriptor> ParseTables(const TArray<UDataTable*>& Tables, const UInventoryDataSettings& Settings,
		TArray<FString>* OutWarnings = nullptr);

	/** Synchronously loads the tables listed in project settings. */
	static TArray<UDataTable*> LoadConfiguredTables(const UInventoryDataSettings& Settings);

	/** Item IDs from the configured tables, for meta=(GetOptions=...) dropdowns in the Details panel. */
	static TArray<FString> GetConfiguredItemIdOptions();

	/** Replaces {Name}, {Id} and {Rarity} in Format. */
	static FString FormatLabel(const FString& Format, const FItemDescriptor& Item);

	/** One label per item (same order), with " (Id)" appended on collisions so every label maps to exactly one item. */
	static TArray<FString> BuildUniqueLabels(const TArray<FItemDescriptor>& Items, const FString& Format);

	/** Indices of the items that pass the filter, in display order. */
	static TArray<int32> SelectItems(const TArray<FItemDescriptor>& Items, const FItemListFilter& Filter);
};
