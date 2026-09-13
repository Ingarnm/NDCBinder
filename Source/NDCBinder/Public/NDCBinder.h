// NDCBinder.h
#pragma once

#include "CoreMinimal.h"
#include "StructUtils/StructView.h"
//~ The bound-function cache resolves a UFunction's owning class inline, so both must be complete here.
#include "UObject/Class.h"
//~ DefaultAccessContext is a UPROPERTY of this type, which needs the full definition.
#include "NiagaraDataChannelAccessContext.h"
//~ FNDCWriteScope derives from FNDCWriterBase, so it needs the full definition.
#include "NiagaraDataChannelAccessor.h"
#include "NDCBinder.generated.h"

class FDataValidationContext;
class UEnum;
class USceneComponent;
class UNiagaraDataChannel;
class UNiagaraDataChannelAsset;
class UNiagaraDataChannelWriter;
class UNiagaraSystem;
struct FNiagaraTypeDefinition;

/**
 * Value type of a single NDC channel variable binding — one entry per UNiagaraDataChannelWriter
 * Write* overload. Position is an FVector written via WritePosition; Enum is a uint8 written via
 * WriteEnum. SpawnInfo and ID have no constant editor (a literal particle id or spawn burst is
 * never what you want in a data asset) — they are writable from a bound function only, and a row
 * left unbound is skipped so the channel default stands.
 */
//~ WHAT A BLUEPRINT GRAPH MAY CREATE HERE, AND WHAT IT MAY NOT
//~
//~ A writer is configuration: rows authored in a details panel and read at write time. A Blueprint
//~ may own one — a BP-only actor or component can declare an FNDCBinder variable and fill it
//~ in its class defaults — but none of it should be assembled node by node in a graph, and the two
//~ binding rows below are internals that have no business appearing in a type menu at all.
//~
//~ Three cases, three levers, because the engine gates them in three different places:
//~
//~  - The enums drop BlueprintType. IsAllowableBlueprintVariableType's UEnum overload knows nothing
//~    about internal use, so this is the only lever an enum has. A details panel builds its combo
//~    box from the UEnum itself and does not care.
//~  - The binding rows say BlueprintInternalUseOnly, which UHT expands to BlueprintType AND
//~    BlueprintInternalUseOnly — a replacement for the former, not an addition to it. They keep
//~    working on pins and leave variable types, Make, Break, split and Promote to Variable.
//~  - The writer keeps BlueprintType, because a Blueprint owning one is a supported case. It blocks
//~    only the graph-assembly half: DisableSplitPin, plus HasNativeMake/HasNativeBreak that name
//~    nothing. CanBeMade and CanBeBroken bail on the mere presence of that metadata without ever
//~    resolving the path, so the generic node never reaches the menu, and the lookup that would
//~    offer a native one instead finds no function and quietly offers nothing. Every engine use of
//~    HasNativeMake names a real function; naming nothing is this plugin's own use of the same gate,
//~    which is why it is spelled out here. UHT does check the value is a well-formed long path name
//~    — existence it does not check — so the sentinel has to look like one.
//~
//~ All of this gates editor menus, not the runtime: a node someone already placed keeps compiling.
//~ NDCBinder.Editor.GraphSurface pins all three cases so they stay this way.
/**
 * Which of UNiagaraDataChannelWriter's Write* overloads a row goes out through.
 *
 * Niagara states the set of writable types as a list of FUNCTIONS, not as data — WriteFloat,
 * WriteVector, WritePosition and the rest — so there is no engine enum to use here. The nearest
 * candidates all lose what this has to keep: ENiagaraBaseTypes is the GPU scalar layout (Half, Float,
 * Int32, Bool), and EPropertyBagPropertyType calls eight of the entries below a plain "Struct". That
 * distinction is load-bearing: Vector and Position are both an FVector in C++ and two different types
 * to Niagara (FVector3f against FNiagaraPosition), so only the overload taken tells them apart.
 *
 * Not a second source of truth: the channel's FNiagaraTypeDefinition is, and this is converted from
 * it on every sync and back again for the write. See VariableTypeFromNiagaraType.
 *
 * VALUES ARE WRITTEN OUT AND MUST NOT BE REUSED OR REORDERED. This is a UPROPERTY on every row, so
 * the number is what sits in the asset. A reordering would retype rows in assets nobody reopens, and
 * a row whose type no longer matches its channel variable resolves to nothing and is skipped in
 * silence — the loudest that mistake would ever get is an effect that stopped coming out.
 */
UENUM()
enum class ENDCVariableType : uint8
{
	Unsupported = 0,
	Bool = 1,
	Int32 = 2,
	Float = 3,
	Vector2D = 4,
	Vector = 5,
	Vector4 = 6,
	Quat = 7,
	LinearColor = 8,
	Position = 9,
	Enum = 10,
	SpawnInfo = 11,
	ID = 12,
};

/**
 * Where a written value comes from.
 *
 * Values written out and not to be reused or reordered, for the reason above and one more: unlike a
 * row's type, this one is AUTHORED rather than re-derived from the channel, so nothing would ever
 * correct it. A row that read its value back as the wrong source would quietly write its constant
 * where a binding was meant.
 */
UENUM()
enum class ENDCValueSource : uint8
{
	/** The constant stored on this binding — for a context row, the value authored on the context. */
	Constant = 0,
	/** A function on the owning object (see FNDCBinder::IsValidValueFunction for the signature). */
	Function = 1,
	/**
	 * A field of the event data struct, read straight out of it.
	 *
	 * The cheap binding, and usually the right one: no reflected call, no parameter block, just the
	 * member. Most bound functions in practice are `return EventData.Something;` — that getter is
	 * boilerplate this replaces.
	 *
	 * Deliberately the event data and not the owner. A binding has to vary per write to be worth
	 * anything, and the owner often cannot: a GameplayCueNotify executes on its CDO, so reading one
	 * of its properties yields the class default, which is what the constant on this row already is.
	 * The event data is the one thing that is genuinely per write.
	 */
	EventData = 2,
};

/**
 * Why a function may not be used as a binding, before its signature is even looked at.
 *
 * A binding is stored as a bare FName and resolved by reflection at write time, so nothing stops a
 * name from naming a function that is unsafe to call this way — it can arrive from an older asset,
 * from a copy-paste between classes, or from code setting the property directly. The check has to
 * live at resolve time, not only in the picker.
 */
enum class ENDCBindingRejection : uint8
{
	/** Usable. */
	None = 0,
	/** No function of that name on the owning class. */
	Missing,
	/** Replicated: ProcessEvent would send an RPC per write, and may not run the body locally at all. */
	Networked,
	/** Editor-only: the binding would work in the editor and silently stop existing in a cooked build. */
	EditorOnly,
	/** A delegate signature, not a callable function: it has no body to run. */
	DelegateSignature,
	/** Neither const nor Blueprint-pure, so calling it may mutate the owner (often a shared CDO). */
	NotPure,
};

/**
 * How a bound function's event data parameter is handed over — which is what one call costs.
 *
 * The whole difference is whether the struct has to be copied. A const reference does not: the call
 * hands the callee a bitwise alias of the caller's value and ProcessEvent neither copies it back nor
 * destroys it, so the cost is a memcpy of the struct's own bytes. A by-value parameter is a deep copy
 * per call — for an event data struct that owns heap data (tag containers, arrays) that is a malloc
 * per member, per call, for a value nothing is allowed to change.
 *
 * A Blueprint parameter is by value unless its pin says otherwise, so the shape the write path wants
 * is one the editor has to author deliberately; see NDCBinderCustomization's generated pin.
 */
