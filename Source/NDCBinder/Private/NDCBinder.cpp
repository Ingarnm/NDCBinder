// NDCBinder.cpp

#include "NDCBinder.h"

#include "NiagaraDataChannelAsset.h"
#include "NiagaraDataChannel.h"
#include "NiagaraDataChannelVariable.h"
#include "NiagaraDataChannel_GameplayBurst.h"
#include "NiagaraDataChannelAccessContext.h"
#include "NiagaraDataChannelAccessor.h"
#include "NiagaraDataChannelFunctionLibrary.h"
#include "NiagaraTypes.h"
#include "NiagaraDataChannelGameData.h"
#include "NiagaraDataChannelLayoutInfo.h"
#include "NiagaraSystem.h"

#include "Misc/ScopeLock.h"
#include "UObject/UObjectThreadContext.h"

DEFINE_LOG_CATEGORY_STATIC(LogNDCBinder, Log, All);

// WHAT THE TWO CACHES ON A ROW CAN REMEMBER, AND WHY
//
// FNDCBoundFunctionCache and FNDCFieldCache are declared in the header because the write path reads
// them inline. Their reasoning is here, because none of it is about what they are: it is about what
// can happen to a UClass, a UFunction and an FProperty between two writes.
//
// THE KEY IS EVERY INPUT TO THE ANSWER. Looking a name up and re-checking a whole signature on every
// write repeats work whose inputs do not change — the owner's class, the name being resolved, the
// writer's EventDataType, the channel variable's type and enum, and for a row that targets a struct
// field, the struct that field belongs to. All of them are in the key, so rebinding a row, retyping a
// channel variable or editing Event Data Type makes the entry stop matching by construction. Nothing
// has to remember to invalidate anything.
//
// WHAT A KEY CANNOT EXPRESS IS A BLUEPRINT RECOMPILE, because that does not produce a new UClass:
// FKismetCompilerContext::CleanAndSanitizeClass cleans the class in place, and every UFunction on it
// is emptied of bytecode, has its parameter properties destroyed and is re-outered into a TRASHCLASS.
// The class pointer, the name and the types all still match, so a remembered function would be called
// with a stale ParmsSize and a destroyed parameter list. Asking whether the remembered function is
// still owned by the class it was remembered for is what catches that, and it needs no delegate to
// invalidate it: a trashed function is no longer owned by the class.
//
// AN FPROPERTY CANNOT BE CHECKED THAT WAY. A native struct's properties are built once at startup and
// live as long as the process; a user defined struct's are destroyed and rebuilt by a recompile, and
// an FProperty is an FField, deleted outright, leaving a dangling pointer with nothing to test it
// against. So FNDCFieldCache does not remember a Blueprint struct at all in the editor. There is no
// recompile outside the editor, so that check compiles away in the builds this exists to make fast.
//
// A REMEMBERED MISS IS NOT TRUSTED IN THE EDITOR. The function that was not there can be added to a
// Blueprint and compiled in while the cache lives on — the class is the same object, so nothing in
// the key notices — and a row that stays dead until the asset is reloaded is a worse surprise than
// one lookup per write in a build that is already paying for the ensure below. A cooked build cannot
// grow a function, so it keeps its miss. FNDCFieldCache needs no equivalent: it already refuses to
// remember anything about a non-native struct in the editor, and a native one cannot gain a field
// without a rebuild.
//
// THE ENSURE IS THE NET UNDER ALL OF IT. Everything above is reasoning about when a remembered answer
// can still be trusted, and reasoning is exactly what was wrong about this cache once already — it
// assumed a Blueprint recompile produced a new UClass, which it does not. So in the editor the answer
// is checked against the lookup it replaced, and any future hole in the reasoning surfaces as a loud
// ensure the first time it is hit rather than as a stale call. A cached miss is not checked: it can
// legitimately mean the function exists but was rejected, which a bare lookup cannot tell apart.
//
// THE ENUM IN THE KEY IS RAW, COMPARED AND NEVER DEREFERENCED — in both caches. The weak pointer it
// used to be cost 2 ns on an event data row and 6 on a function one, a GUObjectArray index and a
// serial-number check on a compare that never needs the object. What makes raw safe is the other
// side: the enum is only ever compared against the row's own EnumDef, a UPROPERTY hard reference, so
// a match means a live object by construction — and in a cooked build a row's EnumDef cannot change
// at all. In the editor it can, on a channel sync, and the old enum may then be collected and its
// slot reused. So the editor keeps a weak twin and compares both: a recycled address matches the raw
// pointer and fails the weak one, which is the whole hazard, caught where it can happen and paid for
// only there.
//
// Neither cache is a UPROPERTY: transient runtime state, never serialized, game thread only.

namespace NDCBinderPrivate
{
	static FCriticalSection WarnedLock;
	//~ The three names themselves rather than a hash of them: a 32-bit collision would silence
	//~ someone else's warning for the rest of the process, and nothing would ever say which.
	//~
	//~ At namespace scope rather than inside WarnOnce so that FNDCBinder::ResetBindingWarnings
	//~ can empty it; see there for why anything would want to.
	static TSet<TTuple<FName, FName, FName>> Warned;

	/** One warn per (owner class, function, variable) — bad bindings fire per shot otherwise. */
	static void WarnOnce(const UObject* Owner, FName FuncName, FName VarName, const TCHAR* Reason)
	{
		const TTuple<FName, FName, FName> Key(
			Owner ? Owner->GetClass()->GetFName() : NAME_None, FuncName, VarName);

		{
			FScopeLock Lock(&WarnedLock);
			bool bAlreadyWarned = false;
			Warned.Add(Key, &bAlreadyWarned);
			if (bAlreadyWarned)
			{
				return;
			}
		}

		UE_LOG(LogNDCBinder, Warning, TEXT("NDC binding '%s' on %s: %s (function '%s')."),
			*VarName.ToString(), *GetNameSafe(Owner), Reason, *FuncName.ToString());
	}

	/**
	 * The channels with a write in progress, and the scope that puts one there.
	 *
	 * Every reflected call a write makes runs author code, so a write can be re-entered — a bound
	 * getter that triggers another cue, say. What that endangers is per channel and not global:
	 * Niagara hands out one shared writer per channel handler (UNiagaraDataChannelHandler::
	 * GetDataChannelWriter) and one shared scratch access context per channel, so a nested write to
	 * the SAME channel would call BeginWrite on the very object the outer loop is still filling. A
	 * nested write to a DIFFERENT channel touches none of that and is allowed — an impact cue whose
	 * getter emits a decal write is an ordinary thing to author, and refusing it would be a silent
	 * loss with nothing to see.
	 *
	 * A fixed stack rather than a TArray: nesting is one or two deep in practice, this is on the write
	 * path, and running out is itself a reason to refuse — nesting this deep is a loop.
	 * Game thread only, which every caller has already checked by the time it gets here.
	 *
	 * ENTRIES, NOT WRITES. A one-shot write holds TWO of these slots: WriteToChannel and
	 * WriteWithContext each take a scope before resolving the access context — that resolve runs
	 * author code against the channel's single scratch context, so it has to be covered — and the
	 * BeginWriteInternal inside them takes another for the buffer slice, released by the write scope's
	 * End. So eight slots is four nested one-shot writes, not eight; a caller driving BeginWrite and
	 * End itself holds one. The refusal is unaffected either way — the channel is looked for anywhere
	 * on the stack, and both checks run before their own push — this is only what the depth counts.
	 */
	static constexpr int32 MaxNestedWrites = 8;
	static const UNiagaraDataChannel* WritingChannels[MaxNestedWrites] = {};
	static int32 NumWritingChannels = 0;

	/** True when this channel already has a write in flight, or the nesting is deep enough to be a bug. */
	static bool CannotWriteChannelNow(const UNiagaraDataChannel* Channel)
	{
		if (NumWritingChannels >= MaxNestedWrites)
		{
			return true;
		}
		for (int32 Index = 0; Index < NumWritingChannels; ++Index)
		{
			if (WritingChannels[Index] == Channel)
			{
				return true;
			}
		}
		return false;
	}

	/** Marks a channel as being written. Every push is matched by exactly one pop. */
	static void PushWritingChannel(const UNiagaraDataChannel* Channel)
	{
		if (ensure(NumWritingChannels < MaxNestedWrites))
		{
			WritingChannels[NumWritingChannels++] = Channel;
		}
	}

	static void PopWritingChannel()
	{
		if (ensure(NumWritingChannels > 0))
		{
			--NumWritingChannels;
		}
	}

	/**
	 * Holds a channel for the duration of a scope.
	 *
	 * WriteToChannel needs this before FNDCWriteScope exists: the shared thing it protects is
	 * the channel's scratch access context, which is already in use while ResolveAccessContext runs
	 * author code, well before there is a buffer to write into. The write scope then takes a place of
	 * its own, so one write of that shape occupies two — which is why the stack has room to spare.
	 */
	class FChannelWriteScope
	{
	public:
		explicit FChannelWriteScope(const UNiagaraDataChannel* Channel) { PushWritingChannel(Channel); }
		~FChannelWriteScope() { PopWritingChannel(); }
		FChannelWriteScope(const FChannelWriteScope&) = delete;
		FChannelWriteScope& operator=(const FChannelWriteScope&) = delete;
	};

	/**
	 * The most specific thing that can be said about a name that did not resolve to a usable
	 * function. Worth the extra branch on a path that only runs once per class: "not const" and
	 * "wrong signature" send an author to completely different places.
	 */
	static const TCHAR* DescribeBindingFailure(const UFunction* Func)
	{
		const ENDCBindingRejection Rejection = FNDCBinder::GetBindingRejection(Func);
		return Rejection != ENDCBindingRejection::None
			? FNDCBinder::DescribeRejection(Rejection)
			: TEXT("the function's signature or return type does not match the binding");
	}

