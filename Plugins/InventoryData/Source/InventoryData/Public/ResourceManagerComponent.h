#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ResourceManagerComponent.generated.h"

class FNumericProperty;

UENUM(BlueprintType)
enum class EResourceOpResult : uint8
{
	Success,
	/** Applied, but the stored value differs from the requested one because of limits or the property type range. */
	Clamped,
	/** No numeric property with this name, or access to it is not allowed. */
	NotFound,
	Insufficient,
	InvalidAmount,
	/** Modification requested on a client while bRequireAuthority is set. */
	NoAuthority
};

USTRUCT(BlueprintType)
struct INVENTORYDATA_API FResourceLimits
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Limits")
	bool bClampMin = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Limits", meta = (EditCondition = "bClampMin"))
	float MinValue = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Limits")
	bool bClampMax = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Limits", meta = (EditCondition = "bClampMax"))
	float MaxValue = 0.f;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnResourceValueChanged, FName, ResourceName, float, OldValue, float, NewValue);

/**
 * Reads and writes numeric resources (currency, materials, ...) by name through the reflection system.
 * A resource is any numeric property (int8..int64, uint8..uint64, float, double; enums excluded) of the
 * resource source object. Nested struct members are addressed with dots: "Wallet.Gold".
 *
 * Resource source:
 *  - this component (default): make a Blueprint/C++ subclass and add variables like Gold, Wood;
 *  - the owning actor (bUseOwnerAsSource), e.g. a PlayerState with UPROPERTY int32 Gold;
 *  - any object passed to SetResourceSource.
 *
 * Access: if AllowedResources is set, only those names resolve. Otherwise any BlueprintVisible numeric
 * property that is not declared by engine classes is accessible.
 *
 * Integer resources are exact up to 2^53; changes on integer properties are rounded to whole numbers.
 */
UCLASS(ClassGroup = (Inventory), Blueprintable, meta = (BlueprintSpawnableComponent))
class INVENTORYDATA_API UResourceManagerComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UResourceManagerComponent();

	UFUNCTION(BlueprintCallable, Category = "Resources")
	void SetResourceSource(UObject* NewSource);

	UFUNCTION(BlueprintPure, Category = "Resources")
	UObject* GetResourceSource() const;

	UFUNCTION(BlueprintPure, Category = "Resources")
	bool HasResource(FName ResourceName) const;

	/** Names of all accessible top-level resources (or of AllowedResources, if set). */
	UFUNCTION(BlueprintPure, Category = "Resources")
	TArray<FName> GetResourceNames() const;

	UFUNCTION(BlueprintPure, Category = "Resources")
	float GetResourceValue(FName ResourceName, bool& bFound) const;

	UFUNCTION(BlueprintCallable, Category = "Resources")
	EResourceOpResult SetResourceValue(FName ResourceName, float NewValue);

	/** Adds Delta (may be negative); the result is clamped by the resource limits. */
	UFUNCTION(BlueprintCallable, Category = "Resources")
	EResourceOpResult AddResource(FName ResourceName, float Delta);

	/** Subtracts Amount only if the whole amount is available. */
	UFUNCTION(BlueprintCallable, Category = "Resources")
	EResourceOpResult TrySpendResource(FName ResourceName, float Amount);

	UFUNCTION(BlueprintPure, Category = "Resources")
	bool CanAfford(FName ResourceName, float Amount) const;

	/** All-or-nothing spend of several resources (crafting, shop prices). Nothing changes unless every cost is affordable. */
	UFUNCTION(BlueprintCallable, Category = "Resources")
	EResourceOpResult TrySpendResources(const TMap<FName, float>& Costs);

	// int64 variants for currencies that exceed float precision (~16.7M).

	UFUNCTION(BlueprintPure, Category = "Resources|Int64", meta = (DisplayName = "Get Resource Value (Int64)"))
	int64 GetResourceValueInt64(FName ResourceName, bool& bFound) const;

	UFUNCTION(BlueprintCallable, Category = "Resources|Int64", meta = (DisplayName = "Set Resource Value (Int64)"))
	EResourceOpResult SetResourceValueInt64(FName ResourceName, int64 NewValue);

	UFUNCTION(BlueprintCallable, Category = "Resources|Int64", meta = (DisplayName = "Add Resource (Int64)"))
	EResourceOpResult AddResourceInt64(FName ResourceName, int64 Delta);

	UFUNCTION(BlueprintCallable, Category = "Resources|Int64", meta = (DisplayName = "Try Spend Resource (Int64)"))
	EResourceOpResult TrySpendResourceInt64(FName ResourceName, int64 Amount);

	// C++ API in double precision; the Blueprint functions above forward here.

	bool TryGetValue(FName ResourceName, double& OutValue) const;
	EResourceOpResult SetValue(FName ResourceName, double NewValue);
	EResourceOpResult ModifyValue(FName ResourceName, double Delta);
	EResourceOpResult TrySpend(FName ResourceName, double Amount);

	/** Broadcast on the machine where the change was made. For replicated sources, use RepNotify on the client side. */
	UPROPERTY(BlueprintAssignable, Category = "Resources")
	FOnResourceValueChanged OnResourceChanged;

protected:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resources|Source")
	bool bUseOwnerAsSource = false;

	/** Whitelist of resource names/paths ("Gold", "Wallet.Gold"). Empty = any accessible numeric property. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resources|Access")
	TArray<FName> AllowedResources;

	/** Refuse modifications on non-authority instances, so clients cannot change server-owned values locally. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resources|Access")
	bool bRequireAuthority = true;

	/** Limits for resources not listed in ResourceLimits. By default values cannot go below zero. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resources|Limits")
	FResourceLimits DefaultLimits;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resources|Limits")
	TMap<FName, FResourceLimits> ResourceLimits;

private:
	struct FResolvedResource
	{
		FNumericProperty* Property = nullptr;
		void* ValuePtr = nullptr;
	};

	bool Resolve(FName ResourceName, FResolvedResource& Out) const;
	bool IsAccessibleProperty(const FProperty* Property) const;
	bool HasModifyAuthority() const;
	const FResourceLimits& GetLimits(FName ResourceName) const;
	/** Value after spending Amount; false if that would go below the resource floor. */
	bool ComputeSpend(FName ResourceName, const FResolvedResource& Resource, double Amount, double& OutNewValue) const;
	EResourceOpResult Apply(FName ResourceName, const FResolvedResource& Resource, double RequestedValue);

	static double ReadValue(const FResolvedResource& Resource);
	static void WriteValue(const FResolvedResource& Resource, double Value);
	static double ClampToTypeRange(const FNumericProperty* Property, double Value);
	static bool IsUnsigned(const FNumericProperty* Property);

	TWeakObjectPtr<UObject> SourceOverride;
};