enum class ENDCEventDataPassing : uint8
{
	/** Copied into the call, deeply. Legal, and what a Blueprint pin gives by default. */
	ByValue = 0,
	/** `const FEventData&` — aliased, not copied. What the write path wants. */
	ByConstReference,
	/**
	 * A reference the callee may write through. Refused as a binding: a binding is a read, and the
	 * alias would let it reach past the value it was asked for into the thing that produced it.
	 */
	ByMutableReference,
};

/**
 * One bound function, resolved once and remembered.
 *
 * The key is every input the answer depends on, so rebinding a row, retyping a channel variable or
 * editing Event Data Type makes the entry stop matching by construction and nothing has to remember
 * to invalidate anything. The one thing a key cannot express is a Blueprint recompile, which cleans
 * a UClass in place rather than making a new one; that is what IsRememberedFunctionStillOwnedBy is
 * for.
 *
 * Not a UPROPERTY — transient runtime state, never serialized. Game-thread only, like every caller
 * of ProcessEvent. Why each of those is the way it is: see WHAT THE TWO CACHES ON A ROW CAN REMEMBER
 * at the top of NDCBinder.cpp.
 */
struct FNDCBoundFunctionCache
{
	/** True when this cache already has an answer for exactly these inputs; OutFunction is then it, possibly null. */
	bool TryGet(const UClass* OwnerClass, FName FunctionName, const UScriptStruct* EventDataType, ENDCVariableType ValueType, const UEnum* ValueEnum, UFunction*& OutFunction, const UStruct* FieldOwnerStruct = nullptr) const
	{
		if (OwnerClass
			&& CachedClass.Get() == OwnerClass
			&& CachedFunctionName == FunctionName
			&& CachedEventDataType.Get() == EventDataType
			&& CachedValueType == ValueType
			&& CachedValueEnum == ValueEnum
#if WITH_EDITOR
			&& CachedValueEnumWeak.Get() == ValueEnum
#endif
			&& CachedFieldOwnerStruct.Get() == FieldOwnerStruct
			&& IsRememberedFunctionStillOwnedBy(OwnerClass))
		{
#if WITH_EDITOR
			// A remembered MISS is not trusted in the editor: a Blueprint can grow the function while
			// this entry lives on, and nothing in the key would notice.
			if (bCachedMiss)
			{
				return false;
			}
#endif
			OutFunction = CachedFunction.Get();
#if WITH_EDITOR
			// The development-only net under the whole scheme: the remembered answer is checked against
			// the lookup it replaced, so a hole in the reasoning is loud rather than stale.
			ensureMsgf(!OutFunction || OutFunction == OwnerClass->FindFunctionByName(FunctionName),
				TEXT("NDCBinder: cached binding '%s' on %s is stale — the class now resolves it to a different function."),
				*FunctionName.ToString(), *OwnerClass->GetName());
#endif
			return true;
		}
		return false;
	}

	/** Remembers the answer. A null Function is cached too — a broken binding stays cheap to skip. */
	void Store(const UClass* OwnerClass, FName FunctionName, const UScriptStruct* EventDataType, ENDCVariableType ValueType, const UEnum* ValueEnum, UFunction* Function, const UStruct* FieldOwnerStruct = nullptr) const
	{
		CachedClass = OwnerClass;
		CachedFunctionName = FunctionName;
		CachedEventDataType = EventDataType;
		CachedValueType = ValueType;
		CachedValueEnum = ValueEnum;
#if WITH_EDITOR
		CachedValueEnumWeak = ValueEnum;
#endif
		CachedFieldOwnerStruct = FieldOwnerStruct;
		CachedFunction = Function;
		bCachedMiss = (Function == nullptr);
	}

private:
	/**
	 * True when the remembered answer is still the class's own. A cached miss (no function of that
	 * name) has nothing to re-check and stays valid; a cached hit must still be reachable from the
	 * class, which a recompiled-away function no longer is.
	 */
	bool IsRememberedFunctionStillOwnedBy(const UClass* OwnerClass) const
	{
		const UFunction* Function = CachedFunction.Get();
		if (!Function)
		{
			// Either nothing was found last time, or the function has since been collected. Both are
			// answered correctly by "no function": the miss is the cached answer, and a collected one
			// is re-resolved because bCachedMiss says the entry was a hit.
			return bCachedMiss;
		}
		// Inherited functions are owned by a super class, so this is IsChildOf and not equality.
		return OwnerClass->IsChildOf(Function->GetOwnerClass());
	}

	mutable TWeakObjectPtr<const UClass> CachedClass;
	mutable TWeakObjectPtr<const UScriptStruct> CachedEventDataType;
	//~ Part of the key, not of the answer: an Enum row is satisfied only by its own enum, and a
	//~ channel edit can change which enum that is without touching the row's name or its type.
	//~ Raw and only ever compared, with a weak twin in the editor where an enum can be swapped and its
	//~ slot reused. Why that is safe, and what it saves: the caching note in NDCBinder.cpp.
	mutable const UEnum* CachedValueEnum = nullptr;
#if WITH_EDITOR
	mutable TWeakObjectPtr<const UEnum> CachedValueEnumWeak;
#endif
	mutable FName CachedFunctionName;
	/** Unsupported for the writer-level bindings, which have no channel variable type of their own. */
	mutable ENDCVariableType CachedValueType = ENDCVariableType::Unsupported;
	/**
	 * For a binding that targets a field of a struct (an access context row), the struct the field
	 * belongs to. A row names its field by FName, so the same name can mean a different property of a
	 * different type once the channel — and with it the context type — changes; without this in the
	 * key the remembered answer would be one validated against the field that name used to mean.
	 * Null for bindings that target no field.
	 */
	mutable TWeakObjectPtr<const UStruct> CachedFieldOwnerStruct;
	/** Weak so a collected function reads back as null instead of dangling; see IsRememberedFunctionStillOwnedBy. */
	mutable TWeakObjectPtr<UFunction> CachedFunction;
	/** Distinguishes "remembered that there is no usable function" from "remembered one that has gone". */
	mutable bool bCachedMiss = false;
};

/**
 * One property lookup remembered across writes.
 *
 * UStruct::FindPropertyByName walks the struct's property list comparing FNames, and a bound row does
 * two or three of those per write. None of the answers can differ between two writes, so the walk
 * belongs to the first one and the answer to every one after it — the same move Epic's own binding
 * system makes in FPropertyBindingBindingCollection, which resolves a binding once into the leaf
 * properties and copies through those rather than through the path.
 *
 * Keyed on the owning struct alone, because the name is a member of the row that owns the cache and
 * cannot change without the row changing. A cache is therefore only ever valid for the one name it
 * was filled for, which is why these are separate members rather than one shared cache.
 *
 * Not a UPROPERTY — transient runtime state, never serialized. Game-thread only, like every write.
 * See the caching note at the top of NDCBinder.cpp.
 */
struct FNDCFieldCache
{
	/** True when this cache already holds the answer for these inputs; OutField is then it, possibly null. */
	bool TryGet(const UScriptStruct* Owner, const FProperty*& OutField, ENDCVariableType ValueType = ENDCVariableType::Unsupported, const UEnum* ValueEnum = nullptr, int32* OutOffset = nullptr) const
	{
		if (bResolved && Owner && CachedOwner.Get() == Owner && CachedValueType == ValueType
			&& CachedValueEnum == ValueEnum
#if WITH_EDITOR
			&& CachedValueEnumWeak.Get() == ValueEnum
#endif
			&& CanRemember(Owner))
		{
			OutField = CachedField;
			if (OutOffset)
			{
				*OutOffset = CachedOffset;
			}
			return true;
		}
		return false;
	}

