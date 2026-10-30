#include "ResourceManagerComponent.h"

#include <cmath>
#include "GameFramework/Actor.h"
#include "InventoryReflectionUtils.h"
#include "UObject/UnrealType.h"

namespace ResourceManagerPrivate
{
	/** Largest magnitude at which every integer is exactly representable as double. */
	constexpr double MaxExactInteger = 9007199254740992.0; // 2^53

	bool IsValidNumber(double Value)
	{
		return std::isfinite(Value);
	}
}

UResourceManagerComponent::UResourceManagerComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UResourceManagerComponent::SetResourceSource(UObject* NewSource)
{
	SourceOverride = NewSource;
}

UObject* UResourceManagerComponent::GetResourceSource() const
{
	if (UObject* Override = SourceOverride.Get())
	{
		return Override;
	}
	if (bUseOwnerAsSource)
	{
		return GetOwner();
	}
	return const_cast<UResourceManagerComponent*>(this);
}

bool UResourceManagerComponent::HasResource(FName ResourceName) const
{
	FResolvedResource Resource;
	return Resolve(ResourceName, Resource);
}

TArray<FName> UResourceManagerComponent::GetResourceNames() const
{
	TArray<FName> Names;
	if (AllowedResources.Num() > 0)
	{
		for (const FName& Name : AllowedResources)
		{
			if (HasResource(Name))
			{
				Names.Add(Name);
			}
		}
		return Names;
	}

	const UObject* Source = GetResourceSource();
	if (!Source)
	{
		return Names;
	}
	const UClass* SourceClass = Source->GetClass();
	for (TFieldIterator<FNumericProperty> It(SourceClass); It; ++It)
	{
		if (!It->IsEnum() && IsAccessibleProperty(*It))
		{
			Names.Add(FName(*SourceClass->GetAuthoredNameForField(*It)));
		}
	}
	return Names;
}

float UResourceManagerComponent::GetResourceValue(FName ResourceName, bool& bFound) const
{
	double Value = 0.0;
	bFound = TryGetValue(ResourceName, Value);
	return static_cast<float>(Value);
}

EResourceOpResult UResourceManagerComponent::SetResourceValue(FName ResourceName, float NewValue)
{
	return SetValue(ResourceName, NewValue);
}

EResourceOpResult UResourceManagerComponent::AddResource(FName ResourceName, float Delta)
{
	return ModifyValue(ResourceName, Delta);
}

EResourceOpResult UResourceManagerComponent::TrySpendResource(FName ResourceName, float Amount)
{
	return TrySpend(ResourceName, Amount);
}

int64 UResourceManagerComponent::GetResourceValueInt64(FName ResourceName, bool& bFound) const
{
	double Value = 0.0;
	bFound = TryGetValue(ResourceName, Value);
	return static_cast<int64>(Value);
}

EResourceOpResult UResourceManagerComponent::SetResourceValueInt64(FName ResourceName, int64 NewValue)
{
	return SetValue(ResourceName, static_cast<double>(NewValue));
}

EResourceOpResult UResourceManagerComponent::AddResourceInt64(FName ResourceName, int64 Delta)
{
	return ModifyValue(ResourceName, static_cast<double>(Delta));
}

EResourceOpResult UResourceManagerComponent::TrySpendResourceInt64(FName ResourceName, int64 Amount)
{
	return TrySpend(ResourceName, static_cast<double>(Amount));
}

bool UResourceManagerComponent::TryGetValue(FName ResourceName, double& OutValue) const
{
	FResolvedResource Resource;
	if (!Resolve(ResourceName, Resource))
	{
		return false;
	}
	OutValue = ReadValue(Resource);
	return true;
}

EResourceOpResult UResourceManagerComponent::SetValue(FName ResourceName, double NewValue)
{
	if (!ResourceManagerPrivate::IsValidNumber(NewValue))
	{
		return EResourceOpResult::InvalidAmount;
	}
	if (!HasModifyAuthority())
	{
		return EResourceOpResult::NoAuthority;
	}
	FResolvedResource Resource;
	if (!Resolve(ResourceName, Resource))
	{
		return EResourceOpResult::NotFound;
	}
	return Apply(ResourceName, Resource, NewValue);
}

EResourceOpResult UResourceManagerComponent::ModifyValue(FName ResourceName, double Delta)
{
	if (!ResourceManagerPrivate::IsValidNumber(Delta))
	{
		return EResourceOpResult::InvalidAmount;
	}
	if (!HasModifyAuthority())
	{
		return EResourceOpResult::NoAuthority;
	}
	FResolvedResource Resource;
	if (!Resolve(ResourceName, Resource))
	{
		return EResourceOpResult::NotFound;
	}
	return Apply(ResourceName, Resource, ReadValue(Resource) + Delta);
}