	/**
	 * Visits the function's parameters (the return parm included) and nothing else.
	 * A Blueprint UFunction's property list continues past the parameters into its local variables,
	 * which live beyond ParmsSize — walking into them would read and write off the end of a parms
	 * block. Parameters always come first, so stopping at the first non-parm is the whole guard.
	 */
	template <typename BodyType>
	static void ForEachParm(const UFunction* Func, BodyType&& Body)
	{
		for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			Body(*It);
		}
	}

	/**
	 * The standard bound-function signature: `T Func()` or, when the owner declared an EventDataType,
	 * `T Func(FEventData EventData)`. One parameter at most, and it is the event data.
	 *
	 * There is deliberately no privileged actor parameter. Everything a bound function needs arrives
	 * through the one struct its own framework defines, which is what keeps the writer from having an
	 * opinion about what a "subject" is — a cue has a target, an ability has a caster, a component has
	 * neither.
	 */
	static bool HasBoundFunctionSignature(const UFunction* Func, const UScriptStruct* EventDataType)
	{
		UScriptStruct* Declared = nullptr;
		if (!FNDCBinder::GetBoundFunctionEventDataStruct(Func, Declared))
		{
			return false;
		}
		// Taking nothing fits any writer; taking a struct fits only the writer that declared it.
		return Declared == nullptr || Declared == EventDataType;
	}

	/**
	 * One reflected call on the owning object: allocates and initializes the parameter block, fills
	 * the event data param, calls the function, and destroys the block on scope exit (an event data
	 * struct may own heap data — arrays, tag containers — so the parms must be destroyed, not just
	 * freed).
	 *
	 * Constructing with a null function is a no-op, so a caller with an optional binding can declare
	 * the call unconditionally and test GetReturnValue().
	 */
	class FBoundFunctionCall
	{
	public:
		FBoundFunctionCall(const UObject* Owner, UFunction* InFunc, FConstStructView EventData)
		{
			if (!Owner || !InFunc)
			{
				return;
			}
			Func = InFunc;

			// Parameter blocks are small and this object lives for exactly one call, so the common case
			// comes off the stack; only an unusually large or over-aligned signature reaches the heap.
			const int32 ParmsSize = FMath::Max<int32>(Func->ParmsSize, 1);
			const int32 ParmsAlignment = FMath::Max<int32>(Func->GetMinAlignment(), 1);
			if (ParmsSize <= InlineParmsCapacity && ParmsAlignment <= InlineParmsAlignment)
			{
				Parms = InlineParms;
			}
			else
			{
				Parms = static_cast<uint8*>(FMemory::Malloc(ParmsSize, ParmsAlignment));
				bParmsOnHeap = true;
			}
			FMemory::Memzero(Parms, ParmsSize);

			ForEachParm(Func, [this, &EventData](FProperty* Prop)
			{
				Prop->InitializeValue_InContainer(Parms);
				if (Prop->HasAnyPropertyFlags(CPF_ReturnParm))
				{
					return;
				}
				FStructProperty* StructProp = CastField<FStructProperty>(Prop);
				if (!StructProp)
				{
					return;
				}

				void* Dest = StructProp->ContainerPtrToValuePtr<void>(Parms);
				// Aliasing is safe only because the parameter is const. A by-reference parameter that is
				// NOT const — `UPARAM(ref) FMyEvent&`, which a const method or a Blueprint pure graph may
				// still declare — could be written through, and the alias is a shallow copy: growing an
				// array or a tag container inside it would reallocate a buffer this block does not own
				// and leave the caller's struct pointing at freed memory. Such a parameter is deep-copied
				// instead, which is what a by-value parameter already gets.
				const bool bByReference =
					FNDCBinder::GetEventDataPassing(Prop) == ENDCEventDataPassing::ByConstReference;

				// Only the event data struct the caller actually supplied; a parameter of any other
				// type keeps the default the initialization above gave it.
				if (!EventData.IsValid() || StructProp->Struct != EventData.GetScriptStruct())
				{
					return;
				}

				if (bByReference)
				{
					// A reference parameter — `const FMyEvent&` — is read straight out of this block:
					// ProcessEvent hands the callee an out-parm record pointing here, and on the way
					// out it neither copies the value back nor destroys it (only locals are
					// destroyed; by-value parms are copied back for the caller to destroy). So a
					// bitwise alias of the caller's struct is enough, and it saves deep-copying
					// whatever heap data that struct owns on every single bound call. The parameter
					// is const, so nothing writes through the alias.
					FMemory::Memcpy(Dest, EventData.GetMemory(), StructProp->GetElementSize());
					AddAliasedParm(StructProp);
				}
				else
				{
					// Taken by value: the callee's frame gets ownership handed back to this block,
					// so it has to be a real copy that we then destroy.
					StructProp->CopyCompleteValue(Dest, EventData.GetMemory());
				}
			});

			// Cues execute on CDOs; the engine does the same when it fires the cue's own BP events.
			const_cast<UObject*>(Owner)->ProcessEvent(Func, Parms);
			bCalled = true;
		}

		~FBoundFunctionCall()
		{
			Release();
		}

		FBoundFunctionCall(const FBoundFunctionCall&) = delete;
		FBoundFunctionCall& operator=(const FBoundFunctionCall&) = delete;

		/** The called function's return property, or nullptr when the call did not happen. */
		const FProperty* GetReturnProperty() const
		{
			return bCalled ? Func->GetReturnProperty() : nullptr;
		}

		/** Address of the returned value inside the parms block, or nullptr when the call did not happen. */
		const void* GetReturnValue() const
		{
			const FProperty* ReturnProp = GetReturnProperty();
			return ReturnProp ? ReturnProp->ContainerPtrToValuePtr<void>(Parms) : nullptr;
		}

	private:
		/** Remembers a parameter that is a bitwise alias of memory this block does not own. */
		void AddAliasedParm(const FProperty* Prop)
		{
			if (NumAliasedParms < UE_ARRAY_COUNT(AliasedParms))
			{
				AliasedParms[NumAliasedParms++] = Prop;
			}
		}

		bool IsAliasedParm(const FProperty* Prop) const
		{
			for (int32 Index = 0; Index < NumAliasedParms; ++Index)
			{
				if (AliasedParms[Index] == Prop)
				{
					return true;
				}
			}
			return false;
		}

		void Release()
		{
			if (Parms)
			{
				ForEachParm(Func, [this](FProperty* Prop)
				{
					// An aliased reference parameter points at memory this block never owned — for the
					// access context, ownership has already been handed back to the caller.
					if (IsAliasedParm(Prop))
					{
						return;
					}
					Prop->DestroyValue_InContainer(Parms);
				});
				if (bParmsOnHeap)
				{
					FMemory::Free(Parms);
					bParmsOnHeap = false;
				}
				Parms = nullptr;
			}
			Func = nullptr;
			bCalled = false;
			NumAliasedParms = 0;
		}

		UFunction* Func = nullptr;
		uint8* Parms = nullptr;
		bool bCalled = false;
		bool bParmsOnHeap = false;
		/** Parameters that went in as a bitwise alias of caller memory: never destroyed here. */
		const FProperty* AliasedParms[1] = { nullptr };
		int32 NumAliasedParms = 0;

		//~ Sized and aligned to hold a target pointer, a sizeable event data struct and a return value
		//~ without touching the heap. The constants are compared against directly rather than derived
		//~ with alignof: alignas on a member does not change the alignment of its array type.
		static constexpr int32 InlineParmsCapacity = 512;
		static constexpr int32 InlineParmsAlignment = 16;
		alignas(InlineParmsAlignment) uint8 InlineParms[InlineParmsCapacity];
	};

	/**
	 * Finds and validates the binding's value function on Owner, warning once on failure and
	 * remembering the answer for that class — the lookup and the signature check then happen once
	 * per class rather than once per write.
	 */
	static UFunction* FindValueFunction(const FNDCBinder& Writer, const FNDCVariableBinding& Binding, const UObject* Owner)
	{
		if (!Owner)
		{
			return nullptr;
		}

		const UClass* OwnerClass = Owner->GetClass();
		UFunction* Cached = nullptr;
		if (Binding.ValueFunctionCache.TryGet(OwnerClass, Binding.BoundFunction, Writer.GetEventDataType(), Binding.Type, Binding.EnumDef, Cached))
		{
			return Cached;
		}

		UFunction* Func = nullptr;
		if (Binding.BoundFunction.IsNone())
		{
			WarnOnce(Owner, Binding.BoundFunction, Binding.VarName, TEXT("no function bound"));
		}
		else
		{
			Func = OwnerClass->FindFunctionByName(Binding.BoundFunction);
			if (!Writer.IsValidValueFunction(Func, Binding.Type, Binding.EnumDef))
			{
				WarnOnce(Owner, Binding.BoundFunction, Binding.VarName, DescribeBindingFailure(Func));
				Func = nullptr;
			}
		}

		Binding.ValueFunctionCache.Store(OwnerClass, Binding.BoundFunction, Writer.GetEventDataType(), Binding.Type, Binding.EnumDef, Func);
		return Func;
	}

	//~ Return-value readers. IsValidValueFunction checked the return type before the call, so a
	//~ mismatch here is unreachable; leaving Out untouched keeps the binding's constant as the value
	//~ rather than dropping the write.
	static void ReadReturn(const FProperty* Prop, const void* Addr, bool& Out)
	{
		if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			Out = BoolProp->GetPropertyValue(Addr);
		}
	}

	static void ReadReturn(const FProperty* Prop, const void* Addr, int32& Out)
	{
		if (const FIntProperty* IntProp = CastField<FIntProperty>(Prop))
		{
			Out = IntProp->GetPropertyValue(Addr);
		}
	}

	static void ReadReturn(const FProperty* Prop, const void* Addr, double& Out)
	{
		if (const FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
		{
			Out = DoubleProp->GetPropertyValue(Addr);
		}
		else if (const FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
		{
			Out = FloatProp->GetPropertyValue(Addr);
		}
	}

	static void ReadReturn(const FProperty* Prop, const void* Addr, uint8& Out)
	{
		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			Out = (uint8)EnumProp->GetUnderlyingProperty()->GetUnsignedIntPropertyValue(Addr);
		}
		else if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			Out = ByteProp->GetPropertyValue(Addr);
		}
	}

	/** Struct returns; a child struct (FNiagaraPosition for FVector) is sliced to the wanted type. */
	template <typename StructType>
	static void ReadReturn(const FProperty* Prop, const void* Addr, StructType& Out)
	{
		const FStructProperty* StructProp = CastField<FStructProperty>(Prop);
		if (StructProp && StructProp->Struct && StructProp->Struct->IsChildOf(TBaseStructure<StructType>::Get()))
		{
			Out = *static_cast<const StructType*>(Addr);
		}
	}

	/**
	 * Whether a member is one a binding may name at all, before anything asks about its type.
	 *
	 * Editor-only and deprecated members are refused because a row bound to one passes every test in
	 * the editor and then reads a member that is not in a cooked build.
	 *
	 * A static array is refused because its name does not say which element. `FVector Values[4]` is
	 * one FStructProperty of FVector as far as every type test here is concerned, and both the offset
	 * a path resolves to and the value a store reads are element zero — an answer nobody asked for,
	 * arrived at silently, on a row that looks exactly like a correct one. Binding element zero on
	 * purpose is a getter away and says so; this is the one that cannot.
	 */
	static bool IsBindableMember(const FProperty* Prop)
	{
		return Prop
			&& !Prop->HasAnyPropertyFlags(CPF_EditorOnly | CPF_Deprecated)
			&& Prop->ArrayDim <= 1;
	}

	/** The enum a property is of, whether it is stored as an FEnumProperty or a legacy enum-typed byte. */
	static const UEnum* GetPropertyEnum(const FProperty* Prop)
	{
		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			return EnumProp->GetEnum();
		}
		if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			return ByteProp->Enum;
		}
		return nullptr;
	}

	/**
	 * The one place that decides whether a bound function's return value may drive a context field,
	 * and — when both addresses are given — puts it there.
	 *
	 * Check and apply are deliberately the same walk. The check runs once per class and is cached; the
	 * apply runs per write and asks the question again rather than trusting that cache, so a hole in
	 * the caching (a retyped field the key failed to notice) costs a skipped row instead of a wrongly
	 * typed store into the context's memory. Two functions would drift; this one cannot.
	 *
	 * Conversions are the ones that cannot hide a mistake: a child struct sliced to its parent, and a
	 * soft object reference resolved. Nothing narrows — an int does not become a float, and an enum is
	 * driven only by its own enum — because a silent coercion is exactly the sort of wrong a binding
	 * panel is supposed to make impossible.
	 */
	static bool AssignReturnToField(const FProperty* ReturnProp, const void* ReturnAddr, const FProperty* Field, void* Dest)
	{
		// Both ends, and here rather than in a check beside it: this function is the store as well as
		// the question, so a member it would read or write element zero of is refused where the write
		// itself would have done it, and the refusal is the one the row already reports.
		if (!IsBindableMember(ReturnProp) || !IsBindableMember(Field))
		{
			return false;
		}
		const bool bApply = (ReturnAddr != nullptr && Dest != nullptr);

		if (const FObjectPropertyBase* FieldObj = CastField<FObjectPropertyBase>(Field))
		{
			const FObjectPropertyBase* ReturnObj = CastField<FObjectPropertyBase>(ReturnProp);
			if (!ReturnObj || !ReturnObj->PropertyClass || !FieldObj->PropertyClass
				|| !ReturnObj->PropertyClass->IsChildOf(FieldObj->PropertyClass))
			{
				return false;
			}
			if (bApply)
			{
				// A soft reference is resolved here, the same convenience the handler-system binding
				// has: a getter that reads its answer out of a data table naturally holds one.
				UObject* Value = nullptr;
				if (const FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(ReturnProp))
				{
					Value = SoftProp->GetPropertyValue(ReturnAddr).LoadSynchronous();
				}
				else
				{
					Value = ReturnObj->GetObjectPropertyValue(ReturnAddr);
				}
				FieldObj->SetObjectPropertyValue(Dest, Value);
			}
			return true;
		}

		if (const FStructProperty* FieldStruct = CastField<FStructProperty>(Field))
		{
			const FStructProperty* ReturnStruct = CastField<FStructProperty>(ReturnProp);
			if (!ReturnStruct || !ReturnStruct->Struct || !FieldStruct->Struct
				|| !ReturnStruct->Struct->IsChildOf(FieldStruct->Struct))
			{
				return false;
			}
			if (bApply)
			{
				// The field's own struct ops over the child's leading bytes. A child struct begins with
				// its parent's layout, so this is the slice FNiagaraPosition -> FVector needs.
				FieldStruct->Struct->CopyScriptStruct(Dest, ReturnAddr);
			}
			return true;
		}

		if (const FBoolProperty* FieldBool = CastField<FBoolProperty>(Field))
		{
			const FBoolProperty* ReturnBool = CastField<FBoolProperty>(ReturnProp);
			if (!ReturnBool)
			{
				return false;
			}
			if (bApply)
			{
				// SetPropertyValue rather than a raw store: a context's flags are bitfields
				// (uint32 bSomething : 1) and only the property knows which bit of the byte it owns.
				FieldBool->SetPropertyValue(Dest, ReturnBool->GetPropertyValue(ReturnAddr));
			}
			return true;
		}

		// Enums before the general numeric case: an enum field is driven by its own enum, never by
		// whatever integer happens to fit in it.
		if (const UEnum* FieldEnum = GetPropertyEnum(Field))
		{
			if (GetPropertyEnum(ReturnProp) != FieldEnum)
			{
				return false;
			}
			// Both sides may be either storage form, so go through the numeric property underneath.
			const FNumericProperty* FieldNum = CastField<FEnumProperty>(Field)
				? CastField<FEnumProperty>(Field)->GetUnderlyingProperty()
				: CastField<FNumericProperty>(Field);
			const FNumericProperty* ReturnNum = CastField<FEnumProperty>(ReturnProp)
				? CastField<FEnumProperty>(ReturnProp)->GetUnderlyingProperty()
				: CastField<FNumericProperty>(ReturnProp);
			if (!FieldNum || !ReturnNum)
			{
				return false;
			}
			if (bApply)
			{
				FieldNum->SetIntPropertyValue(Dest, ReturnNum->GetSignedIntPropertyValue(ReturnAddr));
			}
			return true;
		}

		if (const FNumericProperty* FieldNum = CastField<FNumericProperty>(Field))
		{
			const FNumericProperty* ReturnNum = CastField<FNumericProperty>(ReturnProp);
			if (!ReturnNum || ReturnNum->IsFloatingPoint() != FieldNum->IsFloatingPoint())
			{
				return false;
			}
			if (bApply)
			{
				if (FieldNum->IsFloatingPoint())
				{
					// float and double are interchangeable on purpose: a Blueprint "float" is a double
					// and a C++ one usually is not, which is not a distinction an author made.
					FieldNum->SetFloatingPointPropertyValue(Dest, ReturnNum->GetFloatingPointPropertyValue(ReturnAddr));
				}
				else
				{
					FieldNum->SetIntPropertyValue(Dest, ReturnNum->GetSignedIntPropertyValue(ReturnAddr));
				}
			}
			return true;
		}

		// Everything else — names, strings, text, containers — on identical types only. SameType is
		// the engine's own answer to "would this assignment be well formed", inner types included.
		if (ReturnProp->SameType(Field))
		{
			if (bApply)
			{
				Field->CopyCompleteValue(Dest, ReturnAddr);
			}
			return true;
		}
		return false;
	}

	/**
	 * Whether a bound function answered "no opinion" — a null object reference, the only value that
	 * can mean that. A soft reference is judged unresolved rather than loaded: asking whether an
	 * author pointed at something is not a reason to pull an asset off disk.
	 *
	 * Everything else always has an opinion: a zero vector is a location and false is a flag.
	 */
	static bool IsNullObjectReturn(const FProperty* ReturnProp, const void* ReturnAddr)
	{
		if (const FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(ReturnProp))
		{
			return SoftProp->GetPropertyValue(ReturnAddr).IsNull();
		}
		const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(ReturnProp);
		return ObjProp && ObjProp->GetObjectPropertyValue(ReturnAddr) == nullptr;
	}
}