	/** Remembers the answer. A null Field is remembered too — a row that resolves to nothing stays cheap to skip. */
	void Store(const UScriptStruct* Owner, const FProperty* Field, ENDCVariableType ValueType = ENDCVariableType::Unsupported, const UEnum* ValueEnum = nullptr, int32 Offset = 0) const
	{
		if (!CanRemember(Owner))
		{
			return;
		}
		CachedOwner = Owner;
		CachedValueType = ValueType;
		CachedValueEnum = ValueEnum;
#if WITH_EDITOR
		CachedValueEnumWeak = ValueEnum;
#endif
		CachedField = Field;
		CachedOffset = Offset;
		bResolved = true;
	}

private:
	/**
	 * Whether a property resolved on this struct can outlive the write that resolved it: a native
	 * struct's FProperties live as long as the process, while a Blueprint struct's are deleted
	 * outright by a recompile with nothing left to test a stale pointer against — so a Blueprint
	 * struct is simply not remembered. There is no recompile outside the editor, so this compiles
	 * away there. See the caching note in NDCBinder.cpp.
	 */
	static bool CanRemember(const UScriptStruct* Owner)
	{
#if WITH_EDITOR
		return Owner && (Owner->StructFlags & STRUCT_Native) != 0;
#else
		return Owner != nullptr;
#endif
	}

	/** Weak so a collected struct reads back as null and misses, rather than matching a recycled address. */
	mutable TWeakObjectPtr<const UScriptStruct> CachedOwner;
	/**
	 * The channel variable type the remembered field was accepted for, where the answer depends on one.
	 *
	 * A payload row's cache holds "the field this row can read", which is only an answer for the type
	 * the row writes — and a row survives a channel change with its type rewritten under it, because
	 * BuildSyncedBindings carries the existing row over and then overwrites the channel-derived
	 * fields. Unsupported for the context caches, whose answer is the field itself.
	 */
	mutable ENDCVariableType CachedValueType = ENDCVariableType::Unsupported;
	//~ And the enum it was accepted for, since a channel edit can swap that too. Raw and compared, with
	//~ an editor-only weak twin, for the reasons FNDCBoundFunctionCache's own enum member gives.
	mutable const UEnum* CachedValueEnum = nullptr;
#if WITH_EDITOR
	mutable TWeakObjectPtr<const UEnum> CachedValueEnumWeak;
#endif
	/** Raw: an FProperty is an FField, not a UObject, and is owned by the struct above for as long as it. */
	mutable const FProperty* CachedField = nullptr;
	/**
	 * Byte offset of the value from the start of the owning struct — the whole path, not the leaf's own
	 * offset. A nested path is a fixed sum of member offsets, so resolving it once leaves the write with
	 * the same single addition a direct member costs. Zero for the caches that hold no path.
	 */
	mutable int32 CachedOffset = 0;
	/** Distinguishes "remembered that this name resolves to nothing" from "never asked". */
	mutable bool bResolved = false;
};

/**
 * One channel variable to write: a constant value or a bound function on the owning object.
 * Rows are normally synced from the DataChannel's Channel Variables by the editor details panel;
 * native default payloads are built with MakeFunctionBinding.
 */
USTRUCT(BlueprintInternalUseOnly)
struct NDCBINDER_API FNDCVariableBinding
{
	GENERATED_BODY()

	/** Value function resolved for the owning class (transient; see FNDCBoundFunctionCache). */
	FNDCBoundFunctionCache ValueFunctionCache;

	/**
	 * BoundEventDataField resolved against the writer's EventDataType (transient; see FNDCFieldCache).
	 * Holds null when the name resolves to nothing, or to a field whose type this row cannot write.
	 */
	FNDCFieldCache EventFieldCache;

	//~ The category on a STRUCT member never reaches a details panel — categories group the properties
	//~ of an object, and a struct's children are listed as they come. So these stay the short "NDC"
	//~ while the two places a category IS read say "Niagara Data Channel": the Blueprint nodes'
	//~ palette entry (NDCBinderLibrary) and the property a consumer declares to hold a writer.

	/** Channel variable name (synced from the DataChannel asset). */
	UPROPERTY(VisibleAnywhere, Category = "NDC")
	FName VarName;

	/** Channel variable type (synced from the DataChannel asset). */
	UPROPERTY(VisibleAnywhere, Category = "NDC")
	ENDCVariableType Type = ENDCVariableType::Unsupported;

	/** Where the written value comes from. */
	UPROPERTY(EditAnywhere, Category = "NDC")
	ENDCValueSource Source = ENDCValueSource::Constant;

	/**
	 * Name of the value function on the owning object when Source == Function. Valid signatures:
	 * `T Func()` or `T Func(FEventData EventData)`, with T matching
	 * Type and FEventData the owner's declared FNDCBinder::EventDataType. May be a
	 * BlueprintNativeEvent so Blueprint children can override the default.
	 */
	UPROPERTY(EditAnywhere, Category = "NDC")
	FName BoundFunction;

	/**
	 * Field of the writer's EventDataType read for this row when Source == EventData. Kept alongside
	 * BoundFunction rather than sharing one name field, so switching a row between the two and back
	 * does not lose what it was bound to.
	 */
	UPROPERTY(EditAnywhere, Category = "NDC")
	FName BoundEventDataField;

	/** Underlying enum of an Enum-typed channel variable (synced from the channel; drives the value dropdown). */
	UPROPERTY(VisibleAnywhere, Category = "NDC")
	TObjectPtr<UEnum> EnumDef = nullptr;

	//~ Constant values — only the field matching Type is shown by the details customization.
	UPROPERTY(EditAnywhere, Category = "NDC")
	bool BoolValue = false;

	UPROPERTY(EditAnywhere, Category = "NDC")
	int32 IntValue = 0;

	UPROPERTY(EditAnywhere, Category = "NDC")
	double FloatValue = 0.0;

	UPROPERTY(EditAnywhere, Category = "NDC")
	FVector2D Vector2DValue = FVector2D::ZeroVector;

	/** Also holds the Position constant. */
	UPROPERTY(EditAnywhere, Category = "NDC")
	FVector VectorValue = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, Category = "NDC")
	FVector4 Vector4Value = FVector4::Zero();

	UPROPERTY(EditAnywhere, Category = "NDC")
	FQuat QuatValue = FQuat::Identity;

	UPROPERTY(EditAnywhere, Category = "NDC")
	FLinearColor ColorValue = FLinearColor::White;

	UPROPERTY(EditAnywhere, Category = "NDC")
	uint8 EnumValue = 0;

	/**
	 * True when this row writes something the author picked, rather than the constant sitting on it.
	 *
	 * Source alone does not answer it: a row can name a source with nothing picked yet, which is the
	 * unfinished state the validator reports on its own terms. Both halves, or neither.
	 */
	bool IsBound() const;

	/**
	 * True when the constant on this row is not the one it was born with.
	 *
	 * Read as "what this row would put in the channel differs from what an untouched row of its type
	 * would", which is why it compares the same field per type that the write does, and why an exact
	 * comparison is the right one: typing the default back in leaves a row that writes what a fresh
	 * one writes, and nothing is lost by treating it as untouched.
	 */
	bool HasAuthoredConstant() const;

	/**
	 * True when someone put something into this row: a binding, or a constant they changed.
	 *
	 * This is what decides whether a row that has lost its channel variable is kept and reported, or
	 * dropped in silence. Asked by the sync that drops rows, by the validator that reports them and by
	 * the panel that greys them, so that all three are answering one question.
	 *
	 * The panel gives every channel variable a row whether or not anyone wanted one, so most rows in
	 * existence hold neither. Those are what this exists to let go of.
	 */
	bool HasAuthoredContent() const;

	/**
	 * Builds a function-sourced binding row (used for native default payloads). InEnumDef only drives
	 * the editor value dropdown of an Enum row and is otherwise refreshed from the channel on sync,
	 * so it is worth passing only to get the dropdown before the panel is first opened.
	 */
	static FNDCVariableBinding MakeFunctionBinding(FName InVarName, ENDCVariableType InType, FName InFunction, UEnum* InEnumDef = nullptr);

	/** The same, sourced from a field of the writer's EventDataType instead of a function. */
	static FNDCVariableBinding MakeEventDataBinding(FName InVarName, ENDCVariableType InType, FName InEventDataField, UEnum* InEnumDef = nullptr);

	/** Maps a Niagara variable type to a binding type (Unsupported when there is no Write* overload for it). */
	static ENDCVariableType VariableTypeFromNiagaraType(const FNiagaraTypeDefinition& TypeDef);

	/**
	 * The inverse: the Niagara type a row of this kind is written as.
	 *
	 * Taken from UNiagaraDataChannelWriter's own Write* overloads, one for one, because this is the
	 * type a channel's layout is keyed by — a mismatch is a row that silently writes nothing. Enum
	 * goes in as an int, exactly as WriteEnum forwards to WriteInt; Position is its own type and not a
	 * vector, which is the whole reason the writer keeps the two apart.
	 */
	static FNiagaraTypeDefinition NiagaraTypeFromVariableType(ENDCVariableType Type);
};

