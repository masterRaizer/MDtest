#pragma once

#include "CoreMinimal.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace InventoryReflection
{
	/**
	 * FindPropertyByName that also works for Blueprint (User Defined) structs,
	 * whose real property names are mangled ("Gold_12_8F3A...") — those are matched by authored name.
	 */
	inline FProperty* FindPropertyByAuthoredName(const UStruct* Struct, FName Name)
	{
		if (!Struct || Name.IsNone())
		{
			return nullptr;
		}
		if (FProperty* Property = Struct->FindPropertyByName(Name))
		{
			return Property;
		}
		const FString Wanted = Name.ToString();
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (Struct->GetAuthoredNameForField(*It).Equals(Wanted, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
		return nullptr;
	}
}
