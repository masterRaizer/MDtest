#include "ItemDatabaseParser.h"

#include "Algo/Sort.h"
#include "Engine/DataTable.h"
#include "InventoryDataSettings.h"
#include "InventoryReflectionUtils.h"

namespace ItemParserPrivate
{
	bool ReadName(const FProperty* Property, const void* Row, FName& Out)
	{
		if (const FNameProperty* NameProp = CastField<FNameProperty>(Property))
		{
			Out = NameProp->GetPropertyValue_InContainer(Row);
		}
		else if (const FStrProperty* StrProp = CastField<FStrProperty>(Property))
		{
			const FString& Value = StrProp->GetPropertyValue_InContainer(Row);
			Out = Value.IsEmpty() ? NAME_None : FName(*Value);
		}
		else if (const FNumericProperty* NumProp = CastField<FNumericProperty>(Property))
		{
			if (!NumProp->IsInteger() || NumProp->IsEnum())
			{
				return false;
			}
			Out = FName(*LexToString(NumProp->GetSignedIntPropertyValue(NumProp->ContainerPtrToValuePtr<void>(Row))));
		}
		return !Out.IsNone();
	}

	bool ReadText(const FProperty* Property, const void* Row, FText& Out)
	{
		if (const FTextProperty* TextProp = CastField<FTextProperty>(Property))
		{
			Out = TextProp->GetPropertyValue_InContainer(Row);
		}
		else if (const FStrProperty* StrProp = CastField<FStrProperty>(Property))
		{
			Out = FText::FromString(StrProp->GetPropertyValue_InContainer(Row));
		}
		else if (const FNameProperty* NameProp = CastField<FNameProperty>(Property))
		{
			Out = FText::FromName(NameProp->GetPropertyValue_InContainer(Row));
		}
		return !Out.IsEmpty();
	}

	bool ReadAssetPath(const FProperty* Property, const void* Row, FSoftObjectPath& Out)
	{
		// Soft references are checked first: FSoftObjectProperty derives from FObjectPropertyBase.
		if (const FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Property))
		{
			Out = SoftProp->GetPropertyValue_InContainer(Row).ToSoftObjectPath();
		}
		else if (const FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Property))
		{
			if (UObject* Object = ObjectProp->GetObjectPropertyValue_InContainer(Row))
			{
				Out = FSoftObjectPath(Object);
			}
		}
		else if (const FStrProperty* StrProp = CastField<FStrProperty>(Property))
		{
			Out = FSoftObjectPath(StrProp->GetPropertyValue_InContainer(Row));
		}
		return Out.IsValid();
	}

	bool ReadRarity(const FProperty* Property, const void* Row, EItemRarity& Out)
	{
		int64 Raw = INDEX_NONE;
		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
		{
			Raw = EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(EnumProp->ContainerPtrToValuePtr<void>(Row));
		}
		else if (const FNumericProperty* NumProp = CastField<FNumericProperty>(Property))
		{
			if (NumProp->IsInteger())
			{
				Raw = NumProp->GetSignedIntPropertyValue(NumProp->ContainerPtrToValuePtr<void>(Row));
			}
		}
		else
		{
			FName AsName;
			if (ReadName(Property, Row, AsName))
			{
				Raw = StaticEnum<EItemRarity>()->GetValueByNameString(AsName.ToString());
			}
		}

		if (Raw < 0)
		{
			return false;
		}
		// Values of a foreign enum with more entries are clamped to the highest rarity.
		Out = static_cast<EItemRarity>(FMath::Min<int64>(Raw, static_cast<int64>(EItemRarity::Legendary)));
		return true;
	}

	bool ReadColor(const FProperty* Property, const void* Row, FLinearColor& Out)
	{
		const FStructProperty* StructProp = CastField<FStructProperty>(Property);
		if (!StructProp)
		{
			return false;
		}
		const void* Value = StructProp->ContainerPtrToValuePtr<void>(Row);
		if (StructProp->Struct == TBaseStructure<FLinearColor>::Get())
		{
			Out = *static_cast<const FLinearColor*>(Value);
			return true;
		}
		if (StructProp->Struct == TBaseStructure<FColor>::Get())
		{
			Out = FLinearColor(*static_cast<const FColor*>(Value));
			return true;
		}
		return false;
	}

	bool ReadBool(const FProperty* Property, const void* Row, bool& Out)
	{
		if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
		{
			Out = BoolProp->GetPropertyValue_InContainer(Row);
			return true;
		}
		return false;
	}

	const FProperty* FindMappedField(const UScriptStruct* RowStruct, FName FieldName, const UDataTable& Table, TArray<FString>* OutWarnings)
	{
		const FProperty* Property = InventoryReflection::FindPropertyByAuthoredName(RowStruct, FieldName);
		if (!Property && !FieldName.IsNone() && OutWarnings)
		{
			OutWarnings->Add(FString::Printf(TEXT("%s: row struct %s has no field '%s'"),
				*Table.GetName(), *RowStruct->GetName(), *FieldName.ToString()));
		}
		return Property;
	}
}

