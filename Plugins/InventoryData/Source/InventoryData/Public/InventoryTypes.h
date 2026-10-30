#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "Engine/Texture2D.h"
#include "InventoryTypes.generated.h"

INVENTORYDATA_API DECLARE_LOG_CATEGORY_EXTERN(LogInventoryData, Log, All);

UENUM(BlueprintType)
enum class EItemRarity : uint8
{
	Common,
	Uncommon,
	Rare,
	Epic,
	Legendary
};

/**
 * Reference row layout for item DataTables.
 * The parser does not require this exact struct: any row struct works as long as
 * the field names are configured in FItemTableFieldMapping.
 */
USTRUCT(BlueprintType)
struct INVENTORYDATA_API FItemTableRow : public FTableRowBase
{
	GENERATED_BODY()

	/** Unique item ID. If empty, the DataTable row name is used. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item")
	FName ItemId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item", meta = (MultiLine = true))
	FText Description;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item|Visual")
	TSoftObjectPtr<UTexture2D> Icon;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item|Visual")
	EItemRarity Rarity = EItemRarity::Common;

	/** When false, the color comes from the rarity palette in project settings. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item|Visual")
	bool bOverrideColor = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item|Visual", meta = (EditCondition = "bOverrideColor"))
	FLinearColor Color = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item", meta = (ClampMin = 1))
	int32 MaxStack = 1;
};

/** Names of the row-struct fields the parser reads. Set a field to None to skip it. */
USTRUCT(BlueprintType)
struct INVENTORYDATA_API FItemTableFieldMapping
{
	GENERATED_BODY()

	/** FName / FString / integer field. Falls back to the row name when missing or empty. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mapping")
	FName IdField = TEXT("ItemId");

	/** FText / FString / FName field. Falls back to the item ID. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mapping")
	FName NameField = TEXT("DisplayName");

	/** Soft or hard texture reference, or a string asset path. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mapping")
	FName IconField = TEXT("Icon");

	/** EItemRarity / any enum / integer / string ("Epic") field. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mapping")
	FName RarityField = TEXT("Rarity");

	/** FLinearColor or FColor field. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mapping")
	FName ColorField = TEXT("Color");

	/** Bool gating ColorField. If the row struct has no such field, ColorField always wins over the rarity color. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mapping")
	FName ColorOverrideField = TEXT("bOverrideColor");
};

/** Visual properties bound to an item ID. */
USTRUCT(BlueprintType)
struct INVENTORYDATA_API FItemVisualData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item|Visual")
	TSoftObjectPtr<UTexture2D> Icon;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item|Visual")
	FLinearColor Color = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item|Visual")
	EItemRarity Rarity = EItemRarity::Common;
};

/** One parsed item: identity, display name and visuals, plus where it came from. */
USTRUCT(BlueprintType)
struct INVENTORYDATA_API FItemDescriptor
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Item")
	FName ItemId;

	UPROPERTY(BlueprintReadOnly, Category = "Item")
	FText DisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "Item")
	FItemVisualData Visual;

	UPROPERTY(BlueprintReadOnly, Category = "Item")
	UDataTable* SourceTable = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "Item")
	FName RowName;
};

/** Which items go into a list (ComboBox) and in what order. */
USTRUCT(BlueprintType)
struct INVENTORYDATA_API FItemListFilter
{
	GENERATED_BODY()

	/** Empty = all rarities. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Filter")
	TArray<EItemRarity> AllowedRarities;

	/** Case-insensitive substring matched against display name and ID. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Filter")
	FString SearchText;

	/** Sort by display name; otherwise keep DataTable order. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Filter")
	bool bSortByName = true;
};