FNDCContextBinding FNDCContextBinding::Make(FName InFieldName, FName InFunction, bool bInRequired, FName InEnableFlagField)
{
	FNDCContextBinding Binding;
	Binding.FieldName = InFieldName;
	Binding.Source = ENDCValueSource::Function;
	Binding.BoundFunction = InFunction;
	Binding.bRequired = bInRequired;
	Binding.EnableFlagField = InEnableFlagField;
	return Binding;
}

FNDCContextBinding FNDCContextBinding::MakeFromEventData(FName InFieldName, FName InEventDataField, bool bInRequired, FName InEnableFlagField)
{
	FNDCContextBinding Binding;
	Binding.FieldName = InFieldName;
	Binding.Source = ENDCValueSource::EventData;
	Binding.BoundEventDataField = InEventDataField;
	Binding.bRequired = bInRequired;
	Binding.EnableFlagField = InEnableFlagField;
	return Binding;
}

FNDCVariableBinding FNDCVariableBinding::MakeFunctionBinding(FName InVarName, ENDCVariableType InType, FName InFunction, UEnum* InEnumDef)
{
	FNDCVariableBinding Binding;
	Binding.VarName = InVarName;
	Binding.Type = InType;
	Binding.Source = ENDCValueSource::Function;
	Binding.BoundFunction = InFunction;
	Binding.EnumDef = InEnumDef;
	return Binding;
}

FNDCVariableBinding FNDCVariableBinding::MakeEventDataBinding(FName InVarName, ENDCVariableType InType, FName InEventDataField, UEnum* InEnumDef)
{
	FNDCVariableBinding Binding;
	Binding.VarName = InVarName;
	Binding.Type = InType;
	Binding.Source = ENDCValueSource::EventData;
	Binding.BoundEventDataField = InEventDataField;
	Binding.EnumDef = InEnumDef;
	return Binding;
}

FNiagaraTypeDefinition FNDCVariableBinding::NiagaraTypeFromVariableType(ENDCVariableType Type)
{
	switch (Type)
	{
	case ENDCVariableType::Bool:        return FNiagaraTypeDefinition::GetBoolDef();
	case ENDCVariableType::Int32:       return FNiagaraTypeDefinition::GetIntDef();
	case ENDCVariableType::Enum:        return FNiagaraTypeDefinition::GetIntDef();
	case ENDCVariableType::Float:       return FNiagaraTypeHelper::GetDoubleDef();
	case ENDCVariableType::Vector2D:    return FNiagaraTypeHelper::GetVector2DDef();
	case ENDCVariableType::Vector:      return FNiagaraTypeHelper::GetVectorDef();
	case ENDCVariableType::Vector4:     return FNiagaraTypeHelper::GetVector4Def();
	case ENDCVariableType::Quat:        return FNiagaraTypeHelper::GetQuatDef();
	case ENDCVariableType::LinearColor: return FNiagaraTypeDefinition::GetColorDef();
	case ENDCVariableType::Position:    return FNiagaraTypeDefinition::GetPositionDef();
	case ENDCVariableType::SpawnInfo:   return FNiagaraTypeDefinition(FNiagaraSpawnInfo::StaticStruct());
	case ENDCVariableType::ID:          return FNiagaraTypeDefinition::GetIDDef();
	default:                         return FNiagaraTypeDefinition();
	}
}
ENDCVariableType FNDCVariableBinding::VariableTypeFromNiagaraType(const FNiagaraTypeDefinition& TypeDef)
{
	if (TypeDef == FNiagaraTypeDefinition::GetBoolDef())     { return ENDCVariableType::Bool; }
	if (TypeDef == FNiagaraTypeDefinition::GetIntDef())      { return ENDCVariableType::Int32; }
	if (TypeDef == FNiagaraTypeDefinition::GetFloatDef())    { return ENDCVariableType::Float; }
	// Editor-created "float" variables are doubles (FNiagaraTypeHelper::GetTypeDef<float>() -> DoubleDef).
	if (TypeDef == FNiagaraTypeHelper::GetDoubleDef())       { return ENDCVariableType::Float; }
	if (TypeDef == FNiagaraTypeDefinition::GetVec2Def())     { return ENDCVariableType::Vector2D; }
	if (TypeDef == FNiagaraTypeDefinition::GetVec3Def())     { return ENDCVariableType::Vector; }
	if (TypeDef == FNiagaraTypeDefinition::GetVec4Def())     { return ENDCVariableType::Vector4; }
	if (TypeDef == FNiagaraTypeDefinition::GetQuatDef())     { return ENDCVariableType::Quat; }
	if (TypeDef == FNiagaraTypeDefinition::GetColorDef())    { return ENDCVariableType::LinearColor; }
	if (TypeDef == FNiagaraTypeDefinition::GetPositionDef()) { return ENDCVariableType::Position; }
	if (TypeDef == FNiagaraTypeDefinition::GetIDDef())       { return ENDCVariableType::ID; }
	if (TypeDef.IsEnum())                                    { return ENDCVariableType::Enum; }

	// Not fallbacks in practice — this is where a real channel variable is matched. A data channel
	// normalizes its variables to the LWC types on the way in (FNiagaraDataChannelVariable::
	// ToDataChannelType maps Vec3 -> FVector, Float -> double, and so on), and those defs wrap the
	// plain CoreUObject structs, so it is the struct that identifies them. The builtin comparisons
	// above stay for a caller passing a type def straight from Niagara rather than from a channel.
	if (const UScriptStruct* Struct = Cast<UScriptStruct>(TypeDef.GetStruct()))
	{
		if (Struct == TBaseStructure<FVector>::Get())      { return ENDCVariableType::Vector; }
		if (Struct == TBaseStructure<FVector2D>::Get())    { return ENDCVariableType::Vector2D; }
		if (Struct == TBaseStructure<FVector4>::Get())     { return ENDCVariableType::Vector4; }
		if (Struct == TBaseStructure<FQuat>::Get())        { return ENDCVariableType::Quat; }
		if (Struct == TBaseStructure<FLinearColor>::Get()) { return ENDCVariableType::LinearColor; }
		if (Struct == FNiagaraPosition::StaticStruct())    { return ENDCVariableType::Position; }
		if (Struct == FNiagaraBool::StaticStruct())        { return ENDCVariableType::Bool; }
		if (Struct == FNiagaraInt32::StaticStruct())       { return ENDCVariableType::Int32; }
		if (Struct == FNiagaraSpawnInfo::StaticStruct())   { return ENDCVariableType::SpawnInfo; }
		if (Struct == FNiagaraID::StaticStruct())          { return ENDCVariableType::ID; }
	}
	return ENDCVariableType::Unsupported;
}

