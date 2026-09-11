// NDCBinderTestTypes.h
#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Object.h"
#include "UObject/StrongObjectPtr.h"
#include "NDCBinderTestTypes.generated.h"

class UNiagaraSystem;
class UNiagaraDataChannelAsset;
struct FNiagaraTypeDefinition;

namespace NDCBinderTestChannel
{
	/**
	 * A Data Channel asset with the given variables, built in memory.
	 *
	 * Both fields this has to set are private on Niagara's own types and have no setter — a channel is
	 * something you author, not something you construct. Reflection is how a test or a benchmark gets
	 * one anyway, and the alternative is leaving the write path unmeasured and the sync logic
	 * untested. If Epic renames either property this returns null rather than a half-built channel.
	 */
	UNiagaraDataChannelAsset* Make(TConstArrayView<TPair<FName, FNiagaraTypeDefinition>> Variables);
}
class USceneComponent;

/** The enum an Enum row's channel variable declares, in the tests that care which enum it is. */
UENUM()
enum class ENDCBinderTestEnum : uint8
{
	Alpha,
	Beta,
};

/** A second one of the same shape and width — the confusion an Enum row has to refuse. */
UENUM()
enum class ENDCBinderTestOtherEnum : uint8
{
	First,
	Second,
};

/**
 * Entries past what a byte holds. An enum row is carried as a uint8 all the way down — Niagara's own
 * WriteEnum takes one — so this is the shape the validator has to warn about rather than write
 * wrapped in silence. int32 underlying, since a uint8 one could not express it.
 */
UENUM()
enum class ENDCBinderTestWideEnum : int32
{
	Near = 1,
	Far = 300,
};

/**
 * The old-style form, reflected as an FByteProperty carrying an enum rather than an FEnumProperty.
 * EPhysicalSurface is one of these, and it is what the shipping Enum row in this project writes, so
 * "does the enum match" has to answer the same for both storage forms.
 */
UENUM()
enum ENDCBinderTestByteEnum : int
{
	ByteAlpha,
	ByteBeta,
};

/**
 * Stand-in for whatever event data a host framework passes (a cue's parameters, an ability
 * payload...). BlueprintType because the real ones are — FGameplayCueParameters included — and the
 * performance test needs a BlueprintNativeEvent to take it, which is the shape every real bound
 * getter has.
 */
USTRUCT(BlueprintType)
struct FNDCBinderTestContext
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	/** Heap-owning member on purpose: a copied-in struct must be destroyed, not just freed. */
	UPROPERTY()
	TArray<FName> Tags;

	/**
	 * A static array, which every type test would otherwise take for the single FVector it starts
	 * with. Declared here so the refusal is checked against a real reflected member rather than
	 * against a belief about what one looks like.
	 */
	UPROPERTY()
	FVector Corners[2] = { FVector::ZeroVector, FVector::ZeroVector };
};

/** A second struct type, to prove the parameter is matched by type and not just "some struct". */
USTRUCT()
struct FNDCBinderTestOtherContext
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Unused = 0;
};

//~ Four levels of plain struct nesting, so a bound path has somewhere to go and somewhere to run out.

USTRUCT()
struct FNDCBinderTestLeaf
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Point = FVector::ZeroVector;
};

USTRUCT()
struct FNDCBinderTestMid
{
	GENERATED_BODY()

	UPROPERTY()
	FNDCBinderTestLeaf Leaf;
};

USTRUCT()
struct FNDCBinderTestOuter
{
	GENERATED_BODY()

	UPROPERTY()
	FNDCBinderTestMid Mid;
};

/** Event data with something to path into, and with each of the things a path must refuse to enter. */
USTRUCT()
struct FNDCBinderTestNestedContext
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Direct = FVector::ZeroVector;

	UPROPERTY()
	FNDCBinderTestMid Mid;

	UPROPERTY()
	FNDCBinderTestOuter Outer;

	/** A container: the path carries no index, so it cannot be walked into. */
	UPROPERTY()
	TArray<FNDCBinderTestLeaf> Many;

	/**
	 * The same container, boxed — the shape a graph has when its payloads arrived already instanced.
	 * Here so the array the Blueprint node reads can be tested in both spellings; a real FArrayProperty
	 * is not something a test can build, so it borrows one from a struct that has it.
	 */
	UPROPERTY()
	TArray<FInstancedStruct> Boxed;

	/** An object: walking it would be a load and a null check on every write, not a fixed offset. */
	UPROPERTY()
	TObjectPtr<UObject> Object = nullptr;