/**
 * One field of the access context, computed per write by a function on the owning object.
 *
 * The context is authored as a value in the details panel (FNDCBinder::DefaultAccessContext);
 * a row here overrides one of its fields for the duration of a write. Rows are sparse — a field only
 * has one once something is bound to it, and unbinding removes it again — so an unbound field costs
 * nothing at all, not even a skipped iteration.
 *
 * This is what replaced binding the context as a whole. A whole-context function had to know the
 * context type, construct or fill one, and get every field right including the ones it did not care
 * about; a field row states the one thing that actually varies and leaves the rest to the authored
 * value. It is also the only shape a Blueprint can express purely, which the fill shape could not.
 */
USTRUCT(BlueprintInternalUseOnly)
struct NDCBINDER_API FNDCContextBinding
{
	GENERATED_BODY()

	/** Bound function resolved for the owning class (transient; see FNDCBoundFunctionCache). */
	FNDCBoundFunctionCache FunctionCache;

	/** FieldName resolved against the access context type (transient; see FNDCFieldCache). */
	FNDCFieldCache FieldCache;

	/** BoundEventDataField resolved against the writer's EventDataType (transient; see FNDCFieldCache). */
	FNDCFieldCache EventFieldCache;

	/**
	 * EnableFlagField resolved against the access context type (transient; see FNDCFieldCache).
	 *
	 * Holds the flag only when the write is the thing that may set it — a Transient bool, which has no
	 * checkbox and so no author. Every other answer (no flag, not a bool, authorable) is remembered as
	 * null, which folds the whole question into one pointer test on the write path.
	 */
	FNDCFieldCache TransientFlagCache;

	/**
	 * The context field this row drives, named as it is declared. Only fields Niagara marks with
	 * NDCAccessContextInput are offered: the rest are the channel's answers back to the caller, and
	 * writing them would be talking over it.
	 */
	UPROPERTY(VisibleAnywhere, Category = "NDC")
	FName FieldName;

	/**
	 * Where this row's value comes from. Never Constant: a row exists only because something is bound
	 * to its field, and the constant is the value authored on the context itself.
	 */
	UPROPERTY(EditAnywhere, Category = "NDC")
	ENDCValueSource Source = ENDCValueSource::Function;

	/**
	 * Function on the owning object returning this field's type: `T Func()` or
	 * `T Func(FEventData EventData)`. Same gate as every other binding — const or Blueprint-pure, not
	 * replicated, not editor-only.
	 */
	UPROPERTY(EditAnywhere, Category = "NDC")
	FName BoundFunction;

	/** Field of the writer's EventDataType read for this row when Source == EventData. */
	UPROPERTY(EditAnywhere, Category = "NDC")
	FName BoundEventDataField;

	/**
	 * A null answer declines the whole write instead of leaving the authored value standing.
	 *
	 * This is how "there is nothing to attach to here, skip it" is said, and it is the reason the
	 * whole-context binding could be removed without losing anything: three of the four cues that
	 * used one used it for exactly this.
	 *
	 * Object-valued fields only. A vector or a bool has no value that means "no answer", so nothing
	 * here could tell a deliberate zero from a failure to compute one; the editor offers the checkbox
	 * only where it has meaning.
	 *
	 * Left off, a null answer means "no opinion" and the authored value stands untouched — which is
	 * how a per-surface system getter falls back to whatever the panel says without every caller
	 * having to spell out the fallback.
	 */
	UPROPERTY(EditAnywhere, Category = "NDC")
	bool bRequired = false;

	/**
	 * The bool field this row's field is gated behind, or None.
	 *
	 * Some context fields are inert until a separate bool says to use them — SystemToSpawn does
	 * nothing until bOverrideSystemToSpawn is set, Location nothing until bOverrideLocation is.
	 *
	 * The write sets it only when it is Transient, i.e. when it cannot be authored at all: such a
	 * flag has no checkbox in the panel, so the write is the only thing that could ever set it. An
	 * authorable one is the author's switch and is left exactly as they set it — the editor marks a
	 * row bound against a clear gate rather than overruling the checkbox, because a switch that
	 * silently does not mean what it shows is worse than one that is simply off.
	 *
	 * Stored rather than re-read, because the metadata half of the resolution is editor-only. Kept
	 * current by SyncBindingsWithChannel, so a row authored before the pairing was understood, or
	 * against a context that has since changed, picks up the right flag.
	 */
	UPROPERTY(VisibleAnywhere, Category = "NDC")
	FName EnableFlagField;

	/** Builds a function-sourced context row (used for native defaults, as MakeFunctionBinding is). */
	static FNDCContextBinding Make(FName InFieldName, FName InFunction, bool bInRequired = false, FName InEnableFlagField = NAME_None);

	/** The same, sourced from a field of the writer's EventDataType instead of a function. */
	static FNDCContextBinding MakeFromEventData(FName InFieldName, FName InEventDataField, bool bInRequired = false, FName InEnableFlagField = NAME_None);
};

/**
 * One open write: the destination buffer it is filling, and the channel's write guard it holds.
 *
 * The writer needs to own this rather than borrow Niagara's shared UNiagaraDataChannelWriter, because
 * that object keeps its buffer private and only offers writes by name — and a write by name costs a
 * walk of the channel's variables per row (FNiagaraDataChannelGameData::FindVariableBuffer). Owning
 * the buffer is what makes it possible to resolve each row to an index once and write through that.
 *
 * Non-copyable and scope-bound: it holds a slice of the channel's buffer and a place on the write
 * guard's stack, and both end when it does. Declare it in the frame that writes, never store it.
 */
struct NDCBINDER_API FNDCWriteScope : public FNDCWriterBase
{
	FNDCWriteScope() = default;
	~FNDCWriteScope();

	FNDCWriteScope(const FNDCWriteScope&) = delete;
	FNDCWriteScope& operator=(const FNDCWriteScope&) = delete;

	/** True between a successful BeginWrite and the End that releases it. */
	bool IsWriting() const { return bGuardHeld; }

	/** Releases the buffer and the guard. Idempotent; the destructor calls it. */
	void End();

	//~ FNDCWriterBase keeps these protected for its macro-generated subclasses to reach. This writer
	//~ builds its rows at runtime instead, so it reaches them the same way, one level out.
	const FNiagaraDataChannelGameDataPtr& GetData() const { return Data; }
	int32 GetStartIndex() const { return StartIndex; }
	int32 GetCount() const { return Count; }

private:
	friend struct FNDCBinder;

	/** Whether this scope currently occupies a slot on the write guard's stack. */
	bool bGuardHeld = false;
};

