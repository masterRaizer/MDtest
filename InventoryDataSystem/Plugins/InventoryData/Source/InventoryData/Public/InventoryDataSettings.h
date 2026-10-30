#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "InventoryTypes.h"
#include "InventoryDataSettings.generated.h"

class UDataTable;

/** Project Settings > Game > Inventory Data. Stored in DefaultGame.ini. */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Inventory Data"))
class INVENTORYDATA_API UInventoryDataSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UInventoryDataSettings();

	/** Item Database: every row of every table becomes an item. Duplicate IDs are skipped with a warning. */
	UPROPERTY(Config, EditAnywhere, Category = "Item Database")
	TArray<TSoftObjectPtr<UDataTable>> ItemTables;

	UPROPERTY(Config, EditAnywhere, Category = "Item Database")
	FItemTableFieldMapping FieldMapping;

	UPROPERTY(Config, EditAnywhere, Category = "Visuals")
	TMap<EItemRarity, FLinearColor> RarityColors;

	/** ComboBox label. Tokens: {Name}, {Id}, {Rarity}. Labels are made unique automatically. */
	UPROPERTY(Config, EditAnywhere, Category = "UI")
	FString ComboBoxLabelFormat = TEXT("{Name} [{Id}]");

	FLinearColor GetRarityColor(EItemRarity Rarity) const;

	virtual FName GetCategoryName() const override { return TEXT("Game"); }
};