#if WITH_EDITORONLY_DATA
	/** Resolves in the editor and would read nothing in a cooked build, so it is refused in both. */
	UPROPERTY()
	FNDCBinderTestMid EditorOnlyMid;
#endif
};

/** Event data whose members cover the enum storage forms, kept off the struct the benchmark copies. */
USTRUCT()
struct FNDCBinderTestEnumContext
{
	GENERATED_BODY()

	UPROPERTY()
	ENDCBinderTestEnum Typed = ENDCBinderTestEnum::Alpha;

	UPROPERTY()
	ENDCBinderTestOtherEnum OtherTyped = ENDCBinderTestOtherEnum::First;

	UPROPERTY()
	TEnumAsByte<ENDCBinderTestByteEnum> ByteTyped = ByteAlpha;

	/** No enum at all: the escape hatch an Enum row keeps open. */
	UPROPERTY()
	uint8 Raw = 0;
};

/**
 * Host for one function of every signature shape the writer has an opinion about. The tests bind
 * against these by name, exactly as a configured writer does at runtime.
 */
UCLASS()
class UNDCBinderTestFunctionHost : public UObject
{
	GENERATED_BODY()

public:
	//~ The two accepted shapes.
	UFUNCTION()
	FVector NoParams() const;

	UFUNCTION()
	FVector EventDataOnly(FNDCBinderTestContext EventData) const;

	//~ Rejected shapes.
	UFUNCTION()
	FVector EventDataOfWrongType(FNDCBinderTestOtherContext EventData) const;

	/** An actor is not a parameter shape: everything a binding needs comes through the event data. */
	UFUNCTION()
	FVector ActorParam(AActor* Target) const;

	UFUNCTION()
	FVector EventDataThenExtra(FNDCBinderTestContext EventData, int32 Extra) const;

	UFUNCTION()
	FVector NonStructParam(int32 NotAStruct) const;

	/**
	 * The right struct, by mutable reference. Refused: a binding is a read, and the call path hands a
	 * by-reference parameter a shallow alias of the caller's event data — writing through it would
	 * reallocate buffers the caller still owns.
	 */
	UFUNCTION()
	FVector MutableEventDataRef(UPARAM(ref) FNDCBinderTestContext& EventData) const;

	/** The same shape with the reference const, which is the one every real bound function has. */
	UFUNCTION()
	FVector ConstEventDataRef(const FNDCBinderTestContext& EventData) const;

	UFUNCTION()
	void NoReturnValue() const;

	/**
	 * Correct shape, correct return type, but not const and not Blueprint-pure: calling it could
	 * write to the owner, which for a cue notify is a CDO shared by the whole game.
	 */
	UFUNCTION()
	FVector NotConst(FNDCBinderTestContext EventData);

#if WITH_EDITOR
	/** Correct in every way except that it does not exist once the game is cooked. */
	UFUNCTION()
	FVector EditorOnlyFunction(FNDCBinderTestContext EventData) const;
#endif

	//~ Return types, for the type-match half of the check.
	UFUNCTION()
	bool ReturnsBool() const;

	UFUNCTION()
	int32 ReturnsInt() const;

	UFUNCTION()
	double ReturnsDouble() const;

	UFUNCTION()
	FLinearColor ReturnsColor() const;

	UFUNCTION()
	FQuat ReturnsQuat() const;

	//~ Enum returns, one per storage form plus the untyped one.

	UFUNCTION()
	ENDCBinderTestEnum ReturnsTestEnum() const;

	UFUNCTION()
	ENDCBinderTestOtherEnum ReturnsOtherEnum() const;

	UFUNCTION()
	TEnumAsByte<ENDCBinderTestByteEnum> ReturnsByteEnum() const;

	UFUNCTION()
	uint8 ReturnsRawByte() const;