ENDCBindingRejection FNDCBinder::GetBindingRejection(const UFunction* Func)
{
	if (!Func)
	{
		return ENDCBindingRejection::Missing;
	}

	// FUNC_Delegate: a delegate signature is a UFunction living on the class and is findable by name,
	//   but has no body — ProcessEvent returns before touching the parameter block, so the binding
	//   would quietly write a default-constructed value forever.
	// FUNC_EditorOnly: gone from a cooked build. The binding works through all of editor testing and
	//   then resolves to nothing in a packaged game, which is the worst moment to find out.
	// FUNC_Net: ProcessEvent routes a replicated function through GetFunctionCallspace, so calling one
	//   would send an RPC per write and might not run the body locally at all. UHT will not let a
	//   replicated function have a return value, so this is unreachable from a UHT-declared class and
	//   is here for names that reach a binding from somewhere else — the check is one masked AND.
	static constexpr EFunctionFlags UnbindableFlags = FUNC_Delegate | FUNC_EditorOnly | FUNC_Net;
	if (Func->HasAnyFunctionFlags(UnbindableFlags))
	{
		return Func->HasAnyFunctionFlags(FUNC_Delegate)   ? ENDCBindingRejection::DelegateSignature
			 : Func->HasAnyFunctionFlags(FUNC_EditorOnly) ? ENDCBindingRejection::EditorOnly
			 :                                              ENDCBindingRejection::Networked;
	}

	// The rule that keeps a value binding a read. A cue notify executes on its CDO —
	// GameplayCueNotify_Burst and _Static are not instanced — so a bound function that writes to a
	// member would be mutating state shared by every use of that cue in the game, from inside what
	// reads like a getter. This is the same test the engine applies to a read binding in
	// SPropertyBinding::ForEachBindableFunction.
	//
	// No exception to this any more. The one that used to exist was the whole-context binding, which
	// was handed a context to fill and so could not be pure; per-field rows return values instead,
	// which is what let it go. Every binding the writer has is now a read.
	//
	// Worth knowing how much this actually enforces. A const C++ method is checked by the compiler, and
	// a Blueprint pure graph cannot hold a Set node. But a Blueprint override of a const native event
	// inherits FUNC_Const through FUNC_FuncInherit (KismetCompiler.cpp, where the override's flags are
	// masked from the overridden function) while its own graph does have exec pins, and nothing checks
	// what it writes. So this keeps an obviously mutating function out of a binding; it is not a
	// guarantee that a bound function cannot mutate.
	if (!Func->HasAnyFunctionFlags(FUNC_Const | FUNC_BlueprintPure))
	{
		return ENDCBindingRejection::NotPure;
	}

	return ENDCBindingRejection::None;
}

const TCHAR* FNDCBinder::DescribeRejection(ENDCBindingRejection Rejection)
{
	switch (Rejection)
	{
	case ENDCBindingRejection::Missing:           return TEXT("no function of that name on the owning class");
	case ENDCBindingRejection::Networked:         return TEXT("the function is replicated — a binding must not send an RPC per write");
	case ENDCBindingRejection::EditorOnly:        return TEXT("the function is editor-only and would not exist in a cooked build");
	case ENDCBindingRejection::DelegateSignature: return TEXT("that is a delegate signature, not a callable function");
	case ENDCBindingRejection::NotPure:           return TEXT("the function is neither const nor Blueprint-pure — a binding must not mutate its owner");
	default:                                      return TEXT("");
	}
}

bool FNDCBinder::IsPropertyCompatibleWithVariableType(const FProperty* Prop, ENDCVariableType Type, const UEnum* WantedEnum)
{
	// Asked before the type: the bind menu offers members straight out of the event data struct
	// without going through ResolveFieldPath, so this is where a static array would otherwise be
	// offered as though it were the single value it is not.
	if (!NDCBinderPrivate::IsBindableMember(Prop) || Type == ENDCVariableType::Unsupported)
	{
		return false;
	}

	switch (Type)
	{
	case ENDCVariableType::Bool:
		return Prop->IsA(FBoolProperty::StaticClass());
	case ENDCVariableType::Int32:
		return Prop->IsA(FIntProperty::StaticClass());
	case ENDCVariableType::Float:
		// Blueprint floats are double since UE5; accept both.
		return Prop->IsA(FDoubleProperty::StaticClass()) || Prop->IsA(FFloatProperty::StaticClass());
	case ENDCVariableType::Enum:
		{
			if (!Prop->IsA(FEnumProperty::StaticClass()) && !Prop->IsA(FByteProperty::StaticClass()))
			{
				return false;
			}
			// A property that names an enum has to name this row's one. A property that names none —
			// a bare uint8 — is taken at its word: see the header for why this is looser than the
			// context-field rule and meant to be.
			const UEnum* PropEnum = NDCBinderPrivate::GetPropertyEnum(Prop);
			return !WantedEnum || !PropEnum || PropEnum == WantedEnum;
		}
	case ENDCVariableType::Vector2D:
	case ENDCVariableType::Vector:
	case ENDCVariableType::Vector4:
	case ENDCVariableType::Quat:
	case ENDCVariableType::LinearColor:
	case ENDCVariableType::Position:
	case ENDCVariableType::SpawnInfo:
	case ENDCVariableType::ID:
		{
			const FStructProperty* StructProp = CastField<FStructProperty>(Prop);
			if (!StructProp || !StructProp->Struct)
			{
				return false;
			}
			const UScriptStruct* Wanted =
				Type == ENDCVariableType::Vector2D    ? TBaseStructure<FVector2D>::Get() :
				Type == ENDCVariableType::Vector4     ? TBaseStructure<FVector4>::Get() :
				Type == ENDCVariableType::Quat        ? TBaseStructure<FQuat>::Get() :
				Type == ENDCVariableType::LinearColor ? TBaseStructure<FLinearColor>::Get() :
				Type == ENDCVariableType::SpawnInfo   ? FNiagaraSpawnInfo::StaticStruct() :
				Type == ENDCVariableType::ID          ? FNiagaraID::StaticStruct() :
				TBaseStructure<FVector>::Get();

			// A child struct counts, because reading one already works: ReadReturn slices it to the
			// wanted type, and a child begins with its parent's layout. This used to be allowed for
			// Position alone, on the grounds that FNiagaraPosition is an empty FVector child — but so
			// are FVector_NetQuantize10 and FVector_NetQuantizeNormal, which is what a GameplayCue's
			// Location and Normal actually are. Refusing those while the reader handles them fine was
			// the check being stricter than the write it is supposed to describe.
			return StructProp->Struct->IsChildOf(Wanted);
		}
	default:
		return false;
	}
}

void FNDCBinder::ForEachEventDataField(TFunctionRef<void(FProperty&)> Body) const
{
	if (!EventDataType)
	{
		return;
	}
	for (TFieldIterator<FProperty> It(EventDataType); It; ++It)
	{
		// Everything a binding may name; see IsBindableMember for what that rules out and why.
		// Nothing else is refused — reading a struct member calls nothing and mutates nothing, so
		// none of the reasons a function is refused apply here.
		if (NDCBinderPrivate::IsBindableMember(*It))
		{
			Body(**It);
		}
	}
}

bool FNDCBinder::ResolveFieldPath(const UStruct* Root, FName FieldPath, const FProperty*& OutLeaf, int32& OutOffset)
{
	OutLeaf = nullptr;
	OutOffset = 0;
	if (!Root || FieldPath.IsNone())
	{
		return false;
	}

	// A stack buffer rather than an FString: this runs once per row per struct, but a heap allocation
	// to read a name the caller already had is not something to leave on any path.
	TStringBuilder<128> PathText;
	FieldPath.AppendString(PathText);
	FStringView Remaining = PathText.ToView();

	const UStruct* Owner = Root;
	const FProperty* Leaf = nullptr;
	int32 Offset = 0;

	for (int32 Depth = 0; !Remaining.IsEmpty(); ++Depth)
	{
		// Out of depth, or the previous segment was not a struct and there is nothing to look inside.
		if (Depth >= MaxFieldPathDepth || !Owner)
		{
			return false;
		}

		int32 Dot = INDEX_NONE;
		const bool bHasDot = Remaining.FindChar(TEXT('.'), Dot);
		const FStringView Segment = bHasDot ? Remaining.Left(Dot) : Remaining;
		Remaining = bHasDot ? Remaining.RightChop(Dot + 1) : FStringView();
		// A dot promises another segment. Without this, a trailing dot resolves to the segment before
		// it and a path names something it does not say.
		if (Segment.IsEmpty() || (bHasDot && Remaining.IsEmpty()))
		{
			return false;
		}

		// A path with no dot in it is the name the row already holds, and building an FName from its own
		// characters would be a hash and a name-table probe to arrive back at it. Every binding written
		// before paths existed is this case.
		//
		// For the rest, FNAME_Find rather than FNAME_Add: a segment that names nothing is a broken path,
		// and it must not leave an entry in the name table behind for having been typed once.
		const FName SegmentName = (Depth == 0 && !bHasDot)
			? FieldPath
			: FName(Segment.Len(), Segment.GetData(), FNAME_Find);
		const FProperty* Found = nullptr;
		for (TFieldIterator<FProperty> It(Owner); It; ++It)
		{
			// The same rule ForEachEventDataField offers by, applied at every segment rather than only
			// at the leaf: a path THROUGH an editor-only member resolves here and reads nothing in a
			// cooked build, and one through a static array resolves to element zero of it.
			if (It->GetFName() == SegmentName && NDCBinderPrivate::IsBindableMember(*It))
			{
				Found = *It;
				break;
			}
		}
		if (!Found)
		{
			return false;
		}

		Offset += Found->GetOffset_ForInternal();
		Leaf = Found;
		const FStructProperty* StructProp = CastField<FStructProperty>(Found);
		Owner = StructProp ? StructProp->Struct : nullptr;
	}

	if (!Leaf)
	{
		return false;
	}
	OutLeaf = Leaf;
	OutOffset = Offset;
	return true;
}

const FProperty* FNDCBinder::FindEventDataField(FName FieldPath) const
{
	const FProperty* Leaf = nullptr;
	int32 UnusedOffset = 0;
	if (ResolveFieldPath(EventDataType, FieldPath, Leaf, UnusedOffset))
	{
		return Leaf;
	}
	return nullptr;
}

void FNDCBinder::ResetBindingWarnings()
{
	using namespace NDCBinderPrivate;

	FScopeLock Lock(&WarnedLock);
	Warned.Reset();
}

ENDCEventDataPassing FNDCBinder::GetEventDataPassing(const FProperty* Parm)
{
	if (!Parm)
	{
		return ENDCEventDataPassing::ByValue;
	}
	// UHT and the Blueprint compiler both set the two reference flags together — CPF_OutParm rides
	// along on every by-reference parameter — so either one means the value is not owned by the
	// parameter block. Constness is then the whole question: it is what makes an alias of the
	// caller's value safe to hand over.
	if (!Parm->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm))
	{
		return ENDCEventDataPassing::ByValue;
	}
	return Parm->HasAllPropertyFlags(CPF_ConstParm)
		? ENDCEventDataPassing::ByConstReference
		: ENDCEventDataPassing::ByMutableReference;
}

bool FNDCBinder::GetBoundFunctionEventDataStruct(const UFunction* Func, UScriptStruct*& OutEventDataStruct)
{
	if (!Func)
	{
		return false;
	}

	UScriptStruct* Declared = nullptr;
	int32 ParamIndex = 0;
	bool bShaped = true;
	NDCBinderPrivate::ForEachParm(Func, [&Declared, &ParamIndex, &bShaped](const FProperty* Prop)
	{
		if (!bShaped || Prop->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			return;
		}
		if (ParamIndex++ == 0)
		{
			const FStructProperty* StructProp = CastField<FStructProperty>(Prop);
			// Taken by value, or by const reference. A mutable reference is refused: a binding is a
			// read, and one that wrote back into the caller's event data would be reaching past the
			// value it was asked for into the thing that produced it. The call path also copies such
			// a parameter defensively, so this is the message rather than the protection — but the
			// message is the half an author can act on.
			const bool bMutableReference =
				GetEventDataPassing(Prop) == ENDCEventDataPassing::ByMutableReference;
			bShaped = StructProp != nullptr && !bMutableReference;
			Declared = bShaped ? StructProp->Struct : nullptr;
		}
		else
		{
			bShaped = false;
		}
	});

	if (!bShaped)
	{
		return false;
	}
	OutEventDataStruct = Declared;
	return true;
}

bool FNDCBinder::IsValidValueFunction(const UFunction* Func, ENDCVariableType Type, const UEnum* WantedEnum) const
{
	if (GetBindingRejection(Func) != ENDCBindingRejection::None)
	{
		return false;
	}
	if (!IsPropertyCompatibleWithVariableType(Func->GetReturnProperty(), Type, WantedEnum))
	{
		return false;
	}
	return NDCBinderPrivate::HasBoundFunctionSignature(Func, EventDataType);
}