/**
 * A Niagara Data Channel write, bound to data rather than built in a graph: reflects an NDC's own
 * inputs — its channel variables and its access context alike — and fills each from a constant, a
 * field of the event data, or a bound function on the owning object. What it holds is bindings, which
 * is what the name says. Embed as a UPROPERTY in any cue, ability, component or actor that pushes
 * data to an NDC:
 *
 *   UPROPERTY(EditDefaultsOnly, Category = "NDC") FNDCBinder NDCBinder;
 *
 *   // In the owner's constructor — the struct bound functions take as their only parameter.
 *   NDCBinder.InitEventDataType(FMyEventData::StaticStruct());
 *
 *   NDCBinder.WriteToChannel(World, this, FConstStructView::Make(MyEventData));
 *
 * The writer names no context type of its own, which is what keeps it framework-agnostic: a
 * GameplayCue passes FGameplayCueParameters, an ability its own payload struct, a component
 * nothing at all.
 *
 * The details panel lists the channel's variables with a per-variable choice of constant value,
 * event data field or bound function. Rows added for variables that no longer exist are kept (never
 * destructive) and skipped at write time.
 */
USTRUCT(BlueprintType, meta = (DisableSplitPin,
	HasNativeMake = "/Script/NDCBinder.NDCBinderLibrary.NoMakeNode",
	HasNativeBreak = "/Script/NDCBinder.NDCBinderLibrary.NoBreakNode"))
struct NDCBINDER_API FNDCBinder
{
	GENERATED_BODY()

	/**
	 * Struct type a bound function may take as its one parameter — the data about the event that
	 * triggered the write. The value for one write is handed to WriteToChannel as an FConstStructView.
	 * Left unset, bound functions take at most the target actor and the bind menu offers only those.
	 *
	 * A C++ owner declares it once in its constructor:
	 *   NDCBinder.InitEventDataType(FGameplayCueParameters::StaticStruct());
	 * A Blueprint-only owner has no constructor to do that, so it is editable per class in the
	 * details panel instead — EditDefaultsOnly, so on the class and never on an instance — which is
	 * also why it is a UPROPERTY rather than a plain member.
	 *
	 * Private, and reachable only through the two methods below, because it is a declaration about
	 * the owner rather than a value: it says what shape every bound function on this class has. A
	 * later change would redefine what each row means without touching a single row, and the panel
	 * and the validator would go on describing rows against a contract that had moved.
	 */
private:
	UPROPERTY(EditDefaultsOnly, Category = "NDC|Advanced")
	TObjectPtr<UScriptStruct> EventDataType = nullptr;

public:
	/** The struct a bound function takes as its one parameter, or null when none is declared. */
	UScriptStruct* GetEventDataType() const { return EventDataType; }

	//~ The details customization draws this row itself and needs its name. Resolved in here, where the
	//~ member is reachable, so GET_MEMBER_NAME_CHECKED still fails the build if the member is renamed.
	static FName GetEventDataTypePropertyName() { return GET_MEMBER_NAME_CHECKED(FNDCBinder, EventDataType); }

	/**
	 * Declares it. Belongs in the owning object's constructor and the ensure says so — a development
	 * net rather than a wall: it fires where the mistake is made, and compiles out of Shipping.
	 */
	void InitEventDataType(UScriptStruct* InType);

	/**
	 * The same with no questions asked, for a writer that no UObject owns: one a test builds, one the
	 * benchmark times, or the local copy the panel makes to ask "would this bind if the type were
	 * declared?". Named apart from InitEventDataType so the two never blur together in a diff — if
	 * this appears on a writer that lives on an object, that is the bug the ensure was looking for.
	 */
	void SetEventDataTypeUnchecked(UScriptStruct* InType) { EventDataType = InType; }

	/** Niagara Data Channel to write to. */
	UPROPERTY(EditAnywhere, Category = "NDC")
	TObjectPtr<UNiagaraDataChannelAsset> DataChannel;


	/**
	 * Write visibility flags. CPU/GPU reach Niagara emitters; Game reaches BP/C++ readers.
	 *
	 * The only part of a writer a graph may change, and the only part where that is safe: these are
	 * arguments to each write (FNDCWriteScope::BeginWrite takes all three), so a change takes
	 * effect on the next write and invalidates nothing — unlike a row, whose resolved function and
	 * buffer offset are cached against what it used to say.
	 *
	 * Worth knowing before turning one off from gameplay code: a GameplayCueNotify writes from its
	 * CDO, so the change applies to every instance of that cue class for the rest of the session and
	 * is neither saved nor reset. If the answer has to differ per write, a flag is not the mechanism.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NDC|Advanced")
	bool bVisibleToGame = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NDC|Advanced")
	bool bVisibleToCPU = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NDC|Advanced")
	bool bVisibleToGPU = true;

private:
	/**
	 * Per-channel-variable values — one row per variable of the DataChannel (synced by the editor).
	 *
	 * Private for the same reason EventDataType is: the rows are authored data, and every cache under
	 * them is keyed on what a row says. Rebinding one on a live object would leave those caches holding
	 * answers resolved for the name the row used to have — see FNDCBoundFunctionCache, whose key
	 * deliberately omits the row's own names because "the name is a member of the row that owns the
	 * cache and cannot change without the row changing".
	 */
	UPROPERTY(EditAnywhere, Category = "NDC")
	TArray<FNDCVariableBinding> Bindings;

public:
	/**
	 * The context every write starts from, authored field by field in the details panel.
	 *
	 * Its type is not a choice: the editor keeps it at whatever the assigned channel declares
	 * (UNiagaraDataChannel::GetAccessContextType), so the panel always shows that channel's own input
	 * fields and there is no type to pick wrong. Niagara marks those fields with NDCAccessContextInput
	 * and Unreal renders them natively, edit conditions and all.
	 *
	 * Anything constant about placement belongs here — attach flags, cell size, a fixed system to
	 * spawn. What has to be computed per write goes in ContextBindings, which run after this and see
	 * these values already in place.
	 */
	UPROPERTY(EditAnywhere, Category = "NDC|Access Context")
	FNDCAccessContextInst DefaultAccessContext;

private:
	/**
	 * Per-field overrides of DefaultAccessContext, one row per field that has to be computed per
	 * write. Sparse: a field with no row keeps its authored value.
	 *
	 * Authored from the bind button on the field's own row in the details panel, so the value editor
	 * and the binding for a field are the same row and there is no second list to keep in step.
	 */
	UPROPERTY(EditAnywhere, Category = "NDC|Access Context")
	TArray<FNDCContextBinding> ContextBindings;

public:
	/** The payload rows, in the channel's variable order. */
	const TArray<FNDCVariableBinding>& GetBindings() const { return Bindings; }

	/** The sparse context rows: a field with no row keeps its authored value. */
	const TArray<FNDCContextBinding>& GetContextBindings() const { return ContextBindings; }

	/**
	 * Declares the rows from the owning object's constructor, and the ensure says so — the same
	 * development net InitEventDataType uses, for the same reason.
	 *
	 * Nothing calls these today: every cue in this project authors its rows on its Blueprint asset,
	 * which is where rows belong. They are the door a future native cue would come through, so that
	 * it comes through a door and not through the escape below.
	 */
	void InitBindings(TArray<FNDCVariableBinding> InRows);
	void InitContextBindings(TArray<FNDCContextBinding> InRows);

	/**
	 * Mutable rows on a writer that no UObject owns: one a test builds, one the benchmark times.
	 *
	 * Be clear about what this is. It hands out the array itself, so it is exactly as permissive as the
	 * public member it replaced — what changed is that reaching for it is now a decision that shows up
	 * in a diff, next to a name that says the check was skipped. Used on a writer that lives on an
	 * object, it is the bug InitBindings' ensure exists to catch.
	 */
	TArray<FNDCVariableBinding>& GetMutableBindingsUnchecked() { return Bindings; }
	TArray<FNDCContextBinding>& GetMutableContextBindingsUnchecked() { return ContextBindings; }