EResourceOpResult UResourceManagerComponent::TrySpend(FName ResourceName, double Amount)
{
	if (!ResourceManagerPrivate::IsValidNumber(Amount) || Amount < 0.0)
	{
		return EResourceOpResult::InvalidAmount;
	}
	if (!HasModifyAuthority())
	{
		return EResourceOpResult::NoAuthority;
	}
	FResolvedResource Resource;
	if (!Resolve(ResourceName, Resource))
	{
		return EResourceOpResult::NotFound;
	}
	double NewValue = 0.0;
	if (!ComputeSpend(ResourceName, Resource, Amount, NewValue))
	{
		return EResourceOpResult::Insufficient;
	}
	return Apply(ResourceName, Resource, NewValue);
}

bool UResourceManagerComponent::CanAfford(FName ResourceName, float Amount) const
{
	FResolvedResource Resource;
	double NewValue = 0.0;
	return Amount >= 0.f && Resolve(ResourceName, Resource) && ComputeSpend(ResourceName, Resource, Amount, NewValue);
}

EResourceOpResult UResourceManagerComponent::TrySpendResources(const TMap<FName, float>& Costs)
{
	if (!HasModifyAuthority())
	{
		return EResourceOpResult::NoAuthority;
	}

	struct FPendingSpend
	{
		FName Name;
		FResolvedResource Resource;
		double NewValue;
	};
	TArray<FPendingSpend> Pending;

	// Validate everything first so a failure leaves all resources untouched.
	for (const TPair<FName, float>& Cost : Costs)
	{
		if (!ResourceManagerPrivate::IsValidNumber(Cost.Value) || Cost.Value < 0.f)
		{
			return EResourceOpResult::InvalidAmount;
		}
		FPendingSpend Spend{Cost.Key, FResolvedResource(), 0.0};
		if (!Resolve(Cost.Key, Spend.Resource))
		{
			return EResourceOpResult::NotFound;
		}
		if (!ComputeSpend(Cost.Key, Spend.Resource, Cost.Value, Spend.NewValue))
		{
			return EResourceOpResult::Insufficient;
		}
		Pending.Add(Spend);
	}

	EResourceOpResult Result = EResourceOpResult::Success;
	for (const FPendingSpend& Spend : Pending)
	{
		if (Apply(Spend.Name, Spend.Resource, Spend.NewValue) == EResourceOpResult::Clamped)
		{
			Result = EResourceOpResult::Clamped;
		}
	}
	return Result;
}

bool UResourceManagerComponent::ComputeSpend(FName ResourceName, const FResolvedResource& Resource, double Amount, double& OutNewValue) const
{
	if (!Resource.Property->IsFloatingPoint())
	{
		Amount = FMath::RoundToDouble(Amount);
	}
	// With bClampMin off the resource may go negative (debt), down to the type range.
	const FResourceLimits& Limits = GetLimits(ResourceName);
	const double Floor = Limits.bClampMin ? Limits.MinValue : ClampToTypeRange(Resource.Property, -TNumericLimits<double>::Max());
	OutNewValue = ReadValue(Resource) - Amount;
	return OutNewValue >= Floor;
}

bool UResourceManagerComponent::Resolve(FName ResourceName, FResolvedResource& Out) const
{
	UObject* Source = GetResourceSource();
	if (!Source || ResourceName.IsNone())
	{
		return false;
	}

	const bool bWhitelisted = AllowedResources.Num() > 0;
	if (bWhitelisted && !AllowedResources.Contains(ResourceName))
	{
		return false;
	}

	TArray<FString> Segments;
	ResourceName.ToString().ParseIntoArray(Segments, TEXT("."));

	const UStruct* Struct = Source->GetClass();
	void* Container = Source;
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		FProperty* Property = InventoryReflection::FindPropertyByAuthoredName(Struct, FName(*Segments[Index]));
		if (!Property || (!bWhitelisted && !IsAccessibleProperty(Property)))
		{
			return false;
		}
		void* Value = Property->ContainerPtrToValuePtr<void>(Container);

		if (Index == Segments.Num() - 1)
		{
			FNumericProperty* Numeric = CastField<FNumericProperty>(Property);
			if (!Numeric || Numeric->IsEnum())
			{
				return false;
			}
			Out.Property = Numeric;
			Out.ValuePtr = Value;
			return true;
		}

		const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
		if (!StructProperty)
		{
			return false;
		}
		Struct = StructProperty->Struct;
		Container = Value;
	}
	return false;
}