void FNDCBinder::InitEventDataType(UScriptStruct* InType)
{
	// Only from the owning object's constructor. Everything a row means is stated against this type,
	// so changing it later moves the contract without moving a single row — and the panel, the
	// validator and every cached resolve go on answering for the type that used to be declared.
	//
	// A writer that no object owns is a different case and has its own entry point; see the header.
	ensureMsgf(FUObjectThreadContext::Get().IsInConstructor > 0,
		TEXT("NDCBinder: InitEventDataType outside a constructor. A writer on a live object keeps the type it was declared with; a standalone one uses SetEventDataTypeUnchecked."));

	EventDataType = InType;
}

void FNDCBinder::InitBindings(TArray<FNDCVariableBinding> InRows)
{
	// Only from the owning object's constructor, for the reason InitEventDataType gives: the caches
	// under these rows are keyed on what a row says, and replacing rows on a live object leaves them
	// holding answers resolved against names that are no longer there.
	ensureMsgf(FUObjectThreadContext::Get().IsInConstructor > 0,
		TEXT("NDCBinder: InitBindings outside a constructor. Rows on a live object are authored data; a standalone writer uses GetMutableBindingsUnchecked."));

	Bindings = MoveTemp(InRows);
}

void FNDCBinder::InitContextBindings(TArray<FNDCContextBinding> InRows)
{
	ensureMsgf(FUObjectThreadContext::Get().IsInConstructor > 0,
		TEXT("NDCBinder: InitContextBindings outside a constructor. Rows on a live object are authored data; a standalone writer uses GetMutableContextBindingsUnchecked."));

	ContextBindings = MoveTemp(InRows);
}

UNiagaraDataChannel* FNDCBinder::GetChannel() const
{
	return DataChannel ? DataChannel->Get() : nullptr;
}


