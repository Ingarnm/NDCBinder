// NDCBinderTestTypes.cpp

#include "NDCBinderTestTypes.h"

#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"

bool UNDCBinderTestFunctionHost::bWasCalled = false;
FNDCBinderTestContext UNDCBinderTestFunctionHost::LastContext;
TArray<FVector> UNDCBinderTestFunctionHost::RecordedLocations;
TStrongObjectPtr<USceneComponent> UNDCBinderTestFunctionHost::ComponentToReturn;

void UNDCBinderTestFunctionHost::ResetCallRecord()
{
	bWasCalled = false;
	LastContext = FNDCBinderTestContext();
	RecordedLocations.Reset();
}

FVector UNDCBinderTestFunctionHost::RecordAndReturnLocation(const FNDCBinderTestContext& EventData) const
{
	RecordedLocations.Add(EventData.Location);
	return EventData.Location;
}

FVector UNDCBinderTestFunctionHost::NoParams() const
{
	return FVector(1.0, 2.0, 3.0);
}

FVector UNDCBinderTestFunctionHost::EventDataOnly(FNDCBinderTestContext EventData) const
{
	return EventData.Location;
}

FVector UNDCBinderTestFunctionHost::EventDataOfWrongType(FNDCBinderTestOtherContext EventData) const
{
	return FVector::ZeroVector;
}

FVector UNDCBinderTestFunctionHost::ActorParam(AActor* Target) const
{
	return FVector::ZeroVector;
}

FVector UNDCBinderTestFunctionHost::EventDataThenExtra(FNDCBinderTestContext EventData, int32 Extra) const
{
	return FVector::ZeroVector;
}

FVector UNDCBinderTestFunctionHost::NonStructParam(int32 NotAStruct) const
{
	return FVector::ZeroVector;
}

void UNDCBinderTestFunctionHost::NoReturnValue() const
{
}

FVector UNDCBinderTestFunctionHost::NotConst(FNDCBinderTestContext EventData)
{
	return FVector::ZeroVector;
}

#if WITH_EDITOR
FVector UNDCBinderTestFunctionHost::EditorOnlyFunction(FNDCBinderTestContext EventData) const
{
	return FVector::ZeroVector;
}
#endif

bool UNDCBinderTestFunctionHost::ReturnsBool() const
{
	return true;
}

int32 UNDCBinderTestFunctionHost::ReturnsInt() const
{
	return 7;
}

ENDCBinderTestEnum UNDCBinderTestFunctionHost::ReturnsTestEnum() const
{
	return ENDCBinderTestEnum::Beta;
}

ENDCBinderTestOtherEnum UNDCBinderTestFunctionHost::ReturnsOtherEnum() const
{
	return ENDCBinderTestOtherEnum::Second;
}

TEnumAsByte<ENDCBinderTestByteEnum> UNDCBinderTestFunctionHost::ReturnsByteEnum() const
{
	return ByteBeta;
}

uint8 UNDCBinderTestFunctionHost::ReturnsRawByte() const
{
	return 1;
}

double UNDCBinderTestFunctionHost::ReturnsDouble() const
{
	return 0.5;
}

FLinearColor UNDCBinderTestFunctionHost::ReturnsColor() const
{
	return FLinearColor::Red;
}

FQuat UNDCBinderTestFunctionHost::ReturnsQuat() const
{
	return FQuat::Identity;
}

UNiagaraSystem* UNDCBinderTestFunctionHost::RecordCall(FNDCBinderTestContext EventData) const
{
	bWasCalled = true;
	LastContext = EventData;
	return nullptr;
}

UNiagaraSystem* UNDCBinderTestFunctionHost::RecordCallByRef(const FNDCBinderTestContext& EventData) const
{
	bWasCalled = true;
	LastContext = EventData;
	return nullptr;
}

bool UNDCBinderTestOtherHost::bWasCalled = false;

void UNDCBinderTestOtherHost::ResetCallRecord()
{
	bWasCalled = false;
}

UNiagaraSystem* UNDCBinderTestOtherHost::RecordCall(FNDCBinderTestContext EventData) const
{
	bWasCalled = true;
	return nullptr;
}

USceneComponent* UNDCBinderTestFunctionHost::ReturnsComponent() const
{
	return ComponentToReturn.Get();
}

FVector UNDCBinderTestFunctionHost::MutableEventDataRef(FNDCBinderTestContext& EventData) const
{
	// Never called: what it is for is its declared parameter, which the gate refuses.
	return EventData.Location;
}

FVector UNDCBinderTestFunctionHost::ConstEventDataRef(const FNDCBinderTestContext& EventData) const
{
	return EventData.Location;
}

UObject* UNDCBinderTestFunctionHost::ReturnsBareObject() const
{
	// Never called: what it is for is its declared return type, which no allowed-classes restriction
	// can be satisfied by.
	return nullptr;
}

FVector UNDCBinderTestFunctionHost::ReturnsVector(FNDCBinderTestContext EventData) const
{
	bWasCalled = true;
	LastContext = EventData;
	return EventData.Location;
}

FVector UNDCBinderTestFunctionHost::PerfGetVector_Implementation(const FNDCBinderTestContext& EventData) const
{
	return EventData.Location;
}

#include "NiagaraDataChannel.h"
#include "NiagaraDataChannelAsset.h"
#include "NiagaraDataChannelVariable.h"
#include "NiagaraDataChannel_Global.h"

UNiagaraDataChannelAsset* NDCBinderTestChannel::Make(TConstArrayView<TPair<FName, FNiagaraTypeDefinition>> Variables)
{
	UNiagaraDataChannelAsset* Asset = NewObject<UNiagaraDataChannelAsset>(GetTransientPackage());
	UNiagaraDataChannel* Channel = NewObject<UNiagaraDataChannel_Global>(Asset);

	FArrayProperty* VarsProp = CastField<FArrayProperty>(
		UNiagaraDataChannel::StaticClass()->FindPropertyByName(TEXT("ChannelVariables")));
	FObjectProperty* ChannelProp = CastField<FObjectProperty>(
		UNiagaraDataChannelAsset::StaticClass()->FindPropertyByName(TEXT("DataChannel")));
	if (!VarsProp || !ChannelProp)
	{
		return nullptr;
	}

	TArray<FNiagaraDataChannelVariable>& Vars =
		*VarsProp->ContainerPtrToValuePtr<TArray<FNiagaraDataChannelVariable>>(Channel);
	for (const TPair<FName, FNiagaraTypeDefinition>& Variable : Variables)
	{
		FNiagaraDataChannelVariable& Var = Vars.AddDefaulted_GetRef();
		Var.SetName(Variable.Key);
		Var.SetType(FNiagaraDataChannelVariable::ToDataChannelType(Variable.Value));
	}

	ChannelProp->SetObjectPropertyValue_InContainer(Asset, Channel);
	return Asset;
}