	//~ The details customization builds child handles for these two arrays and needs their names.
	//~ Resolved in here, where the members are reachable, so a rename still fails the build.
	static FName GetBindingsPropertyName() { return GET_MEMBER_NAME_CHECKED(FNDCBinder, Bindings); }
	static FName GetContextBindingsPropertyName() { return GET_MEMBER_NAME_CHECKED(FNDCBinder, ContextBindings); }


	//~ The channel writer wants its debug source as an FString, and the owner's name does not change
	//~ between writes, so it is built once per owner rather than per write. Transient, like the
	//~ function caches.
	//~
	//~ Gone entirely in Shipping, because the label is: both of the things Niagara does with a
	//~ DebugSource — FNiagaraInsights::NotifyDataChannelWrite and the SourceString it hands its debug
	//~ HUD — are compiled out there (WITH_NIAGARA_INSIGHTS is UE_TRACE_ENABLED && !IS_PROGRAM &&
	//~ !UE_BUILD_SHIPPING, and the other sits under !UE_BUILD_SHIPPING). Handing it one anyway cost a
	//~ malloc and a copy on every write, since the scope that receives it is built fresh each time.
#if !UE_BUILD_SHIPPING
	mutable TWeakObjectPtr<const UObject> CachedDebugNameOwner;
	mutable FString CachedDebugName;
	mutable bool bDebugNameCached = false;
#endif

	//~ One buffer index per row, parallel to Bindings, and the channel layout they were resolved
	//~ against. Writing a row by name costs a walk of the channel's variables inside Niagara, per row
	//~ per write; the answer only changes when the channel's layout does, and the layout is what the
	//~ shared pointer here holds it against. Both members copy by value, so a copied writer carries a
	//~ cache that is either still correct or stops matching — the same bargain as every other cache
	//~ on this struct. Transient, like those; never serialized.
	mutable FNiagaraDataChannelLayoutInfoPtr CachedLayout;
	mutable TArray<int32> VarOffsets;

	/** Resolved runtime channel, or nullptr when DataChannel is unset. */
	UNiagaraDataChannel* GetChannel() const;

	/**
	 * The gate every bound function passes before its signature is considered, whatever it is bound
	 * to. Modelled on how the engine gates its own name-resolved read bindings
	 * (SPropertyBinding::ForEachBindableFunction, UE::MVVM::BindingHelper::IsValidForSourceBinding).
	 *
	 * Public so the editor picker, the binding diagnostics and the asset validator all judge a
	 * candidate by exactly the rule the write will apply — a binding that passes the menu and then
	 * fails at runtime is the failure mode worth designing out.
	 *
	 * One rule, no exceptions: every binding the writer has is a read — a payload value, a context
	 * field, the handler system — so every one of them must be const or Blueprint-pure. The
	 * whole-context binding used to be the exception, because filling a context by reference is a
	 * write; per-field rows return values instead, which is what let the exception go.
	 */
	static ENDCBindingRejection GetBindingRejection(const UFunction* Func);

	/** One line saying why, in the words the log and the editor tooltip both use. Empty for None. */
	static const TCHAR* DescribeRejection(ENDCBindingRejection Rejection);

	/**
	 * Forgets which binding warnings have already been said.
	 *
	 * A misconfigured row warns ONCE per owner class and binding, for the life of the process, because
	 * the alternative is a line every frame a weapon fires. That is right for a game and wrong for a
	 * test process: a test that asserts a warning was logged passes the first time the suite runs and
	 * fails the second, and reads as a regression rather than as the suppression it is.
	 *
	 * So the record can be emptied. The test module clears it before every test — see
	 * NDCBinderTestsModule — which is why no test has to remember to.
	 */
	static void ResetBindingWarnings();


	/**
	 * Prepares Context for one write, and says whether to write at all.
	 *
	 * Context is initialized to the channel's own type first, so passing the same one back on every
	 * write costs no allocation — that is what WriteToChannel does with the channel's scratch context.
	 * Pass a context of your own to keep what the channel writes back into it (SpawnedSystems).
	 *
	 *  - the authored DefaultAccessContext is copied in;
	 *  - every ContextBindings row then overrides its field with what its bound function answers;
	 *  - a required row answering null declines the write — false;
	 *  - a non-required row answering null leaves the authored value alone;
	 *  - a gated field's flag is set only when it is Transient and so could not have been authored.
	 *
	 * With no rows bound the context is exactly what the panel shows. The writer cannot read a
	 * position out of an arbitrary event data struct and will not invent one, so a channel that
	 * buckets spatially needs either a location authored on the default or a row bound to it.
	 */
	bool ResolveAccessContext(FNDCAccessContextInst& Context, const UObject* FunctionOwner, FConstStructView EventData) const;

	/**
	 * The context fields a row may target: the ones Niagara marks NDCAccessContextInput, supers
	 * first. Everything else on a context is its answer back to the caller.
	 *
	 * Shared by the details panel, the bind menu and the asset validator so all three agree on what
	 * is bindable, and on a null or non-context struct visits nothing.
	 */
	static void ForEachContextInputField(const UScriptStruct* ContextType, TFunctionRef<void(FProperty&)> Body);

	/**
	 * The bool input field Field is gated behind — its EditCondition, or the bOverride<Field>
	 * convention where there is none — or None.
	 *
	 * Editor-only, and asked once: a row stores the answer (FNDCContextBinding::EnableFlagField) when
	 * it is created or re-synced, because the question is answered from metadata and the write has to
	 * work in a cooked build. Whatever it returns is a bool this context declares as an input; a
	 * compound EditCondition ("bA && bB") names no single flag, so it resolves nothing of its own and
	 * takes the convention like a field that stated nothing.
	 */
#if WITH_EDITOR
	static FName ResolveContextFieldEnableFlag(const UScriptStruct* ContextType, const FProperty* Field);
#endif

	/** The context type of the assigned channel, or nullptr when there is no channel. */
	const UScriptStruct* GetContextType() const;

	/** The row driving Field, or nullptr when nothing is bound to it. */
	const FNDCContextBinding* FindContextBinding(FName FieldName) const;

	/**
	 * True when Func can drive Field: it passes the gate, its return value is assignable to the
	 * field, and its signature is the standard `T Func()` / `T Func(FEventData)`.
	 */
	bool IsValidContextFieldFunction(const UFunction* Func, const FProperty* Field) const;

	/**
	 * Applies one row to a context already of the right type: calls its bound function and stores the
	 * answer in its field, switching on the field's enable flag when there is one.
	 *
	 * Returns false only when the row DECLINED the write — it is required and the answer was null.
	 * A row that merely cannot run (no such field on this context, a function that is missing or
	 * unusable) warns once and returns true, leaving the authored value standing: a broken binding
	 * must not be able to turn a write off, or a typo in a function name would silently stop an
	 * effect from ever playing and look like a content bug.
	 */
	bool ApplyContextBinding(const FNDCContextBinding& Binding, FNDCAccessContextInst& Context, const UObject* FunctionOwner, FConstStructView EventData) const;

	/**
	 * True when a value of Source's type can be stored into a context field. The type half of
	 * IsValidContextFieldFunction, shared so a function's return value and an event data field are
	 * judged by exactly the rule the write will apply.
	 */
	static bool CanAssignPropertyToField(const FProperty* Source, const FProperty* Field);

	/**
	 * The classes an object field will actually accept, from its AllowedClasses metadata. Empty when
	 * the field carries no such restriction, or is not an object field at all.
	 *
	 * A field's declared type is not always the type it wants. FNDCAccessContext::SystemToSpawn is a
	 * TObjectPtr<UObject> because it takes either a NiagaraSystem or a NiagaraSystemCollection, two
	 * types with no common ancestor below UObject, and says so in metadata that only the property
	 * editor's asset picker reads. Left at its declared type, a bind menu offers every object getter
	 * on the class for a field that can use two of them.
	 *
	 * Metadata is editor-only data, so this is empty in a cooked build — which is why it is not part
	 * of the write. The write stores by type in every configuration, and the editor is what keeps a
	 * binding the write could not use from being made in the first place.
	 */
	static void GetFieldAllowedClasses(const FProperty* Field, TArray<const UClass*>& OutClasses);

