#include "InventoryDataSettings.h"

UInventoryDataSettings::UInventoryDataSettings()
{
	RarityColors.Add(EItemRarity::Common, FLinearColor(0.80f, 0.80f, 0.80f));
	RarityColors.Add(EItemRarity::Uncommon, FLinearColor(0.12f, 0.85f, 0.20f));
	RarityColors.Add(EItemRarity::Rare, FLinearColor(0.10f, 0.45f, 1.00f));
	RarityColors.Add(EItemRarity::Epic, FLinearColor(0.64f, 0.21f, 0.93f));
	RarityColors.Add(EItemRarity::Legendary, FLinearColor(1.00f, 0.55f, 0.00f));
}

FLinearColor UInventoryDataSettings::GetRarityColor(EItemRarity Rarity) const
{
	const FLinearColor* Color = RarityColors.Find(Rarity);
	return Color ? *Color : FLinearColor::White;
}