bool UResourceManagerComponent::IsAccessibleProperty(const FProperty* Property) const
{
	if (!Property->HasAnyPropertyFlags(CPF_BlueprintVisible))
	{
		return false;
	}
	// Engine internals and this component's own settings are never resources.
	static const FName BlockedPackages[] = {
		FName(TEXT("/Script/CoreUObject")),
		FName(TEXT("/Script/Engine")),
		FName(TEXT("/Script/InventoryData"))
	};
	const UStruct* Owner = Property->GetOwnerStruct();
	const FName PackageName = Owner ? Owner->GetOutermost()->GetFName() : NAME_None;
	for (const FName& Blocked : BlockedPackages)
	{
		if (PackageName == Blocked)
		{
			return false;
		}
	}
	return true;
}

bool UResourceManagerComponent::HasModifyAuthority() const
{
	const AActor* Owner = GetOwner();
	return !bRequireAuthority || !Owner || Owner->HasAuthority();
}

const FResourceLimits& UResourceManagerComponent::GetLimits(FName ResourceName) const
{
	const FResourceLimits* Limits = ResourceLimits.Find(ResourceName);
	return Limits ? *Limits : DefaultLimits;
}

EResourceOpResult UResourceManagerComponent::Apply(FName ResourceName, const FResolvedResource& Resource, double RequestedValue)
{
	const bool bInteger = !Resource.Property->IsFloatingPoint();
	const double Target = bInteger ? FMath::RoundToDouble(RequestedValue) : RequestedValue;

	const FResourceLimits& Limits = GetLimits(ResourceName);
	double NewValue = Target;
	if (Limits.bClampMin)
	{
		NewValue = FMath::Max(NewValue, static_cast<double>(Limits.MinValue));
	}
	if (Limits.bClampMax)
	{
		NewValue = FMath::Min(NewValue, static_cast<double>(Limits.MaxValue));
	}
	if (bInteger)
	{
		NewValue = FMath::RoundToDouble(NewValue);
	}
	NewValue = ClampToTypeRange(Resource.Property, NewValue);

	const double OldValue = ReadValue(Resource);
	if (NewValue != OldValue)
	{
		WriteValue(Resource, NewValue);
		OnResourceChanged.Broadcast(ResourceName, static_cast<float>(OldValue), static_cast<float>(ReadValue(Resource)));
	}
	return NewValue == Target ? EResourceOpResult::Success : EResourceOpResult::Clamped;
}

double UResourceManagerComponent::ReadValue(const FResolvedResource& Resource)
{
	const FNumericProperty* Property = Resource.Property;
	if (Property->IsFloatingPoint())
	{
		return Property->GetFloatingPointPropertyValue(Resource.ValuePtr);
	}
	if (IsUnsigned(Property))
	{
		return static_cast<double>(Property->GetUnsignedIntPropertyValue(Resource.ValuePtr));
	}
	return static_cast<double>(Property->GetSignedIntPropertyValue(Resource.ValuePtr));
}

void UResourceManagerComponent::WriteValue(const FResolvedResource& Resource, double Value)
{
	FNumericProperty* Property = Resource.Property;
	if (Property->IsFloatingPoint())
	{
		Property->SetFloatingPointPropertyValue(Resource.ValuePtr, Value);
	}
	else if (IsUnsigned(Property))
	{
		Property->SetIntPropertyValue(Resource.ValuePtr, static_cast<uint64>(Value));
	}
	else
	{
		Property->SetIntPropertyValue(Resource.ValuePtr, static_cast<int64>(Value));
	}
}

double UResourceManagerComponent::ClampToTypeRange(const FNumericProperty* Property, double Value)
{
	using ResourceManagerPrivate::MaxExactInteger;

	if (Property->IsA<FFloatProperty>())
	{
		return FMath::Clamp(Value, -static_cast<double>(MAX_FLT), static_cast<double>(MAX_FLT));
	}
	if (Property->IsFloatingPoint())
	{
		return Value;
	}

	double Min = -MaxExactInteger;
	double Max = MaxExactInteger;
	if (Property->IsA<FInt8Property>())        { Min = MIN_int8;  Max = MAX_int8; }
	else if (Property->IsA<FInt16Property>())  { Min = MIN_int16; Max = MAX_int16; }
	else if (Property->IsA<FIntProperty>())    { Min = MIN_int32; Max = MAX_int32; }
	else if (Property->IsA<FByteProperty>())   { Min = 0.0;       Max = MAX_uint8; }
	else if (Property->IsA<FUInt16Property>()) { Min = 0.0;       Max = MAX_uint16; }
	else if (Property->IsA<FUInt32Property>()) { Min = 0.0;       Max = MAX_uint32; }
	else if (Property->IsA<FUInt64Property>()) { Min = 0.0; }
	return FMath::Clamp(Value, Min, Max);
}

bool UResourceManagerComponent::IsUnsigned(const FNumericProperty* Property)
{
	return Property->IsA<FByteProperty>() || Property->IsA<FUInt16Property>()
		|| Property->IsA<FUInt32Property>() || Property->IsA<FUInt64Property>();
}