void FNDCBinder::ForEachContextInputField(const UScriptStruct* ContextType, TFunctionRef<void(FProperty&)> Body)
{
#if WITH_EDITORONLY_DATA
	if (!ContextType || !ContextType->IsChildOf(FNDCAccessContextBase::StaticStruct()))
	{
		return;
	}

	// Supers first, so a panel built from this reads base-to-derived the way the struct does. A plain
	// TFieldIterator walks the most-derived struct first, which would put OwningComponent — the field
	// most contexts are actually about — at the bottom of the list.
	TArray<const UStruct*, TInlineAllocator<4>> Chain;
	for (const UStruct* Struct = ContextType; Struct; Struct = Struct->GetSuperStruct())
	{
		Chain.Add(Struct);
	}
	for (int32 Index = Chain.Num() - 1; Index >= 0; --Index)
	{
		for (TFieldIterator<FProperty> It(Chain[Index], EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			// Niagara's own marker for "this is input to the lookup". Everything else on a context is
			// its answer back to the caller — SpawnedSystems and friends — and writing those would be
			// talking over the channel.
			if (It->HasMetaData(TEXT("NDCAccessContextInput")))
			{
				Body(**It);
			}
		}
	}
#endif
}

#if WITH_EDITOR
FName FNDCBinder::ResolveContextFieldEnableFlag(const UScriptStruct* ContextType, const FProperty* Field)
{
	if (!ContextType || !Field)
	{
		return NAME_None;
	}

	// Only a flag that is itself an input: a condition on some hidden internal is not something a row
	// should be quietly flipping. That test reads metadata, which is why this whole function is
	// editor-only rather than guarded case by case: a build without metadata cannot ask the question,
	// and the version of it that answered anyway would have been a second, laxer rule that nothing
	// called and nothing marked unreachable. The answer is stored on the row at edit time, so a
	// cooked build has no reason to ask.
	auto AcceptFlag = [ContextType](FName Candidate) -> FName
	{
		const FBoolProperty* Flag = CastField<FBoolProperty>(ContextType->FindPropertyByName(Candidate));
		return (Flag && Flag->HasMetaData(TEXT("NDCAccessContextInput"))) ? Candidate : NAME_None;
	};

	// Stated outright, which is the case worth trusting: SystemToSpawn, CellSizeOverride,
	// SystemBoundsPadding. Only a bare property name resolves here — a compound expression
	// ("bA && bB", "Mode == EFoo::Bar") names no single flag to switch, so it finds no property and
	// falls through to the convention below rather than resolving to half of itself.
	if (Field->HasMetaData(TEXT("EditCondition")))
	{
		if (const FName Stated = AcceptFlag(FName(*Field->GetMetaData(TEXT("EditCondition")))); !Stated.IsNone())
		{
			return Stated;
		}
	}

	// Not stated, but real: Location does nothing unless bOverrideLocation is set —
	// FNDCAccessContext::GetLocation returns the owning component's location otherwise — and Niagara
	// links the two by name rather than by metadata. Binding a location and silently getting the
	// component's is exactly the failure this whole panel exists to prevent, so the convention is
	// followed where the metadata is missing — and where it is present but unusable, which is what a
	// compound condition is. AcceptFlag is what keeps that safe: the convention can only ever produce
	// a bool this context declares as an input, never some neighbouring field it does not gate.
	return AcceptFlag(FName(*(TEXT("bOverride") + Field->GetName())));
}
#endif // WITH_EDITOR

const UScriptStruct* FNDCBinder::GetContextType() const
{
	const UNiagaraDataChannel* Channel = GetChannel();
	return Channel ? Channel->GetAccessContextType().Get() : nullptr;
}

const FNDCContextBinding* FNDCBinder::FindContextBinding(FName FieldName) const
{
	return ContextBindings.FindByPredicate(
		[FieldName](const FNDCContextBinding& Binding) { return Binding.FieldName == FieldName; });
}

bool FNDCBinder::CanContextFieldBeRequired(const FProperty* Field)
{
	// Only an object reference has a value that means "there wasn't one". A zero vector is a location
	// and false is a flag, so offering the checkbox there would promise something it cannot deliver.
	return Field && Field->IsA(FObjectPropertyBase::StaticClass());
}

bool FNDCBinder::CanAssignPropertyToField(const FProperty* Source, const FProperty* Field)
{
	// Check only — no addresses, so nothing is written.
	return NDCBinderPrivate::AssignReturnToField(Source, nullptr, Field, nullptr);
}

void FNDCBinder::GetFieldAllowedClasses(const FProperty* Field, TArray<const UClass*>& OutClasses)
{
	OutClasses.Reset();
#if WITH_EDITORONLY_DATA
	static const FName AllowedClassesName(TEXT("AllowedClasses"));
	if (!CastField<FObjectPropertyBase>(Field) || !Field->HasMetaData(AllowedClassesName))
	{
		return;
	}

	TArray<FString> Names;
	Field->GetMetaData(AllowedClassesName).ParseIntoArray(Names, TEXT(","), /*InCullEmpty=*/ true);
	for (FString& Name : Names)
	{
		Name.TrimStartAndEndInline();
		// Entries are written as paths (/Script/Niagara.NiagaraSystem) but short names are legal too,
		// and an entry naming a class that no longer exists simply drops out — see the caller, which
		// treats "nothing resolved" as "no restriction" rather than as "nothing is allowed".
		//
		// Which is why the load matters rather than being a nicety: a named class that is merely not
		// loaded yet would drop out the same way an absent one does, and a restriction that loses all
		// of its entries does not narrow to nothing, it WIDENS to everything. The engine's own reader
		// of this metadata (PropertyCustomizationHelpers::GetClassesFromMetadataString) falls back the
		// same way; it cannot be called from here, since it lives in an editor module this runtime one
		// must not depend on.
		const UClass* Allowed = UClass::TryFindTypeSlow<UClass>(Name, EFindFirstObjectOptions::None);
		if (!Allowed)
		{
			Allowed = LoadObject<UClass>(nullptr, *Name);
		}
		if (Allowed)
		{
			OutClasses.Add(Allowed);
		}
	}
#endif
}

bool FNDCBinder::IsSourceAllowedByFieldClasses(const FProperty* Source, const FProperty* Field)
{
	TArray<const UClass*> Allowed;
	GetFieldAllowedClasses(Field, Allowed);
	if (Allowed.IsEmpty())
	{
		// Either the field says nothing about which classes it wants, or every class it named has since
		// gone. Neither is a reason to forbid everything: a restriction nobody can read is not one.
		return true;
	}

	const FObjectPropertyBase* SourceObj = CastField<FObjectPropertyBase>(Source);
	if (!SourceObj || !SourceObj->PropertyClass)
	{
		return false;
	}
	for (const UClass* AllowedClass : Allowed)
	{
		if (SourceObj->PropertyClass->IsChildOf(AllowedClass))
		{
			return true;
		}
	}
	return false;
}

bool FNDCBinder::IsValidContextFieldFunction(const UFunction* Func, const FProperty* Field) const
{
	if (GetBindingRejection(Func) != ENDCBindingRejection::None || !Field)
	{
		return false;
	}
	return CanAssignPropertyToField(Func->GetReturnProperty(), Field)
		&& NDCBinderPrivate::HasBoundFunctionSignature(Func, EventDataType);
}

bool FNDCBinder::ApplyContextBinding(const FNDCContextBinding& Binding, FNDCAccessContextInst& Context, const UObject* FunctionOwner, FConstStructView EventData) const
{
	using namespace NDCBinderPrivate;

	const UScriptStruct* ContextType = Context.GetScriptStruct();
	void* ContextMemory = Context.AccessContext.GetMutableMemory();
	if (!ContextType || !ContextMemory)
	{
		return true;
	}
	const bool bFromEventData = (Binding.Source == ENDCValueSource::EventData);
	const FName BoundName = bFromEventData ? Binding.BoundEventDataField : Binding.BoundFunction;

	// Resolved once per context type rather than once per write. FindPropertyByName walks the struct's
	// property list comparing FNames, and this row's name and struct are both fixed.
	const FProperty* Field = nullptr;
	if (!Binding.FieldCache.TryGet(ContextType, Field))
	{
		Field = ContextType->FindPropertyByName(Binding.FieldName);
		Binding.FieldCache.Store(ContextType, Field);
	}
	if (!Field)
	{
		// A row left over from a different context type. Kept rather than deleted, like a payload row
		// for a departed channel variable, and skipped with one line saying so.
		WarnOnce(FunctionOwner, BoundName, Binding.FieldName,
			TEXT("the access context has no such field — the row is skipped"));
		return true;
	}

	// Where the value comes from. Both paths end at the same pair — a property and an address — so
	// everything below is written once and does not care which of the two produced it.
	const FProperty* SourceProp = nullptr;
	const void* SourceAddr = nullptr;

	UFunction* Func = nullptr;
	if (bFromEventData)
	{
		// The field is looked up by name — the row stores a bare name and the struct it refers to can
		// change underneath it, which is what the cache is keyed on — but the TYPE is not checked here.
		// The store below answers the same question as a side effect of doing the work, and asking it
		// twice walked the whole property-kind chain twice for every row of every write.
		const FProperty* EventField = nullptr;
		int32 EventFieldOffset = 0;
		if (!Binding.EventFieldCache.TryGet(EventDataType, EventField, ENDCVariableType::Unsupported, nullptr, &EventFieldOffset))
		{
			if (!ResolveFieldPath(EventDataType, Binding.BoundEventDataField, EventField, EventFieldOffset))
			{
				EventField = nullptr;
				EventFieldOffset = 0;
			}
			Binding.EventFieldCache.Store(EventDataType, EventField, ENDCVariableType::Unsupported, nullptr, EventFieldOffset);
		}
		if (!EventField)
		{
			WarnOnce(FunctionOwner, BoundName, Binding.FieldName,
				TEXT("the event data has no such field — the row is skipped"));
			return true;
		}
		if (!EventData.IsValid() || EventData.GetScriptStruct() != EventDataType)
		{
			// Nothing to read from; the authored value stands. WriteToChannel has already said why.
			return true;
		}
		SourceProp = EventField;
		SourceAddr = static_cast<const uint8*>(EventData.GetMemory()) + EventFieldOffset;
	}
	else
	{
		if (!FunctionOwner)
		{
			return true;
		}
		const UClass* OwnerClass = FunctionOwner->GetClass();
		if (!Binding.FunctionCache.TryGet(OwnerClass, Binding.BoundFunction, EventDataType,
				ENDCVariableType::Unsupported, nullptr, Func, ContextType))
		{
			Func = Binding.BoundFunction.IsNone() ? nullptr : OwnerClass->FindFunctionByName(Binding.BoundFunction);
			if (!IsValidContextFieldFunction(Func, Field))
			{
				WarnOnce(FunctionOwner, Binding.BoundFunction, Binding.FieldName, DescribeBindingFailure(Func));
				Func = nullptr;
			}
			Binding.FunctionCache.Store(OwnerClass, Binding.BoundFunction, EventDataType,
				ENDCVariableType::Unsupported, nullptr, Func, ContextType);
		}
		if (!Func)
		{
			return true;
		}
	}

	// Held across the whole body: a returned value lives inside this block, and the block dies with
	// the scope. Constructing it with a null function is a no-op, which is what the event data path
	// leaves it as.
	const FBoundFunctionCall Call(FunctionOwner, Func, EventData);
	if (!bFromEventData)
	{
		SourceProp = Call.GetReturnProperty();
		SourceAddr = Call.GetReturnValue();
	}
	if (!SourceAddr)
	{
		return true;
	}

	// A null object answer is "no opinion", and what that means is the row's to say. Tested on the
	// source value, before anything is stored, so declining costs no write at all and so a no-opinion
	// answer leaves both the field and its enable flag exactly as authored.
	if (IsNullObjectReturn(SourceProp, SourceAddr))
	{
		// The write declines itself: no weapon to attach to, no target to bucket at. This is the whole
		// reason a row can be marked required, and it is what the removed whole-context binding was
		// doing when it handed back an invalid context.
		return !Binding.bRequired;
	}

	void* FieldAddr = Field->ContainerPtrToValuePtr<void>(ContextMemory);
	if (!AssignReturnToField(SourceProp, SourceAddr, Field, FieldAddr))
	{
		// This is where a type mismatch surfaces, for both kinds of row: the store refuses and says
		// so, rather than a separate check asking the same question first. A function row will
		// normally have been caught by IsValidContextFieldFunction long before reaching here; an
		// event data row is checked only here, which is the whole saving.
		WarnOnce(FunctionOwner, BoundName, Binding.FieldName,
			TEXT("the bound value's type does not fit this context field — the row is skipped"));
		return true;
	}

	// A field gated behind a flag does nothing until that flag is set, and whose decision that is
	// depends on whether it is a decision anyone could have made.
	//
	// An authorable gate is the author's: it is a checkbox on the field's own row in the panel, and a
	// write that set it behind their back would make that checkbox a lie. Binding a field whose gate
	// is clear therefore writes the field and changes nothing — which the panel marks, so it is not
	// silent.
	//
	// A Transient gate is nobody's: it does not serialize, so there is no checkbox and no way to
	// author it. FNDCAccessContext::Location works this way — GetLocation() returns the owning
	// component's location unless bOverrideLocation is set, and bOverrideLocation cannot be saved.
	// The write is the only thing that can set it, so it does, per write.
	//
	// Resolved once per context type, down to the single question the write has to ask: is there a
	// flag here that this write is the one to set. "No flag", "not a bool" and "the author's to set"
	// are all remembered as null, so the write path is one pointer test rather than a name walk, a
	// cast and a flag test.
	const FProperty* TransientFlag = nullptr;
	if (!Binding.TransientFlagCache.TryGet(ContextType, TransientFlag))
	{
		const FBoolProperty* Flag = Binding.EnableFlagField.IsNone()
			? nullptr
			: CastField<FBoolProperty>(ContextType->FindPropertyByName(Binding.EnableFlagField));
		TransientFlag = (Flag && Flag->HasAnyPropertyFlags(CPF_Transient)) ? Flag : nullptr;
		Binding.TransientFlagCache.Store(ContextType, TransientFlag);
	}
	if (TransientFlag)
	{
		const FBoolProperty* Flag = CastFieldChecked<const FBoolProperty>(TransientFlag);
		Flag->SetPropertyValue(Flag->ContainerPtrToValuePtr<void>(ContextMemory), true);
	}
	return true;
}

bool FNDCBinder::ResolveAccessContext(FNDCAccessContextInst& Context, const UObject* FunctionOwner, FConstStructView EventData) const
{
	UNiagaraDataChannel* Channel = GetChannel();
	if (!Channel)
	{
		Context.Reset();
		return false;
	}

	// Start from the authored default, of the channel's own type. Seeding from it is a property-wise
	// copy into the allocation the context already has, so this stays allocation-free — and everything
	// an author set in the panel is already in place before any bound function runs.
	//
	// A default of the wrong type is what a channel swap leaves behind until the editor re-syncs it;
	// falling back to a clean context of the right type keeps the write correct in the meantime.
	const TNDCAccessContextType ContextType = Channel->GetAccessContextType();
	if (DefaultAccessContext.GetScriptStruct() == ContextType.Get())
	{
		Context.AccessContext.InitializeAs(ContextType.Get(), DefaultAccessContext.AccessContext.GetMemory());
	}
	else
	{
		Context.Init(ContextType);
	}
	if (!Context.IsValid())
	{
		return false;
	}


	for (const FNDCContextBinding& Binding : ContextBindings)
	{
		if (!ApplyContextBinding(Binding, Context, FunctionOwner, EventData))
		{
			Context.Reset();
			return false;
		}
	}
	return true;
}


bool FNDCBinder::WriteToChannel(UWorld* World, const UObject* FunctionOwner, FConstStructView EventData) const
{
	// One element is a batch of one. Everything below it — the checks, the guard, the scratch context,
	// the scope — is the same work either way, and having two copies of it is how the two forms would
	// eventually stop agreeing about what a write does.
	return WriteToChannel(World, FunctionOwner, MakeArrayView(&EventData, 1));
}

bool FNDCBinder::WriteToChannel(UWorld* World, const UObject* FunctionOwner, TConstArrayView<FConstStructView> EventData) const
{
	using namespace NDCBinderPrivate;

	UNiagaraDataChannel* Channel = GetChannel();
	if (!World)
	{
		return false;
	}
	// Nothing to write is not a failure worth a line in the log: a caller handing this an empty array
	// is a shot that hit nothing, not a writer that is set up wrong.
	if (EventData.IsEmpty())
	{
		return false;
	}
	// Not an assert: ProcessEvent and Niagara both check this, but only outside Shipping, and a write
	// arriving from a worker thread would corrupt shared state there with nothing to show for it.
	if (!IsInGameThread())
	{
		WarnOnce(FunctionOwner, NAME_None, TEXT("Thread"), TEXT("WriteToChannel must be called on the game thread"));
		return false;
	}
	// ProcessEvent silently returns on a garbage owner (IsValidChecked, in every configuration), which
	// would leave every bound row holding its default-constructed return value and nothing in the log
	// to say why the effect came out wrong. A torn-down owner has no payload worth writing, so the
	// write is skipped and said out loud instead.
	if (FunctionOwner && !IsValid(FunctionOwner))
	{
		WarnOnce(FunctionOwner, NAME_None, TEXT("Owner"), TEXT("the function owner is pending kill or garbage — the write is skipped"));
		return false;
	}
	if (!Channel)
	{
		// Silence here is the classic "why is nothing happening" — an embedded writer with no channel
		// assigned is always a setup mistake, never an intentional no-op.
		WarnOnce(FunctionOwner, NAME_None, TEXT("DataChannel"), TEXT("no Data Channel assigned — nothing is written"));
		return false;
	}
	//~ The first element speaks for the batch: a batch is one payload shape by construction, and every
	//~ row re-checks the type of the element it is actually reading anyway.
	if (EventData[0].IsValid() && EventData[0].GetScriptStruct() != EventDataType)
	{
		WarnOnce(FunctionOwner, NAME_None, TEXT("EventDataType"), TEXT("the event data passed to WriteToChannel is not the owner's declared EventDataType — bound functions will get a default-constructed one"));
	}

	// Armed here rather than around the write itself, because the shared state this protects is in
	// use from the line below onwards: ResolveAccessContext runs author
	// code, and the context they run against is the channel's single scratch instance. A nested write
	// would call GetTransientAccessContext, which Init()s that instance — clearing it out from under
	// the call that is still filling it.
	if (CannotWriteChannelNow(Channel))
	{
		WarnOnce(FunctionOwner, NAME_None, TEXT("Reentrancy"),
			TEXT("a bound function started another write to the channel this one is still filling — the nested write is skipped"));
		return false;
	}
	const FChannelWriteScope WriteScope(Channel);

	// The channel's own scratch context, so no path here allocates one: FInstancedStruct::InitializeAs
	// reuses its allocation when the type already matches, and every context row writes its field in
	// place. Borrowing it is only safe because the context is used and finished with inside this
	// call — the channel hands the same one to the next caller.
	//
	// Not const: the channel writes its results back through the context. This entry point does not
	// surface them — a caller who wants them drives ResolveAccessContext + WriteWithContext itself,
	// passing a context of their own.
	// Resolved from the FIRST element, and it is the same context for all of them — not a shortcut but
	// the shape of the thing: Niagara takes one access context per write, so a batch is by definition a
	// set of elements that share where the write goes and what it attaches to. Elements that do not
	// share that are separate writes. The context rows therefore run ONCE for the whole batch, which is
	// a saving the per-element rows do not get.
	FNDCAccessContextInst& AccessContext = Channel->GetTransientAccessContext();
	if (!ResolveAccessContext(AccessContext, FunctionOwner, EventData[0]))
	{
		return false;
	}

	// The unguarded form: this call already holds the guard.
	return WriteContextAndBindings(World, AccessContext, FunctionOwner, EventData);
}

FNDCWriteScope::~FNDCWriteScope()
{
	End();
}

void FNDCWriteScope::End()
{
	if (bGuardHeld)
	{
		// Order matters only in that both must happen: the buffer is released back to Niagara, and the
		// channel stops counting as being written so the next write on it is not refused as nested.
		EndWrite();
		NDCBinderPrivate::PopWritingChannel();
		bGuardHeld = false;
	}
}

bool FNDCBinder::BeginWrite(FNDCWriteScope& Scope, UWorld* World, FNDCAccessContextInst& AccessContext, int32 Count, const UObject* DebugNameOwner) const
{
	using namespace NDCBinderPrivate;

	// The same three questions WriteToChannel asks, because this is the other way in and the code it
	// leads to is the same. Unlike the old two-step form, this one can hold the guard: the scope the
	// caller declares owns it, and their write loop runs inside its lifetime.
	if (!IsInGameThread())
	{
		WarnOnce(DebugNameOwner, NAME_None, TEXT("Thread"), TEXT("BeginWrite must be called on the game thread"));
		return false;
	}
	if (DebugNameOwner && !IsValid(DebugNameOwner))
	{
		WarnOnce(DebugNameOwner, NAME_None, TEXT("Owner"), TEXT("the function owner is pending kill or garbage — the write is skipped"));
		return false;
	}
	if (CannotWriteChannelNow(GetChannel()))
	{
		WarnOnce(DebugNameOwner, NAME_None, TEXT("Reentrancy"),
			TEXT("a write to this channel is already in progress — the nested write is skipped"));
		return false;
	}
	return BeginWriteInternal(Scope, World, AccessContext, Count, DebugNameOwner);
}

bool FNDCBinder::BeginWriteInternal(FNDCWriteScope& Scope, UWorld* World, FNDCAccessContextInst& AccessContext, int32 Count, const UObject* DebugNameOwner) const
{
	using namespace NDCBinderPrivate;

	UNiagaraDataChannel* Channel = GetChannel();
	if (!Channel)
	{
		// Word for word what WriteToChannel says, because BeginWrite and WriteWithContext reach here
		// too and an unassigned channel is a setup mistake from any of them. Saying it on one entry
		// point and not the others is how the same mistake became findable by one caller and silent
		// for the next.
		WarnOnce(DebugNameOwner, NAME_None, TEXT("DataChannel"), TEXT("no Data Channel assigned — nothing is written"));
		return false;
	}

	// An invalid context is the deliberate "skip the write" signal from ResolveAccessContext.
	if (!World || !AccessContext.IsValid() || Count <= 0)
	{
		return false;
	}

	// Only where something reads it; see the cache members for which two things those are and why
	// neither survives Shipping. The assignment is not free — Scope is fresh per write, so its
	// FString starts empty and every copy into it allocates.
#if !UE_BUILD_SHIPPING
	if (!bDebugNameCached || CachedDebugNameOwner.Get() != DebugNameOwner)
	{
		CachedDebugName = GetNameSafe(DebugNameOwner);
		CachedDebugNameOwner = DebugNameOwner;
		bDebugNameCached = true;
	}
	Scope.DebugSource = CachedDebugName;
#endif

	// The context goes in by reference and comes back written to — the handler records the systems this
	// write spawned or joined in it — which is why nothing here copies it.
	if (!Scope.BeginWrite(World, Channel, AccessContext, Count, bVisibleToGame, bVisibleToCPU, bVisibleToGPU))
	{
		return false;
	}

	// Held from here, so a bound function that starts another write on this channel is refused while
	// this one still owns a slice of its buffer.
	PushWritingChannel(Channel);
	Scope.bGuardHeld = true;

	// The rows' buffer indices, resolved against whatever layout this write landed on. Niagara hands
	// out a new layout when the channel's variables change, so comparing the pointer is the whole test.
	const FNiagaraDataChannelLayoutInfoPtr Layout = Channel->GetLayoutInfo();
	if (CachedLayout != Layout || VarOffsets.Num() != Bindings.Num())
	{
		ResolveVarOffsets(Layout);
	}
	return true;
}

void FNDCBinder::ResolveVarOffsets(const FNiagaraDataChannelLayoutInfoPtr& Layout) const
{
	using namespace NDCBinderPrivate;

	CachedLayout = Layout;
	VarOffsets.Reset();
	VarOffsets.AddUninitialized(Bindings.Num());

	const FNiagaraDataChannelGameDataLayout* GameDataLayout = Layout.IsValid() ? &Layout->GetGameDataLayout() : nullptr;
	for (int32 RowIndex = 0; RowIndex < Bindings.Num(); ++RowIndex)
	{
		const FNDCVariableBinding& Binding = Bindings[RowIndex];
		VarOffsets[RowIndex] = INDEX_NONE;
		if (!GameDataLayout || Binding.VarName.IsNone() || Binding.Type == ENDCVariableType::Unsupported)
		{
			continue;
		}

		// The same question FindVariableBuffer answers per write, asked once: is there a variable of
		// this name whose type this row can write, and where does it live. Its enum concession is kept
		// — a channel may declare an enum where the writer sends an int — because dropping it here
		// would silently stop enum rows working.
		//
		// A name that matches with the wrong type is not the answer, so the scan goes on rather than
		// giving up on it. Two variables CAN share a name: the channel asset makes names unique when
		// one is added or duplicated, but not when one is renamed onto another. And the layout is a
		// TMap, so which of the two a scan meets first is hash order rather than authoring order —
		// stopping at the first name would leave one of the two rows silently unresolved, and which
		// one would not be stable. FindVariableBuffer scans the whole map; matching it is the point.
		const FNiagaraTypeDefinition WantedType = FNDCVariableBinding::NiagaraTypeFromVariableType(Binding.Type);
		for (const TPair<FNiagaraVariableBase, int32>& Pair : GameDataLayout->VariableIndices)
		{
			if (Pair.Key.GetName() != Binding.VarName)
			{
				continue;
			}
			const FNiagaraTypeDefinition& LayoutType = Pair.Key.GetType();
			if (WantedType == LayoutType
				|| (LayoutType.IsEnum() && WantedType == FNiagaraTypeDefinition::GetIntDef()))
			{
				VarOffsets[RowIndex] = Pair.Value;
				break;
			}
		}
	}
}

bool FNDCBinder::WriteWithContext(UWorld* World, FNDCAccessContextInst& AccessContext, const UObject* FunctionOwner, FConstStructView EventData) const
{
	using namespace NDCBinderPrivate;

	// Every reflected call below runs arbitrary author code, so a write can be re-entered — a bound
	// getter that triggers another cue, say. Niagara hands out one shared writer per channel handler
	// (UNiagaraDataChannelHandler::GetDataChannelWriter) and one shared scratch access context per
	// channel, so a nested write would call BeginWrite on the very object the outer loop is still
	// filling. Dropping the inner write keeps the outer one whole, which is the failure an author can
	// actually see and fix.
	if (!IsInGameThread())
	{
		WarnOnce(FunctionOwner, NAME_None, TEXT("Thread"), TEXT("WriteWithContext must be called on the game thread"));
		return false;
	}
	if (FunctionOwner && !IsValid(FunctionOwner))
	{
		WarnOnce(FunctionOwner, NAME_None, TEXT("Owner"), TEXT("the function owner is pending kill or garbage — the write is skipped"));
		return false;
	}
	const UNiagaraDataChannel* Channel = GetChannel();
	if (CannotWriteChannelNow(Channel))
	{
		WarnOnce(FunctionOwner, NAME_None, TEXT("Reentrancy"),
			TEXT("a bound function started another write to the channel this one is still filling — the nested write is skipped"));
		return false;
	}
	const FChannelWriteScope WriteScope(Channel);

	return WriteContextAndBindings(World, AccessContext, FunctionOwner, MakeArrayView(&EventData, 1));
}

bool FNDCBinder::WriteContextAndBindings(UWorld* World, FNDCAccessContextInst& AccessContext, const UObject* FunctionOwner, TConstArrayView<FConstStructView> EventData) const
{
	// The unchecked pair: this call already holds the channel's write scope, and asking the public
	// forms would have them refuse the write for exactly that reason. The scope ends with this frame,
	// which is what hands the buffer back to Niagara.
	FNDCWriteScope Scope;
	if (!BeginWriteInternal(Scope, World, AccessContext, EventData.Num(), FunctionOwner))
	{
		return false;
	}
	// One pass per element, each with its own event data: that is the only thing that differs between
	// them, and it is what the rows read.
	for (int32 Index = 0; Index < EventData.Num(); ++Index)
	{
		WriteBindingsInternal(Scope, Index, FunctionOwner, EventData[Index]);
	}
	return true;
}

void FNDCBinder::WriteBindings(FNDCWriteScope& Scope, int32 Index, const UObject* FunctionOwner, FConstStructView EventData) const
{
	using namespace NDCBinderPrivate;

	// Every row here may call into author code on FunctionOwner and touch this writer's transient
	// caches, so the public form asks what WriteToChannel asks before any of that happens.
	if (!IsInGameThread())
	{
		WarnOnce(FunctionOwner, NAME_None, TEXT("Thread"), TEXT("WriteBindings must be called on the game thread"));
		return;
	}
	if (FunctionOwner && !IsValid(FunctionOwner))
	{
		WarnOnce(FunctionOwner, NAME_None, TEXT("Owner"), TEXT("the function owner is pending kill or garbage — the rows are skipped"));
		return;
	}
	WriteBindingsInternal(Scope, Index, FunctionOwner, EventData);
}

void FNDCBinder::WriteBindingsInternal(FNDCWriteScope& Scope, int32 Index, const UObject* FunctionOwner, FConstStructView EventData) const
{
	using namespace NDCBinderPrivate;

	const FNiagaraDataChannelGameDataPtr& Data = Scope.GetData();
	if (!Scope.IsWriting() || !Data.IsValid() || VarOffsets.Num() != Bindings.Num())
	{
		return;
	}
	// Where this write's slice of the shared buffer starts. Niagara appends, so index 0 of this write
	// is not index 0 of the buffer.
	const int32 BufferIndex = Scope.GetStartIndex() + Index;

	for (int32 RowIndex = 0; RowIndex < Bindings.Num(); ++RowIndex)
	{
		const FNDCVariableBinding& Binding = Bindings[RowIndex];
		// The row's place in the buffer, resolved once per channel layout. INDEX_NONE is a row the
		// channel has no variable for — a departed one, or one whose type no longer matches.
		const int32 VarOffset = VarOffsets[RowIndex];
		if (VarOffset == INDEX_NONE || Binding.VarName.IsNone())
		{
			continue;
		}

		// Function-sourced rows call out to the owner and the returned value stays alive in the call
		// scope until the end of this iteration; constant rows leave the call empty.
		UFunction* Func = nullptr;
		if (Binding.Source == ENDCValueSource::Function)
		{
			Func = FindValueFunction(*this, Binding, FunctionOwner);
			if (!Func)
			{
				continue;
			}
		}

		const FBoundFunctionCall Call(FunctionOwner, Func, EventData);
		const void* ReturnAddr = Call.GetReturnValue();
		const FProperty* ReturnProp = Call.GetReturnProperty();

		// An event data row reads the member instead, into the same pair the cases below already
		// consume — no call, no parameter block, nothing to destroy. The type is re-checked rather
		// than trusted from the picker, for the same reason every other binding is: the row stores a
		// bare name and the struct it names can change underneath it.
		if (Binding.Source == ENDCValueSource::EventData)
		{
			// Both halves — the name walk and the type check — answer the same thing every write for a
			// given event data struct, and the row is skipped on either. So they run once and what is
			// remembered is the usable field, or null for "skip this row", which the warning above has
			// already explained by then.
			const FProperty* Field = nullptr;
			int32 FieldOffset = 0;
			if (!Binding.EventFieldCache.TryGet(EventDataType, Field, Binding.Type, Binding.EnumDef, &FieldOffset))
			{
				if (!ResolveFieldPath(EventDataType, Binding.BoundEventDataField, Field, FieldOffset)
					|| !IsPropertyCompatibleWithVariableType(Field, Binding.Type, Binding.EnumDef))
				{
					WarnOnce(FunctionOwner, Binding.BoundEventDataField, Binding.VarName,
						Field ? TEXT("the event data field's type does not match the channel variable — the row is skipped")
							  : TEXT("the event data has no such field — the row is skipped"));
					Field = nullptr;
					FieldOffset = 0;
				}
				Binding.EventFieldCache.Store(EventDataType, Field, Binding.Type, Binding.EnumDef, FieldOffset);
			}
			if (!Field)
			{
				continue;
			}
			if (!EventData.IsValid() || EventData.GetScriptStruct() != EventDataType)
			{
				// Nothing to read from. WriteToChannel already warned about the mismatch; leaving the
				// row's constant standing beats writing a value read out of the wrong struct.
				continue;
			}
			ReturnProp = Field;
			// The resolved offset, not ContainerPtrToValuePtr: for a nested path the leaf's own offset
			// is measured from the struct it sits in, not from the event data.
			ReturnAddr = static_cast<const uint8*>(EventData.GetMemory()) + FieldOffset;
		}

		// Each case starts from the binding's constant and lets a function return replace it.
		switch (Binding.Type)
		{
		case ENDCVariableType::Bool:
			{
				bool Value = Binding.BoolValue;
				if (ReturnAddr) { ReadReturn(ReturnProp, ReturnAddr, Value); }
				Data->Write<FNiagaraBool>(VarOffset, BufferIndex, FNiagaraBool(Value));
				break;
			}
		case ENDCVariableType::Int32:
			{
				int32 Value = Binding.IntValue;
				if (ReturnAddr) { ReadReturn(ReturnProp, ReturnAddr, Value); }
				Data->Write<int32>(VarOffset, BufferIndex, Value);
				break;
			}
		case ENDCVariableType::Float:
			{
				double Value = Binding.FloatValue;
				if (ReturnAddr) { ReadReturn(ReturnProp, ReturnAddr, Value); }
				Data->Write<double>(VarOffset, BufferIndex, Value);
				break;
			}
		case ENDCVariableType::Vector2D:
			{
				FVector2D Value = Binding.Vector2DValue;
				if (ReturnAddr) { ReadReturn(ReturnProp, ReturnAddr, Value); }
				Data->Write<FVector2D>(VarOffset, BufferIndex, Value);
				break;
			}
		case ENDCVariableType::Vector:
			{
				FVector Value = Binding.VectorValue;
				if (ReturnAddr) { ReadReturn(ReturnProp, ReturnAddr, Value); }
				Data->Write<FVector>(VarOffset, BufferIndex, Value);
				break;
			}
		case ENDCVariableType::Vector4:
			{
				FVector4 Value = Binding.Vector4Value;
				if (ReturnAddr) { ReadReturn(ReturnProp, ReturnAddr, Value); }
				Data->Write<FVector4>(VarOffset, BufferIndex, Value);
				break;
			}
		case ENDCVariableType::Quat:
			{
				FQuat Value = Binding.QuatValue;
				if (ReturnAddr) { ReadReturn(ReturnProp, ReturnAddr, Value); }
				Data->Write<FQuat>(VarOffset, BufferIndex, Value);
				break;
			}
		case ENDCVariableType::LinearColor:
			{
				FLinearColor Value = Binding.ColorValue;
				if (ReturnAddr) { ReadReturn(ReturnProp, ReturnAddr, Value); }
				Data->Write<FLinearColor>(VarOffset, BufferIndex, Value);
				break;
			}
		case ENDCVariableType::Position:
			{
				FVector Value = Binding.VectorValue;
				if (ReturnAddr) { ReadReturn(ReturnProp, ReturnAddr, Value); }
				Data->Write<FVector>(VarOffset, BufferIndex, Value);
				break;
			}
		case ENDCVariableType::Enum:
			{
				uint8 Value = Binding.EnumValue;
				if (ReturnAddr) { ReadReturn(ReturnProp, ReturnAddr, Value); }
				Data->Write<int32>(VarOffset, BufferIndex, (int32)Value);
				break;
			}
		//~ No constant editor for these two: an unbound row writes nothing so the channel default stands.
		case ENDCVariableType::SpawnInfo:
			{
				if (!ReturnAddr) { break; }
				FNiagaraSpawnInfo Value;
				ReadReturn(ReturnProp, ReturnAddr, Value);
				Data->Write<FNiagaraSpawnInfo>(VarOffset, BufferIndex, Value);
				break;
			}
		case ENDCVariableType::ID:
			{
				if (!ReturnAddr) { break; }
				FNiagaraID Value;
				ReadReturn(ReturnProp, ReturnAddr, Value);
				Data->Write<FNiagaraID>(VarOffset, BufferIndex, Value);
				break;
			}
		default:
			break;
		}
	}
}

#if WITH_EDITOR
namespace NDCBinderPrivate
{
	static TSet<FName> GatherChannelVarNames(const UNiagaraDataChannel* Channel)
	{
		TSet<FName> Names;
		for (const FNiagaraDataChannelVariable& Var : Channel->GetVariables())
		{
			if (Var.IsValid())
			{
				Names.Add(Var.GetName());
			}
		}
		return Names;
	}

	/**
	 * What "stale" means, stated once.
	 *
	 * A payload row is stale when the channel has no variable of that name; a context row is stale
	 * when the channel's access context has no such field. Both PruneStaleBindings and the test that
	 * predicts it read these, rather than each spelling the rule out — the panel's warning and the
	 * removal it offers have to be the same claim, and two copies of a rule are two chances to drift.
	 */
	static bool IsStaleVariableRow(const FNDCVariableBinding& Row, const TSet<FName>& ChannelVarNames)
	{
		return !ChannelVarNames.Contains(Row.VarName);
	}

	static bool IsStaleContextRow(const FNDCContextBinding& Row, const UScriptStruct* ContextType)
	{
		return !ContextType || !ContextType->FindPropertyByName(Row.FieldName);
	}

	/**
	 * Records, on each context row, the flag its field is gated behind.
	 *
	 * Only records. The flag itself is the author's checkbox and is left exactly as they set it —
	 * a row that binds a field whose gate is clear writes the field and changes nothing, and the
	 * panel marks that rather than quietly fixing it.
	 *
	 * Re-resolved on every sync rather than only when a row is created, so a row authored before the
	 * pairing was understood — or against a channel whose context has since changed — picks up the
	 * right flag instead of silently keeping a stale one.
	 *
	 * Returns true when anything changed.
	 */
	static bool SyncContextBindingFlags(const UScriptStruct* ContextType, TArray<FNDCContextBinding>& Rows)
	{
		if (!ContextType)
		{
			return false;
		}

		bool bChanged = false;
		for (FNDCContextBinding& Row : Rows)
		{
			const FProperty* Field = ContextType->FindPropertyByName(Row.FieldName);
			if (!Field)
			{
				continue;
			}
			const FName Flag = FNDCBinder::ResolveContextFieldEnableFlag(ContextType, Field);
			if (Row.EnableFlagField != Flag)
			{
				Row.EnableFlagField = Flag;
				bChanged = true;
			}
		}
		return bChanged;
	}

	/** Builds what Bindings should look like for Channel; returns true when that differs from Bindings. */
	static bool BuildSyncedBindings(const UNiagaraDataChannel* Channel, const TArray<FNDCVariableBinding>& Bindings, TArray<FNDCVariableBinding>& OutSynced)
	{
		OutSynced.Reset(Bindings.Num());

		// Channel-variable order first, carrying each existing row's data over.
		TSet<FName> SeenNames;
		for (const FNiagaraDataChannelVariable& Var : Channel->GetVariables())
		{
			if (!Var.IsValid())
			{
				continue;
			}

			const FName VarName = Var.GetName();
			bool bAlreadySeen = false;
			SeenNames.Add(VarName, &bAlreadySeen);
			if (bAlreadySeen)
			{
				continue;
			}

			const FNDCVariableBinding* Existing = Bindings.FindByPredicate([VarName](const FNDCVariableBinding& B) { return B.VarName == VarName; });
			FNDCVariableBinding& Row = OutSynced.Add_GetRef(Existing ? *Existing : FNDCVariableBinding());
			Row.VarName = VarName;
			Row.Type = FNDCVariableBinding::VariableTypeFromNiagaraType(Var.GetType());
			Row.EnumDef = Row.Type == ENDCVariableType::Enum ? Var.GetType().GetEnum() : nullptr;
		}

		// Rows for variables the channel no longer has keep their data at the end (never destructive).
		for (const FNDCVariableBinding& Binding : Bindings)
		{
			bool bAlreadySeen = false;
			SeenNames.Add(Binding.VarName, &bAlreadySeen);
			if (!bAlreadySeen)
			{
				OutSynced.Add(Binding);
			}
		}

		// Only the channel-derived fields and the row order can change here — everything else is copied.
		if (OutSynced.Num() != Bindings.Num())
		{
			return true;
		}
		for (int32 RowIndex = 0; RowIndex < OutSynced.Num(); ++RowIndex)
		{
			if (OutSynced[RowIndex].VarName != Bindings[RowIndex].VarName
				|| OutSynced[RowIndex].Type != Bindings[RowIndex].Type
				|| OutSynced[RowIndex].EnumDef != Bindings[RowIndex].EnumDef)
			{
				return true;
			}
		}
		return false;
	}
}

bool FNDCBinder::SyncBindingsWithChannel()
{
	UNiagaraDataChannel* Channel = GetChannel();
	if (!Channel)
	{
		return false;
	}

	bool bChanged = false;

	// The default context's type is the channel's, never a choice. Retyping it here is what keeps a
	// picker off the panel; Init reuses the allocation when the type already matches.
	const TNDCAccessContextType ContextType = Channel->GetAccessContextType();
	if (DefaultAccessContext.GetScriptStruct() != ContextType.Get())
	{
		DefaultAccessContext.Init(ContextType);
		bChanged = true;
	}


	bChanged |= NDCBinderPrivate::SyncContextBindingFlags(ContextType.Get(), ContextBindings);

	TArray<FNDCVariableBinding> Synced;
	if (NDCBinderPrivate::BuildSyncedBindings(Channel, Bindings, Synced))
	{
		Bindings = MoveTemp(Synced);
		bChanged = true;
	}
	return bChanged;
}

bool FNDCBinder::NeedsBindingSync() const
{
	const UNiagaraDataChannel* Channel = GetChannel();
	if (!Channel)
	{
		return false;
	}
	if (DefaultAccessContext.GetScriptStruct() != Channel->GetAccessContextType().Get())
	{
		return true;
	}

	// Asked of a copy rather than reimplemented as a read-only test: a dry run that answers by a
	// different route than the run it predicts is how the two stop agreeing.
	TArray<FNDCContextBinding> ContextRows = ContextBindings;
	if (NDCBinderPrivate::SyncContextBindingFlags(Channel->GetAccessContextType().Get(), ContextRows))
	{
		return true;
	}

	TArray<FNDCVariableBinding> Synced;
	return NDCBinderPrivate::BuildSyncedBindings(Channel, Bindings, Synced);
}

bool FNDCBinder::PruneStaleBindings()
{
	const UNiagaraDataChannel* Channel = GetChannel();
	if (!Channel)
	{
		return false;
	}

	// A context row survives the switch only if the new channel's context still has that field. The
	// same reasoning as the payload rows: a row naming a field this context does not have is not a
	// binding an author could fix, it is one they made against a channel they have now left.
	const TSet<FName> ChannelVarNames = NDCBinderPrivate::GatherChannelVarNames(Channel);
	const UScriptStruct* ContextType = Channel->GetAccessContextType().Get();

	bool bRemoved = Bindings.RemoveAll([&ChannelVarNames](const FNDCVariableBinding& B)
		{ return NDCBinderPrivate::IsStaleVariableRow(B, ChannelVarNames); }) > 0;
	bRemoved |= ContextBindings.RemoveAll([ContextType](const FNDCContextBinding& B)
		{ return NDCBinderPrivate::IsStaleContextRow(B, ContextType); }) > 0;

	return bRemoved;
}

bool FNDCBinder::HasStaleBindings() const
{
	// Asked of the count rather than answered again: rows are few, and a second reading of the rule
	// is what the shared predicates exist to stop.
	return CountStaleBindings() > 0;
}

int32 FNDCBinder::CountStaleBindings() const
{
	const UNiagaraDataChannel* Channel = GetChannel();
	if (!Channel)
	{
		return 0;
	}

	// The same two predicates PruneStaleBindings removes by, not a second reading of them: a test that
	// predicts a change by a different route than the change itself is how the two stop agreeing.
	// NeedsBindingSync answers its own question by dry-running the sync for exactly this reason.
	const TSet<FName> ChannelVarNames = NDCBinderPrivate::GatherChannelVarNames(Channel);
	const UScriptStruct* ContextType = Channel->GetAccessContextType().Get();

	int32 NumStale = 0;
	for (const FNDCVariableBinding& Row : Bindings)
	{
		NumStale += NDCBinderPrivate::IsStaleVariableRow(Row, ChannelVarNames) ? 1 : 0;
	}
	for (const FNDCContextBinding& Row : ContextBindings)
	{
		NumStale += NDCBinderPrivate::IsStaleContextRow(Row, ContextType) ? 1 : 0;
	}
	return NumStale;
}
#endif