	/**
	 * True when Source's type satisfies the field's AllowedClasses restriction, if it has one.
	 *
	 * Assignable in one direction only: the source must be one of the allowed classes or a subclass of
	 * one. A getter returning the field's declared UObject might well hand back a NiagaraSystem at
	 * runtime, but nothing says it will, and offering it puts the designer one silent no-op away from
	 * a cue that does nothing. Declaring the getter's return type is the cheaper half of that trade.
	 *
	 * Always true in a cooked build; see GetFieldAllowedClasses.
	 */
	static bool IsSourceAllowedByFieldClasses(const FProperty* Source, const FProperty* Field);

	/** True when a row on Field can meaningfully be marked Required — i.e. the field is object-valued. */
	static bool CanContextFieldBeRequired(const FProperty* Field);

	/**
	 * True when Func can provide a value of Type: the return param matches the type and the signature
	 * is `T Func()` or, when EventDataType is set, `T Func(FEventData EventData)` with FEventData ==
	 * EventDataType.
	 */
	bool IsValidValueFunction(const UFunction* Func, ENDCVariableType Type, const UEnum* WantedEnum = nullptr) const;

	/**
	 * The event data parameter a bound function declares, and whether it is shaped like a binding at
	 * all. False when the parameter list is not one a binding can be called through: more than one
	 * parameter, a non-struct one, or a mutable reference. OutEventDataStruct is then untouched.
	 *
	 * On success OutEventDataStruct is the struct the function takes, or null when it takes none —
	 * `T Func()` is a legal shape for every writer, whatever its EventDataType.
	 *
	 * Public because the editor asks the same question to name the one edit that would revive a
	 * rejected binding ("declare that struct as Event Data Type"), and a second implementation of
	 * this walk would offer fixes the write path still refuses. It had one, and it did.
	 */
	static bool GetBoundFunctionEventDataStruct(const UFunction* Func, UScriptStruct*& OutEventDataStruct);

	/**
	 * How Parm is passed, from its flags alone.
	 *
	 * One rule with three readers, which is why it is here rather than spelled out at each of them:
	 * the call path aliases exactly ByConstReference and deep-copies the rest, the signature check
	 * above refuses exactly ByMutableReference, and the editor test asserts that the parameter it
	 * generates comes out ByConstReference. Written as three separate flag expressions, the first two
	 * agreed only by inspection and the third could not be written at all.
	 */
	static ENDCEventDataPassing GetEventDataPassing(const FProperty* Parm);

	/**
	 * True when a value of Prop's type can be written to a channel variable of Type. The type half of
	 * IsValidValueFunction, shared so a function's return value and an event data field are judged by
	 * exactly the same rule.
	 *
	 * WantedEnum is the channel variable's own enum (FNDCVariableBinding::EnumDef) and means something
	 * only for an Enum row. Given one, a property carrying a DIFFERENT enum is refused: two unrelated
	 * enumerations that happen to share an integer are the one confusion this type can suffer, and
	 * nothing downstream would notice — the value goes to the channel as a plain int either way.
	 *
	 * A property carrying no enum at all — a bare uint8 — is still accepted, deliberately. The channel
	 * stores an int (WriteEnum forwards to WriteInt), so a raw byte is a meaningful value here, and a
	 * getter that returns one is an author saying which index they mean. That is the difference from a
	 * context field, where CanAssignPropertyToField refuses an untyped byte outright: there the enum is
	 * the destination's type, here it is a hint about an int.
	 */
	static bool IsPropertyCompatibleWithVariableType(const FProperty* Prop, ENDCVariableType Type, const UEnum* WantedEnum = nullptr);

	/**
	 * The fields of EventDataType a row may read, in declaration order.
	 *
	 * There is no gate to speak of: reading a struct member cannot call anything, cannot mutate the
	 * owner and cannot fail, so none of the reasons a function is refused apply. Only what would not
	 * survive cooking is left out.
	 */
	void ForEachEventDataField(TFunctionRef<void(FProperty&)> Body) const;

	/**
	 * The field of EventDataType a path names, or nullptr — including when no EventDataType is declared.
	 * A path with no dot in it is one segment, which is what every binding written before paths existed
	 * is, so nothing needs migrating.
	 */
	const FProperty* FindEventDataField(FName FieldPath) const;

	/** How many segments a bound path may have. Deep enough for the structs event data is made of. */
	static constexpr int32 MaxFieldPathDepth = 4;

	/**
	 * Resolves a dotted path ("Hit.ImpactPoint") against a struct into the leaf property and the byte
	 * offset of its value from the start of that struct.
	 *
	 * STRUCT MEMBERS ONLY, and that is the design rather than a limitation to lift later. A path made
	 * of struct members is a fixed sum of offsets, so it collapses here, once, and the write pays the
	 * same single addition a direct member costs however deep the path goes. An object pointer in the
	 * middle would not collapse — it has to be loaded and null-checked every write — and a container
	 * would need an index the path does not carry. Both of those are what a bound function is for.
	 *
	 * Editor-only and deprecated members are refused at EVERY segment, not just the leaf: a path
	 * through one resolves in the editor and reads nothing in a cooked build.
	 */
	static bool ResolveFieldPath(const UStruct* Root, FName FieldPath, const FProperty*& OutLeaf, int32& OutOffset);

	/**
	 * Full write pipeline: builds the access context (DefaultAccessContext, then ContextBindings) and
	 * writes every binding as one element. False on failure, or when a required context row declined
	 * the write — callers usually treat that as "not handled".
	 */
	bool WriteToChannel(UWorld* World, const UObject* FunctionOwner, FConstStructView EventData) const;

	/**
	 * The same pipeline, emitting ONE ELEMENT PER EVENT DATA in a single write — a shotgun's pellets, a
	 * chain of hits, anything a caller has all of at once.
	 *
	 * Worth reaching for, because most of a write is paid per write and not per element: the world and
	 * handler lookups, Niagara's FindData, the buffer growth, and this writer's own context pass. Eight
	 * elements this way measured 2.5x cheaper than eight single writes, and the context rows — whose
	 * bound functions run once here instead of once per element — are a further saving on top of that
	 * figure.
	 *
	 * THE ROWS DO NOT CHANGE. A writer authored for one element emits many without re-authoring: each
	 * element's rows are evaluated against its own event data, so a row reading an event data field or
	 * calling a bound function gets that element's value.
	 *
	 * WHAT IS SHARED IS THE ACCESS CONTEXT, resolved once from the FIRST element. Niagara takes one per
	 * write, so a batch is by definition a set of elements that agree about where the write goes, what
	 * it attaches to and which handler system it spawns. Elements that disagree about any of those are
	 * separate writes, not a batch — and a required context row answering null declines all of them
	 * together, not one.
	 *
	 * An empty array writes nothing and returns false.
	 */
	bool WriteToChannel(UWorld* World, const UObject* FunctionOwner, TConstArrayView<FConstStructView> EventData) const;

	/**
	 * Low-level write with an already-built AccessContext: creates a single-element writer on the
	 * channel and writes every binding at index 0. Function-sourced bindings are resolved on
	 * FunctionOwner (usually the owning cue/ability CDO), whose name is also the writer's debug
	 * source. An invalid AccessContext skips the write. Returns the writer, nullptr on failure/skip.
	 *
	 * AccessContext is taken by mutable reference because the channel writes back through it: after
	 * the call it carries the handler systems this write spawned or joined (SpawnedSystems), and on a
	 * GameplayBurst channel whether they ended up attached. Use this two-step form instead of
	 * WriteToChannel when you want to reach them.
	 */
	bool WriteWithContext(UWorld* World, FNDCAccessContextInst& AccessContext, const UObject* FunctionOwner, FConstStructView EventData) const;