	/** Records what a reflected call actually received, so the marshalling can be asserted. */
	UFUNCTION()
	UNiagaraSystem* RecordCall(FNDCBinderTestContext EventData) const;

	/**
	 * The same, taking the event data by reference. UHT marks a const reference parameter as an out
	 * parameter, which the writer passes as a bitwise alias rather than a deep copy — a different code
	 * path, and the one every realistic bound function uses.
	 */
	UFUNCTION()
	UNiagaraSystem* RecordCallByRef(const FNDCBinderTestContext& EventData) const;

	//~ Drivers for access context fields. A context row is an ordinary read binding, so these are
	//~ ordinary getters — which is the whole point of the per-field design.

	/** For the OwningComponent field. Hands back ComponentToReturn, so a test can make it null. */
	UFUNCTION()
	USceneComponent* ReturnsComponent() const;

	/** For the Location field, and the case where the answer depends on the event data. */
	UFUNCTION()
	FVector ReturnsVector(FNDCBinderTestContext EventData) const;

	/**
	 * Correct in every way a type check can see, and still not offerable for SystemToSpawn: that field
	 * is declared as a bare UObject and narrowed to Niagara systems by AllowedClasses metadata, which
	 * a getter promising only UObject does not satisfy.
	 */
	UFUNCTION()
	UObject* ReturnsBareObject() const;

	//~ A matched pair for the performance test: identical bodies, one reached by reflection and one
	//~ called directly. Neither records anything, so what is timed is the dispatch and nothing else.
	//~ By const reference, like every real bound function, which is the aliased parameter path.

	//~ BlueprintNativeEvent on purpose: that is what every real bound getter is, and its dispatch is
	//~ the one being measured. A plain UFUNCTION would flatter the result.
	UFUNCTION(BlueprintNativeEvent, Category = "Perf")
	FVector PerfGetVector(const FNDCBinderTestContext& EventData) const;

	FVector PerfGetVectorDirect(const FNDCBinderTestContext& EventData) const { return EventData.Location; }

	/**
	 * A value getter that also keeps what it was handed — the shape a payload row binds to, so a write
	 * of several elements can be asked whether each one really got its own event data.
	 *
	 * RecordCall answers that for one call by keeping the last; this keeps all of them in order, which
	 * is the only way to tell a batch that passed each element's payload from one that passed the same
	 * payload N times. Deliberately not the function the performance cases use: recording allocates.
	 */
	UFUNCTION()
	FVector RecordAndReturnLocation(const FNDCBinderTestContext& EventData) const;

	//~ What the last RecordCall received.
	static bool bWasCalled;
	static FNDCBinderTestContext LastContext;
	/** Every location RecordAndReturnLocation has been handed, in call order. */
	static TArray<FVector> RecordedLocations;
	static void ResetCallRecord();

	/**
	 * The world this host reports as its own, so it can stand in for the Self a Blueprint node gets.
	 *
	 * UNDCBinderLibrary's nodes take the world from their Owner, which for a graph is the actor
	 * calling them. A plain transient UObject has no world and every such call would fail on its first
	 * line, so a test that means to exercise the Blueprint path sets this instead of spawning an actor
	 * it would then have to give the bound functions to.
	 */
	UPROPERTY()
	TWeakObjectPtr<UWorld> WorldForTests;

	//~ UObject
	virtual UWorld* GetWorld() const override { return WorldForTests.Get(); }
#if WITH_EDITOR
	virtual bool ImplementsGetWorld() const override { return true; }
#endif
	//~ End UObject

	/** What ReturnsComponent hands back. Strong, so an unrooted test component is not collected. */
	static TStrongObjectPtr<USceneComponent> ComponentToReturn;
};

/**
 * A second, unrelated class carrying the same function name. Bound functions are resolved once per
 * owning class and remembered, so the tests need two classes to prove the entry is really keyed on
 * the class and does not leak from one to the other.
 */
UCLASS()
class UNDCBinderTestOtherHost : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	UNiagaraSystem* RecordCall(FNDCBinderTestContext EventData) const;

	static bool bWasCalled;
	static void ResetCallRecord();
};