int32 FItemDatabaseParser::ParseTable(UDataTable& Table, const UInventoryDataSettings& Settings,
	TArray<FItemDescriptor>& OutItems, TSet<FName>& InOutSeenIds, TArray<FString>* OutWarnings)
{
	using namespace ItemParserPrivate;

	const UScriptStruct* RowStruct = Table.GetRowStruct();
	if (!RowStruct)
	{
		if (OutWarnings)
		{
			OutWarnings->Add(FString::Printf(TEXT("%s: DataTable has no row struct"), *Table.GetName()));
		}
		return 0;
	}

	const FItemTableFieldMapping& Mapping = Settings.FieldMapping;
	// A missing ID field is not an error: row names are a valid ID source.
	const FProperty* IdProp = FindMappedField(RowStruct, Mapping.IdField, Table, nullptr);
	const FProperty* NameProp = FindMappedField(RowStruct, Mapping.NameField, Table, OutWarnings);
	const FProperty* IconProp = FindMappedField(RowStruct, Mapping.IconField, Table, OutWarnings);
	const FProperty* RarityProp = FindMappedField(RowStruct, Mapping.RarityField, Table, OutWarnings);
	const FProperty* ColorProp = FindMappedField(RowStruct, Mapping.ColorField, Table, nullptr);
	const FProperty* ColorOverrideProp = FindMappedField(RowStruct, Mapping.ColorOverrideField, Table, nullptr);

	int32 NumAdded = 0;
	for (const TPair<FName, uint8*>& RowPair : Table.GetRowMap())
	{
		const void* Row = RowPair.Value;

		FItemDescriptor Item;
		Item.RowName = RowPair.Key;
		Item.SourceTable = &Table;

		if (!ReadName(IdProp, Row, Item.ItemId))
		{
			Item.ItemId = RowPair.Key;
		}
		if (InOutSeenIds.Contains(Item.ItemId))
		{
			if (OutWarnings)
			{
				OutWarnings->Add(FString::Printf(TEXT("%s: duplicate item ID '%s' in row '%s', skipped"),
					*Table.GetName(), *Item.ItemId.ToString(), *RowPair.Key.ToString()));
			}
			continue;
		}

		if (!ReadText(NameProp, Row, Item.DisplayName))
		{
			Item.DisplayName = FText::FromName(Item.ItemId);
		}

		FSoftObjectPath IconPath;
		if (ReadAssetPath(IconProp, Row, IconPath))
		{
			Item.Visual.Icon = TSoftObjectPtr<UTexture2D>(IconPath);
		}

		ReadRarity(RarityProp, Row, Item.Visual.Rarity);
		Item.Visual.Color = Settings.GetRarityColor(Item.Visual.Rarity);

		bool bUseRowColor = true;
		ReadBool(ColorOverrideProp, Row, bUseRowColor);
		FLinearColor RowColor;
		if (bUseRowColor && ReadColor(ColorProp, Row, RowColor))
		{
			Item.Visual.Color = RowColor;
		}

		InOutSeenIds.Add(Item.ItemId);
		OutItems.Add(MoveTemp(Item));
		++NumAdded;
	}
	return NumAdded;
}