	/**
	 * Opens a Count-element write on the channel without writing anything, so a caller emitting
	 * several elements from one payload (a pellet spread, a chain of hits) can drive WriteBindings
	 * per index and add its own per-element values on top. False on failure, or on an invalid
	 * AccessContext. AccessContext is written back the same way as in WriteWithContext.
	 *
	 * The scope holds the destination buffer and the channel's write guard until it is destroyed, so
	 * declare it in the frame that does the writing and let it end there.
	 *
	 * WHY THIS IS HERE WITH NO CALLER IN THE GAME. It is worth 2.5x on eight elements — everything a
	 * write pays before the first value moves is paid once instead of eight times, measured — but the
	 * saving is only available to a caller holding all N payloads at once, and a GameplayCue is
	 * executed once per hit with no way to carry an array of them. Nothing in this project is shaped
	 * to use it yet; a shotgun's pellets written from the ability that fired them would be. Kept
	 * rather than removed because the measurement says what it is worth and the end-to-end test keeps
	 * it honest: NDCBinder.Write.EndToEnd drives exactly this pair, two elements with different event
	 * data, and is the only thing that proves the per-element form carries it.
	 */
	bool BeginWrite(FNDCWriteScope& Scope, UWorld* World, FNDCAccessContextInst& AccessContext, int32 Count, const UObject* DebugNameOwner) const;

	/** Writes every binding at Index into an open scope. Rows whose variable the channel no longer has are skipped. */
	void WriteBindings(FNDCWriteScope& Scope, int32 Index, const UObject* FunctionOwner, FConstStructView EventData) const;

	/**
	 * Fills VarOffsets with one buffer index per row, resolved against the channel's current layout.
	 *
	 * A row's own type is checked against the layout's, exactly as FindVariableBuffer does per write —
	 * the same decision, made once — and a row the layout has no place for keeps INDEX_NONE and is
	 * skipped. Driven by BeginWrite whenever the cached layout stops matching the channel's; public
	 * because the equivalence it claims is worth being able to check against FindVariableBuffer
	 * directly, which is what the test for it does.
	 */
	void ResolveVarOffsets(const FNiagaraDataChannelLayoutInfoPtr& Layout) const;

#if WITH_EDITOR
	/**
	 * Reports every row of this writer that cannot run, as issues on a data validation context.
	 *
	 * Nothing is asked of the class that owns a writer: the editor module raises these by itself, two
	 * ways — a Blueprint compiler extension, which is what makes a broken row FAIL THE COMPILE of the
	 * asset carrying it, and an asset validator, which covers Save, the submit dialog and the
	 * DataValidation commandlet. This is the one implementation both of them call.
	 *
	 * OwnerClass is what bound function names are resolved against. It is a class rather than the
	 * object holding the writer because the two callers disagree about which class that is: the
	 * validator asks the object itself (a component template resolves against the COMPONENT, not the
	 * actor above it), while a compile has to ask the skeleton class, since the values it reads come
	 * from a copy of the class made before the compile started. WriterName is the writer's property
	 * path on whatever holds it ("NDCBinder", "Muzzle.NDCBinder") and only names it in the messages.
	 *
	 * An error means drift: a row names a function, an event data field or a context field that is
	 * gone or no longer fits, and nothing at write time will say so. A warning means unfinished: no
	 * channel picked yet, or a row set to read something with nothing chosen. Only the first is worth
	 * failing a compile over; see NDCBinderValidation.cpp.
	 *
	 * A consumer may also call this from its own UObject::IsDataValid, but it does not need to and
	 * usually should not: the compiler runs IsDataValid as well, so every row would be reported twice.
	 */
	EDataValidationResult ValidateBindings(const UClass* OwnerClass, const FString& WriterName, FDataValidationContext& Context) const;

	/**
	 * Ensures one binding row per channel variable: adds missing rows (as constants) and refreshes
	 * types/enum defs of existing rows. Reorders rows to the channel's variable order; rows for
	 * variables that no longer exist keep their data at the end. Also retypes DefaultAccessContext to
	 * the channel's context type. Returns true when anything changed.
	 *
	 * Context rows are not reconciled as a set: they are sparse and authored one at a time, so a row
	 * whose field the new context type does not have is kept, reported stale, and skipped at write
	 * time, the same as a payload row for a departed variable. Each row's EnableFlagField is
	 * re-resolved, though, so it never goes stale against the context it is being written into.
	 */
	bool SyncBindingsWithChannel();

	/**
	 * Removes every row this channel has no place for: a payload row whose variable is gone, and an
	 * access context row whose field the context type does not have. Returns true when anything was
	 * removed.
	 *
	 * Run automatically on an explicit DataChannel switch, where the old rows' values are meaningless
	 * for the new channel. Removing a variable from the SAME channel does NOT prune: the row is kept,
	 * shown stale and skipped at write time, because an author who deletes a variable by accident
	 * should find their row waiting when they put it back. The panel offers this as a button once
	 * there is something to remove, which is how that keeping stays a choice rather than a dead end.
	 */
	bool PruneStaleBindings();

	//~ Dry runs of the two above. A details panel syncs on every refresh, so it needs to know whether
	//~ there is a real edit before announcing one: an unmatched NotifyPreChange leaves the owning
	//~ object mid-edit (an actor's components stay unregistered, waiting for the PostEditChange).
	bool NeedsBindingSync() const;
	bool HasStaleBindings() const;

	/**
	 * How many rows PruneStaleBindings would remove, which is what a channel SWITCH drops: every row
	 * the new channel has no place for, bound or not, payload or context.
	 *
	 * Not the count the panel shows. That one is the compiler's condition — a stale row with a binding
	 * on it — because the line it labels claims a compile failure. See IsBindingStale.
	 */
	int32 CountStaleBindings() const;

	/**
	 * The names of the assigned channel's variables, empty when there is no channel.
	 *
	 * Gathered once and handed to IsBindingStale per row, so that the rule behind a greyed row in the
	 * panel, the error the compiler raises and the removal PruneStaleBindings performs are the same
	 * sentence rather than three readings of it.
	 */
	TSet<FName> GetChannelVariableNames() const;

	/**
	 * True when the channel has no variable this row could write to.
	 *
	 * False for an empty name set, that being the answer to "judged against what?" — an unassigned
	 * channel reads back no names, and without this every row would go stale the moment the channel
	 * was cleared. The panel and the validator both ask through here so they cannot disagree about
	 * that; the removal paths guard on the channel pointer instead, being reached only with one.
	 */
	static bool IsBindingStale(const FNDCVariableBinding& Row, const TSet<FName>& ChannelVariableNames);
#endif

private:
	/**
	 * The body of a write, without the re-entrancy guard: creates the writer and fills the rows.
	 * WriteToChannel arms the guard earlier than WriteWithContext can — it borrows the channel's
	 * shared scratch context before any author code runs — so it needs a form that does not re-check.
	 */
	bool WriteContextAndBindings(UWorld* World, FNDCAccessContextInst& AccessContext, const UObject* FunctionOwner, TConstArrayView<FConstStructView> EventData) const;

	//~ The unchecked forms of the two public entry points below them. A write that already holds the
	//~ channel's scope has answered every question those forms ask, and one of the answers — "is this
	//~ channel already being written" — would come back yes and refuse the write that asked it.
	bool BeginWriteInternal(FNDCWriteScope& Scope, UWorld* World, FNDCAccessContextInst& AccessContext, int32 Count, const UObject* DebugNameOwner) const;
	void WriteBindingsInternal(FNDCWriteScope& Scope, int32 Index, const UObject* FunctionOwner, FConstStructView EventData) const;



};