TArray<FItemDescriptor> FItemDatabaseParser::ParseTables(const TArray<UDataTable*>& Tables, const UInventoryDataSettings& Settings,
	TArray<FString>* OutWarnings)
{
	TArray<FItemDescriptor> Items;
	TSet<FName> SeenIds;
	for (UDataTable* Table : Tables)
	{
		if (Table)
		{
			ParseTable(*Table, Settings, Items, SeenIds, OutWarnings);
		}
	}
	return Items;
}

TArray<UDataTable*> FItemDatabaseParser::LoadConfiguredTables(const UInventoryDataSettings& Settings)
{
	TArray<UDataTable*> Tables;
	for (const TSoftObjectPtr<UDataTable>& TableRef : Settings.ItemTables)
	{
		if (UDataTable* Table = TableRef.LoadSynchronous())
		{
			Tables.AddUnique(Table);
		}
		else if (!TableRef.IsNull())
		{
			UE_LOG(LogInventoryData, Warning, TEXT("Item table '%s' could not be loaded"), *TableRef.ToString());
		}
	}
	return Tables;
}

TArray<FString> FItemDatabaseParser::GetConfiguredItemIdOptions()
{
	const UInventoryDataSettings& Settings = *GetDefault<UInventoryDataSettings>();
	TArray<FString> Options;
	for (const FItemDescriptor& Item : ParseTables(LoadConfiguredTables(Settings), Settings))
	{
		Options.Add(Item.ItemId.ToString());
	}
	Options.Sort();
	return Options;
}

FString FItemDatabaseParser::FormatLabel(const FString& Format, const FItemDescriptor& Item)
{
	const FString RarityName = StaticEnum<EItemRarity>()->GetNameStringByValue(static_cast<int64>(Item.Visual.Rarity));
	return Format
		.Replace(TEXT("{Name}"), *Item.DisplayName.ToString())
		.Replace(TEXT("{Id}"), *Item.ItemId.ToString())
		.Replace(TEXT("{Rarity}"), *RarityName);
}

TArray<FString> FItemDatabaseParser::BuildUniqueLabels(const TArray<FItemDescriptor>& Items, const FString& Format)
{
	TArray<FString> Labels;
	Labels.Reserve(Items.Num());
	TSet<FString> Used;
	for (const FItemDescriptor& Item : Items)
	{
		FString Label = FormatLabel(Format, Item);
		if (Label.IsEmpty() || Used.Contains(Label))
		{
			// IDs are unique, so suffixing the ID resolves the collision.
			Label += FString::Printf(TEXT(" (%s)"), *Item.ItemId.ToString());
		}
		Used.Add(Label);
		Labels.Add(MoveTemp(Label));
	}
	return Labels;
}

TArray<int32> FItemDatabaseParser::SelectItems(const TArray<FItemDescriptor>& Items, const FItemListFilter& Filter)
{
	TArray<int32> Indices;
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		const FItemDescriptor& Item = Items[Index];
		if (Filter.AllowedRarities.Num() > 0 && !Filter.AllowedRarities.Contains(Item.Visual.Rarity))
		{
			continue;
		}
		if (!Filter.SearchText.IsEmpty()
			&& !Item.DisplayName.ToString().Contains(Filter.SearchText)
			&& !Item.ItemId.ToString().Contains(Filter.SearchText))
		{
			continue;
		}
		Indices.Add(Index);
	}

	if (Filter.bSortByName)
	{
		Algo::Sort(Indices, [&Items](int32 A, int32 B)
		{
			return Items[A].DisplayName.CompareTo(Items[B].DisplayName) < 0;
		});
	}
	return Indices;
}
