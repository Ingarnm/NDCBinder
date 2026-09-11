// NDCBinderTests.cpp
//
// Covers the reflection layer of FNDCBinder: which function signatures it accepts for a
// binding, and what a bound function actually receives when it is called.
//
// NOT covered here, and worth knowing:
//
//  - a Blueprint override whose UFunction carries local variables after its parameters. Those only
//    exist on Blueprint-compiled functions, so a native test cannot produce one. The guard is
//    structural instead — every walk over a UFunction's properties goes through one helper that
//    stops at the first non-parameter (see ForEachParm in NDCBinder.cpp); reading past it
//    wrote off the end of the parameter block.
//
//  - the cache surviving a Blueprint recompile. Recompiling cleans the UClass in place and re-outers
//    its old UFunctions into a TRASHCLASS, which needs a real Blueprint asset and a compile to
//    reproduce. FNDCBoundFunctionCache answers it by re-checking that the remembered function is
//    still owned by the class it was remembered for.
//
//  - the Networked and DelegateSignature rejections. UHT will not let a replicated function have a
//    return value and a delegate signature is not declared as a class function, so neither can be
//    authored on a test host at all; both are structural guards on the reflection path.

#include "NDCBinderTestTypes.h"

#include "Misc/AutomationTest.h"
#include "Misc/DataValidation.h"
#include "NDCBinder.h"
#include "NDCBinderLibrary.h"
#include "NiagaraTypes.h"
#include "StructUtils/StructView.h"
#include "GameFramework/Actor.h"
#include "NiagaraDataChannel.h"
#include "NiagaraDataChannelAsset.h"
#include "NiagaraDataChannelVariable.h"
#include "NiagaraDataChannel_Global.h"
#include "NiagaraDataChannelGameData.h"
#include "NiagaraWorldManager.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace NDCBinderTestsPrivate
{
	static UFunction* FindTestFunction(const TCHAR* Name)
	{
		return UNDCBinderTestFunctionHost::StaticClass()->FindFunctionByName(FName(Name));
	}

	/** A context of the standard type, so a row can be driven without a channel. */
	static FNDCAccessContextInst MakeContext()
	{
		FNDCAccessContextInst Context;
		Context.AccessContext.InitializeAs(FNDCAccessContext::StaticStruct());
		return Context;
	}

	/** A writer configured the way a host framework would configure it. */
	static FNDCBinder MakeWriter(UScriptStruct* EventDataType)
	{
		FNDCBinder Writer;
		Writer.SetEventDataTypeUnchecked(EventDataType);
		return Writer;
	}

	/** A Data Channel asset with the given variables; see NDCBinderTestChannel::Make. */
	static UNiagaraDataChannelAsset* MakeChannelAsset(TConstArrayView<TPair<FName, FNiagaraTypeDefinition>> Variables)
	{
		return NDCBinderTestChannel::Make(Variables);
	}

	/** The row driving VarName, or nullptr. */
	static const FNDCVariableBinding* FindRow(const FNDCBinder& Writer, FName VarName)
	{
		return Writer.GetBindings().FindByPredicate([VarName](const FNDCVariableBinding& B) { return B.VarName == VarName; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderChannelSyncTest,
	"NDCBinder.ChannelVariables.Sync",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderChannelSyncTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

#if WITH_EDITOR
	const FNiagaraTypeDefinition VecType = FNiagaraTypeDefinition::GetVec3Def();
	const FNiagaraTypeDefinition FloatType = FNiagaraTypeDefinition::GetFloatDef();

	FNDCBinder Writer = MakeWriter(FNDCBinderTestContext::StaticStruct());
	Writer.DataChannel = MakeChannelAsset({ { TEXT("Position"), VecType }, { TEXT("Size"), FloatType } });
	if (!Writer.DataChannel || !Writer.GetChannel())
	{
		AddError(TEXT("could not build a Data Channel asset to sync against"));
		return false;
	}

	// One row per channel variable, in the channel's order, typed from the channel.
	TestTrue(TEXT("an unsynced writer reports that it needs a sync"), Writer.NeedsBindingSync());
	TestTrue(TEXT("the sync reports that it changed something"), Writer.SyncBindingsWithChannel());
	TestEqual(TEXT("one row per channel variable"), Writer.GetBindings().Num(), 2);
	TestFalse(TEXT("a synced writer needs no further sync"), Writer.NeedsBindingSync());
	TestFalse(TEXT("a second sync changes nothing"), Writer.SyncBindingsWithChannel());
	if (Writer.GetBindings().Num() == 2)
	{
		TestEqual(TEXT("rows follow the channel's order"), Writer.GetBindings()[0].VarName, FName(TEXT("Position")));
		TestEqual(TEXT("a vector variable makes a vector row"), Writer.GetBindings()[0].Type, ENDCVariableType::Vector);
		TestEqual(TEXT("a float variable makes a float row"), Writer.GetBindings()[1].Type, ENDCVariableType::Float);
	}

	// What the author put in a row survives a sync; only the channel-derived half is rewritten.
	Writer.GetMutableBindingsUnchecked()[1].Source = ENDCValueSource::EventData;
	Writer.GetMutableBindingsUnchecked()[1].BoundEventDataField = TEXT("Size");
	Writer.GetMutableBindingsUnchecked()[0].VectorValue = FVector(1.0, 2.0, 3.0);
	Writer.SyncBindingsWithChannel();
	TestEqual(TEXT("a bound row keeps its binding across a sync"), FindRow(Writer, TEXT("Size"))->BoundEventDataField, FName(TEXT("Size")));
	TestEqual(TEXT("a constant row keeps its value across a sync"), FindRow(Writer, TEXT("Position"))->VectorValue, FVector(1.0, 2.0, 3.0));

	// A variable removed from the SAME channel keeps its row: the writer is never destructive, and a
	// row that no longer has a variable is reported stale and skipped rather than deleted.
	Writer.DataChannel = MakeChannelAsset({ { TEXT("Position"), VecType } });
	Writer.SyncBindingsWithChannel();
	TestEqual(TEXT("a row for a departed variable is kept"), Writer.GetBindings().Num(), 2);
	TestTrue(TEXT("the departed row is reported stale"), Writer.HasStaleBindings());
	// The number the panel puts on its Remove button has to be the number that button removes, or it
	// offers to do one thing and does another.
	const int32 NumStale = Writer.CountStaleBindings();
	const int32 NumBefore = Writer.GetBindings().Num();
	TestEqual(TEXT("and counted"), NumStale, 1);
	TestTrue(TEXT("pruning removes it"), Writer.PruneStaleBindings());
	TestEqual(TEXT("as many rows as were counted"), NumBefore - Writer.GetBindings().Num(), NumStale);
	TestEqual(TEXT("and only it"), Writer.GetBindings().Num(), 1);
	TestFalse(TEXT("nothing is stale afterwards"), Writer.HasStaleBindings());
	TestEqual(TEXT("and none are counted"), Writer.CountStaleBindings(), 0);

	// The case that made FNDCFieldCache key on the row's type: a sync carries the existing row over
	// and then rewrites Type from the channel, so a row can change what it means without being
	// replaced. Anything remembered against the old type has to stop matching.
	Writer.GetMutableBindingsUnchecked()[0].Source = ENDCValueSource::EventData;
	Writer.GetMutableBindingsUnchecked()[0].BoundEventDataField = TEXT("Location");
	const FProperty* LocationField = Writer.FindEventDataField(TEXT("Location"));
	Writer.GetBindings()[0].EventFieldCache.Store(Writer.GetEventDataType(), LocationField, ENDCVariableType::Vector);

	Writer.DataChannel = MakeChannelAsset({ { TEXT("Position"), FloatType } });
	Writer.SyncBindingsWithChannel();
	TestEqual(TEXT("the row is retyped in place"), Writer.GetBindings()[0].Type, ENDCVariableType::Float);

	const FProperty* Cached = nullptr;
	TestFalse(TEXT("what was remembered for the old type no longer matches"),
		Writer.GetBindings()[0].EventFieldCache.TryGet(Writer.GetEventDataType(), Cached, Writer.GetBindings()[0].Type));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderVarOffsetTest,
	"NDCBinder.ChannelVariables.VarOffsets",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderVarOffsetTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

#if WITH_EDITOR
	// One row of every type the writer can write, so the type table that drives the resolution is
	// exercised entry by entry rather than on whichever type a sample channel happened to use.
	FNDCBinder Writer = MakeWriter(FNDCBinderTestContext::StaticStruct());
	Writer.DataChannel = MakeChannelAsset({
		{ TEXT("Flag"),     FNiagaraTypeDefinition::GetBoolDef() },
		{ TEXT("Count"),    FNiagaraTypeDefinition::GetIntDef() },
		{ TEXT("Size"),     FNiagaraTypeDefinition::GetFloatDef() },
		{ TEXT("Uv"),       FNiagaraTypeDefinition::GetVec2Def() },
		{ TEXT("Velocity"), FNiagaraTypeDefinition::GetVec3Def() },
		{ TEXT("Extra"),    FNiagaraTypeDefinition::GetVec4Def() },
		{ TEXT("Rotation"), FNiagaraTypeDefinition::GetQuatDef() },
		{ TEXT("Tint"),     FNiagaraTypeDefinition::GetColorDef() },
		{ TEXT("Origin"),   FNiagaraTypeDefinition::GetPositionDef() },
		{ TEXT("Id"),       FNiagaraTypeDefinition::GetIDDef() },
	});

	UNiagaraDataChannel* Channel = Writer.GetChannel();
	if (!Channel)
	{
		AddError(TEXT("could not build a Data Channel asset to resolve against"));
		return false;
	}
	Writer.SyncBindingsWithChannel();
	TestEqual(TEXT("one row per channel variable"), Writer.GetBindings().Num(), 10);

	// The claim this whole path rests on: the index resolved once names exactly the buffer the
	// by-name write would have found on every write. Asked of Niagara's own lookup, not of a copy of
	// its logic — if the two ever disagree, rows would write into the wrong variable in silence.
	Writer.ResolveVarOffsets(Channel->GetLayoutInfo());
	TestEqual(TEXT("one offset per row"), Writer.VarOffsets.Num(), Writer.GetBindings().Num());

	const FNiagaraDataChannelGameDataPtr Data = Channel->CreateGameData();
	if (!Data.IsValid())
	{
		AddError(TEXT("the channel produced no game data to resolve against"));
		return false;
	}
	const TConstArrayView<FNiagaraDataChannelVariableBuffer> Buffers = Data->GetVariableBuffers();

	for (int32 RowIndex = 0; RowIndex < Writer.GetBindings().Num(); ++RowIndex)
	{
		const FNDCVariableBinding& Binding = Writer.GetBindings()[RowIndex];
		const int32 Offset = Writer.VarOffsets[RowIndex];
		const FString What = FString::Printf(TEXT("row '%s'"), *Binding.VarName.ToString());

		if (!TestTrue(*(What + TEXT(" resolves to a buffer")), Buffers.IsValidIndex(Offset)))
		{
			continue;
		}
		const FNiagaraVariableBase ByName(FNDCVariableBinding::NiagaraTypeFromVariableType(Binding.Type), Binding.VarName);
		TestEqual(*(What + TEXT(" resolves to the buffer the by-name write would find")),
			(const void*)&Buffers[Offset], (const void*)Data->FindVariableBuffer(ByName));
	}

	// A row the channel has no place for resolves to nothing rather than to some other row's buffer.
	Writer.GetMutableBindingsUnchecked().AddDefaulted_GetRef().VarName = TEXT("NoSuchVariable");
	Writer.GetMutableBindingsUnchecked().Last().Type = ENDCVariableType::Vector;
	Writer.ResolveVarOffsets(Channel->GetLayoutInfo());
	TestEqual(TEXT("a row with no matching variable resolves to INDEX_NONE"), Writer.VarOffsets.Last(), (int32)INDEX_NONE);

	// Two channel variables of one name and different types. The channel asset makes names unique on
	// add and on duplicate but not on rename, so this is authorable; and the layout is a TMap, so
	// which of the two a scan meets first is hash order. Both rows have to land on their own type's
	// buffer whichever comes first — resolution that stopped at the first matching name would leave
	// one of them unresolved, and a run on another machine would pick the other one.
	FNDCBinder Ambiguous = MakeWriter(FNDCBinderTestContext::StaticStruct());
	Ambiguous.DataChannel = MakeChannelAsset({
		{ TEXT("Shared"), FNiagaraTypeDefinition::GetIntDef() },
		{ TEXT("Shared"), FNiagaraTypeDefinition::GetFloatDef() },
	});
	UNiagaraDataChannel* const AmbiguousChannel = Ambiguous.GetChannel();
	const FNiagaraDataChannelGameDataPtr AmbiguousData = AmbiguousChannel ? AmbiguousChannel->CreateGameData() : nullptr;
	if (!AmbiguousData.IsValid())
	{
		AddError(TEXT("could not build a channel carrying two variables of one name"));
		return false;
	}

	// Both rows by hand: a sync collapses them to one, which is right for a panel and wrong for this
	// question. The writer has to answer for a row it is given, however it was given one.
	Ambiguous.GetMutableBindingsUnchecked() =
	{
		FNDCVariableBinding::MakeFunctionBinding(TEXT("Shared"), ENDCVariableType::Int32, NAME_None),
		FNDCVariableBinding::MakeFunctionBinding(TEXT("Shared"), ENDCVariableType::Float, NAME_None),
	};
	Ambiguous.ResolveVarOffsets(AmbiguousChannel->GetLayoutInfo());
	const TConstArrayView<FNiagaraDataChannelVariableBuffer> AmbiguousBuffers = AmbiguousData->GetVariableBuffers();
	for (int32 RowIndex = 0; RowIndex < Ambiguous.GetBindings().Num(); ++RowIndex)
	{
		const FNDCVariableBinding& Binding = Ambiguous.GetBindings()[RowIndex];
		const int32 Offset = Ambiguous.VarOffsets[RowIndex];
		const FString What = FString::Printf(TEXT("the '%s' row typed %s"), *Binding.VarName.ToString(),
			*StaticEnum<ENDCVariableType>()->GetNameStringByValue((int64)Binding.Type));

		if (!TestTrue(*(What + TEXT(" resolves past the same name of another type")), AmbiguousBuffers.IsValidIndex(Offset)))
		{
			continue;
		}
		const FNiagaraVariableBase ByName(FNDCVariableBinding::NiagaraTypeFromVariableType(Binding.Type), Binding.VarName);
		TestEqual(*(What + TEXT(" resolves to the buffer the by-name write would find")),
			(const void*)&AmbiguousBuffers[Offset], (const void*)AmbiguousData->FindVariableBuffer(ByName));
	}
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderFieldCacheTest,
	"NDCBinder.BoundFunctions.FieldCache",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderFieldCacheTest::RunTest(const FString& Parameters)
{
	UScriptStruct* EventStruct = FNDCBinderTestContext::StaticStruct();
	UScriptStruct* OtherStruct = FNDCBinderTestOtherContext::StaticStruct();
	const FProperty* Field = EventStruct->FindPropertyByName(TEXT("Location"));
	if (!Field)
	{
		AddError(TEXT("FNDCBinderTestContext no longer has the field this test caches"));
		return false;
	}

	FNDCFieldCache Cache;
	const FProperty* Out = nullptr;

	TestFalse(TEXT("an empty cache answers nothing"), Cache.TryGet(EventStruct, Out));

	Cache.Store(EventStruct, Field);
	TestTrue(TEXT("what was stored comes back"), Cache.TryGet(EventStruct, Out));
	TestEqual(TEXT("and it is the same property"), Out, Field);

	// Every input the answer depends on is part of the key, so nothing has to remember to invalidate.
	Out = nullptr;
	TestFalse(TEXT("a different struct misses"), Cache.TryGet(OtherStruct, Out));
	TestFalse(TEXT("a different value type misses"), Cache.TryGet(EventStruct, Out, ENDCVariableType::Float));
	TestFalse(TEXT("no struct at all misses"), Cache.TryGet(nullptr, Out));

	// A remembered null is an answer too: a row that resolves to nothing stays cheap to skip.
	FNDCFieldCache MissCache;
	MissCache.Store(EventStruct, nullptr);
	Out = Field;
	TestTrue(TEXT("a remembered miss is an answer"), MissCache.TryGet(EventStruct, Out));
	TestNull(TEXT("and the answer is nothing"), Out);

	// Storing against no struct stores nothing, rather than storing under a null key.
	FNDCFieldCache NullOwnerCache;
	NullOwnerCache.Store(nullptr, Field);
	TestFalse(TEXT("nothing is remembered against a null struct"), NullOwnerCache.TryGet(EventStruct, Out));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderSignatureTest,
	"NDCBinder.BoundFunctions.Signature",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderSignatureTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

	const FNDCBinder Writer = MakeWriter(FNDCBinderTestContext::StaticStruct());

	// The two accepted shapes, both returning FVector for a Vector binding.
	TestTrue(TEXT("T Func() is accepted"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("NoParams")), ENDCVariableType::Vector));
	TestTrue(TEXT("T Func(FEventData) is accepted"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("EventDataOnly")), ENDCVariableType::Vector));

	// Everything else is not a binding target.
	TestFalse(TEXT("an event data parameter of another struct type is rejected"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("EventDataOfWrongType")), ENDCVariableType::Vector));
	TestFalse(TEXT("a second parameter is rejected"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("EventDataThenExtra")), ENDCVariableType::Vector));
	TestFalse(TEXT("a non-struct parameter is rejected"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("NonStructParam")), ENDCVariableType::Vector));

	// By value or by const reference, never by mutable reference. The call path aliases a by-reference
	// parameter to the caller's own event data rather than deep-copying it, so a function that could
	// write through it could reallocate a buffer the caller still owns.
	TestTrue(TEXT("a const reference to the event data is accepted"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("ConstEventDataRef")), ENDCVariableType::Vector));
	TestFalse(TEXT("a mutable reference to the event data is rejected"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("MutableEventDataRef")), ENDCVariableType::Vector));
	TestFalse(TEXT("a void function is rejected"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("NoReturnValue")), ENDCVariableType::Vector));
	TestFalse(TEXT("a missing function is rejected"),
		Writer.IsValidValueFunction(nullptr, ENDCVariableType::Vector));

	// The writer has no notion of a subject actor any more: an actor parameter is just a parameter
	// of a type the event data is not, and is rejected like any other.
	TestFalse(TEXT("an actor parameter is rejected"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("ActorParam")), ENDCVariableType::Vector));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderEventDataParamTest,
	"NDCBinder.BoundFunctions.EventDataParam",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderEventDataParamTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

	// What the editor turns into "declare this struct as Event Data Type and the binding works".
	// It used to answer that from its own copy of the walk below, the copy drifted out of agreement
	// with the signature it was mirroring, and the three diagnostics built on it returned nothing for
	// every possible signature. So this covers both halves: the shapes, and the agreement.
	UScriptStruct* const Poison = FNDCBinderTestOtherContext::StaticStruct();
	UScriptStruct* Out = Poison;

	TestTrue(TEXT("T Func() is a binding shape"),
		FNDCBinder::GetBoundFunctionEventDataStruct(FindTestFunction(TEXT("NoParams")), Out));
	TestNull(TEXT("T Func() declares no event data"), Out);

	Out = Poison;
	TestTrue(TEXT("T Func(FEventData) is a binding shape"),
		FNDCBinder::GetBoundFunctionEventDataStruct(FindTestFunction(TEXT("EventDataOnly")), Out));
	TestEqual(TEXT("T Func(FEventData) declares the struct it takes"),
		(const void*)Out, (const void*)FNDCBinderTestContext::StaticStruct());

	Out = Poison;
	TestTrue(TEXT("T Func(const FEventData&) is a binding shape"),
		FNDCBinder::GetBoundFunctionEventDataStruct(FindTestFunction(TEXT("ConstEventDataRef")), Out));
	TestEqual(TEXT("T Func(const FEventData&) declares the struct it takes"),
		(const void*)Out, (const void*)FNDCBinderTestContext::StaticStruct());

	// The same list the signature check refuses, asked the other way round. A rejected shape must
	// leave the answer alone rather than write a null over it: the callers pass a variable they
	// already filled from an earlier candidate.
	const TCHAR* const NotBindingShapes[] =
	{
		TEXT("EventDataThenExtra"),
		TEXT("NonStructParam"),
		TEXT("ActorParam"),
		TEXT("MutableEventDataRef"),
	};
	for (const TCHAR* FuncName : NotBindingShapes)
	{
		Out = Poison;
		TestFalse(FString::Printf(TEXT("%s is not a binding shape"), FuncName),
			FNDCBinder::GetBoundFunctionEventDataStruct(FindTestFunction(FuncName), Out));
		TestEqual(FString::Printf(TEXT("%s leaves the answer untouched"), FuncName),
			(const void*)Out, (const void*)Poison);
	}

	Out = Poison;
	TestFalse(TEXT("a missing function is not a binding shape"),
		FNDCBinder::GetBoundFunctionEventDataStruct(nullptr, Out));

	// The agreement the diagnostics rest on: whenever a struct is named, declaring it is exactly what
	// makes the signature check accept the function, and not declaring it is exactly what makes it
	// refuse. Naming a struct the check would still reject offers a fix that fixes nothing — which is
	// the failure mode this whole helper exists to avoid.
	const TCHAR* const FixableByDeclaring[] =
	{
		TEXT("EventDataOnly"),
		TEXT("ConstEventDataRef"),
		TEXT("EventDataOfWrongType"),
	};
	for (const TCHAR* FuncName : FixableByDeclaring)
	{
		const UFunction* Func = FindTestFunction(FuncName);
		UScriptStruct* Wanted = nullptr;
		if (!FNDCBinder::GetBoundFunctionEventDataStruct(Func, Wanted) || !Wanted)
		{
			AddError(FString::Printf(TEXT("%s should declare an event data struct"), FuncName));
			continue;
		}
		TestFalse(FString::Printf(TEXT("%s is refused while no Event Data Type is declared"), FuncName),
			MakeWriter(nullptr).IsValidValueFunction(Func, ENDCVariableType::Vector));
		TestTrue(FString::Printf(TEXT("%s is accepted once its own struct is declared"), FuncName),
			MakeWriter(Wanted).IsValidValueFunction(Func, ENDCVariableType::Vector));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderEnumIdentityTest,
	"NDCBinder.BoundFunctions.EnumIdentity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderEnumIdentityTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

	const UEnum* const Wanted = StaticEnum<ENDCBinderTestEnum>();
	const UEnum* const Other = StaticEnum<ENDCBinderTestOtherEnum>();
	const UEnum* const AsByte = StaticEnum<ENDCBinderTestByteEnum>();

	const UScriptStruct* const EnumContext = FNDCBinderTestEnumContext::StaticStruct();
	const FProperty* const Typed = EnumContext->FindPropertyByName(TEXT("Typed"));
	const FProperty* const OtherTyped = EnumContext->FindPropertyByName(TEXT("OtherTyped"));
	const FProperty* const ByteTyped = EnumContext->FindPropertyByName(TEXT("ByteTyped"));
	const FProperty* const Raw = EnumContext->FindPropertyByName(TEXT("Raw"));
	if (!Wanted || !Other || !AsByte || !Typed || !OtherTyped || !ByteTyped || !Raw)
	{
		AddError(TEXT("the enum test types no longer have the members this test is written against"));
		return false;
	}

	// Asking without an enum is the old question, and it still gets the old answer: any enum, and a
	// bare byte too. Rows on a channel whose variable carries no enum go through this.
	TestTrue(TEXT("with no enum wanted, an enum member fits"),
		FNDCBinder::IsPropertyCompatibleWithVariableType(Typed, ENDCVariableType::Enum));
	TestTrue(TEXT("with no enum wanted, so does an unrelated one"),
		FNDCBinder::IsPropertyCompatibleWithVariableType(OtherTyped, ENDCVariableType::Enum));
	TestTrue(TEXT("with no enum wanted, so does a bare byte"),
		FNDCBinder::IsPropertyCompatibleWithVariableType(Raw, ENDCVariableType::Enum));

	// Given the channel variable's enum, a member of a DIFFERENT enum is the one thing refused. Both
	// storage forms answer the same: enum class is an FEnumProperty, TEnumAsByte an FByteProperty.
	TestTrue(TEXT("a member of the wanted enum fits"),
		FNDCBinder::IsPropertyCompatibleWithVariableType(Typed, ENDCVariableType::Enum, Wanted));
	TestFalse(TEXT("a member of an unrelated enum does not"),
		FNDCBinder::IsPropertyCompatibleWithVariableType(OtherTyped, ENDCVariableType::Enum, Wanted));
	TestFalse(TEXT("nor does a byte-stored member of an unrelated enum"),
		FNDCBinder::IsPropertyCompatibleWithVariableType(ByteTyped, ENDCVariableType::Enum, Wanted));
	TestTrue(TEXT("a byte-stored member fits its own enum"),
		FNDCBinder::IsPropertyCompatibleWithVariableType(ByteTyped, ENDCVariableType::Enum, AsByte));
	TestFalse(TEXT("and an enum-class member does not fit the byte-stored enum"),
		FNDCBinder::IsPropertyCompatibleWithVariableType(Typed, ENDCVariableType::Enum, AsByte));

	// The deliberate hole: a member that names no enum is still taken. The channel stores an int, so
	// a raw byte is a value and not a type error — unlike on a context field, which is typed.
	TestTrue(TEXT("a bare byte still fits a known enum"),
		FNDCBinder::IsPropertyCompatibleWithVariableType(Raw, ENDCVariableType::Enum, Wanted));

	// A wanted enum is meaningless for every other channel type and must not leak into one.
	TestFalse(TEXT("an enum member is still not an int32 row"),
		FNDCBinder::IsPropertyCompatibleWithVariableType(Typed, ENDCVariableType::Int32, Wanted));

	// The same rule reached through a bound function's return value.
	const FNDCBinder Writer = MakeWriter(FNDCBinderTestContext::StaticStruct());
	TestTrue(TEXT("a getter returning the wanted enum is accepted"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("ReturnsTestEnum")), ENDCVariableType::Enum, Wanted));
	TestFalse(TEXT("a getter returning an unrelated enum is refused"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("ReturnsOtherEnum")), ENDCVariableType::Enum, Wanted));
	TestTrue(TEXT("a getter returning a raw byte is accepted"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("ReturnsRawByte")), ENDCVariableType::Enum, Wanted));
	TestTrue(TEXT("a getter returning a byte-stored enum is accepted for that enum"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("ReturnsByteEnum")), ENDCVariableType::Enum, AsByte));
	TestFalse(TEXT("and refused for another"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("ReturnsByteEnum")), ENDCVariableType::Enum, Wanted));
	TestTrue(TEXT("a non-enum row is unaffected by an enum being passed"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("ReturnsInt")), ENDCVariableType::Int32, Wanted));

	// The answer depends on the enum, so the enum is part of what the cache remembers. A channel edit
	// can retype a variable while the row keeps its name and its ENDCVariableType, and the remembered
	// "this field will do" would otherwise outlive the question it answered.
	FNDCFieldCache Cache;
	const FProperty* Cached = nullptr;
	Cache.Store(EnumContext, Typed, ENDCVariableType::Enum, Wanted);
	TestTrue(TEXT("the remembered field comes back for the enum it was stored under"),
		Cache.TryGet(EnumContext, Cached, ENDCVariableType::Enum, Wanted));
	TestFalse(TEXT("but not for a row that now wants a different enum"),
		Cache.TryGet(EnumContext, Cached, ENDCVariableType::Enum, Other));
	TestFalse(TEXT("nor for one that now wants none"),
		Cache.TryGet(EnumContext, Cached, ENDCVariableType::Enum));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderCachedMissTest,
	"NDCBinder.BoundFunctions.CachedMiss",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderCachedMissTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

	const UClass* const Host = UNDCBinderTestFunctionHost::StaticClass();
	FNDCBoundFunctionCache Cache;
	UFunction* Out = nullptr;

	// A remembered hit is served, in every configuration: that is what the cache is for.
	UFunction* const Real = FindTestFunction(TEXT("NoParams"));
	Cache.Store(Host, TEXT("NoParams"), nullptr, ENDCVariableType::Vector, nullptr, Real);
	TestTrue(TEXT("a remembered hit is served"),
		Cache.TryGet(Host, TEXT("NoParams"), nullptr, ENDCVariableType::Vector, nullptr, Out));
	TestEqual(TEXT("and it is the function that was stored"), (const void*)Out, (const void*)Real);

	// A remembered MISS is where the two builds differ. In the editor the function it did not find can
	// be added to a Blueprint and compiled in against the same UClass, so the miss is re-resolved
	// rather than served; a cooked build cannot grow a function and keeps it.
	Out = Real;
	Cache.Store(Host, TEXT("NoSuchFunctionAnywhere"), nullptr, ENDCVariableType::Vector, nullptr, nullptr);
	const bool bServedMiss = Cache.TryGet(Host, TEXT("NoSuchFunctionAnywhere"), nullptr, ENDCVariableType::Vector, nullptr, Out);
#if WITH_EDITOR
	TestFalse(TEXT("a remembered miss is re-resolved in the editor"), bServedMiss);
#else
	TestTrue(TEXT("a remembered miss is served outside the editor"), bServedMiss);
	TestNull(TEXT("and the served answer is the miss"), Out);
#endif

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderStaticArrayFieldTest,
	"NDCBinder.BoundFunctions.StaticArrayField",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderStaticArrayFieldTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

	// `FVector Corners[2]` is one FStructProperty of FVector, so every question about its TYPE says
	// yes. What it cannot answer is which element, and the two places that would go on to pick one
	// both pick zero: a path resolves to the member offset, a store reads from it. So the refusal has
	// to come before either, in both gates a row can arrive through — the bind menu asks the type
	// question straight about a member of the event data, with no path involved.
	UScriptStruct* const EventData = FNDCBinderTestContext::StaticStruct();
	const FProperty* const Corners = EventData->FindPropertyByName(TEXT("Corners"));
	const FProperty* const Location = EventData->FindPropertyByName(TEXT("Location"));
	if (!Corners || !Location)
	{
		AddError(TEXT("the test event data lost the members this is about"));
		return false;
	}
	TestEqual(TEXT("both members are the same type as far as reflection is concerned"),
		(const void*)CastField<FStructProperty>(Corners)->Struct,
		(const void*)CastField<FStructProperty>(Location)->Struct);
	TestTrue(TEXT("and only one of them is a static array"),
		Corners->ArrayDim > 1 && Location->ArrayDim == 1);

	TestFalse(TEXT("a static array is not offered as the source of a Vector row"),
		FNDCBinder::IsPropertyCompatibleWithVariableType(Corners, ENDCVariableType::Vector));
	TestTrue(TEXT("while the single member of the same type is"),
		FNDCBinder::IsPropertyCompatibleWithVariableType(Location, ENDCVariableType::Vector));

	TestFalse(TEXT("nor may it drive a context field of that type"),
		FNDCBinder::CanAssignPropertyToField(Corners, Location));
	TestTrue(TEXT("where the single member may"),
		FNDCBinder::CanAssignPropertyToField(Location, Location));

	const FProperty* Leaf = nullptr;
	int32 Offset = 0;
	TestFalse(TEXT("and a path naming it resolves to nothing rather than to its first element"),
		FNDCBinder::ResolveFieldPath(EventData, TEXT("Corners"), Leaf, Offset));

	// The enumeration the panel builds its event data list from leaves it out for the same reason.
	FNDCBinder Writer = MakeWriter(EventData);
	bool bOffered = false;
	Writer.ForEachEventDataField([&bOffered](FProperty& Field)
	{
		bOffered |= Field.GetFName() == TEXT("Corners");
	});
	TestFalse(TEXT("and it is not listed as an event data field"), bOffered);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderFieldPathTest,
	"NDCBinder.BoundFunctions.FieldPath",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderFieldPathTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

	const UScriptStruct* const Root = FNDCBinderTestNestedContext::StaticStruct();
	FNDCBinderTestNestedContext Value;
	const uint8* const Base = reinterpret_cast<const uint8*>(&Value);

	auto Resolve = [Root](const TCHAR* Path, const FProperty*& OutLeaf, int32& OutOffset)
	{
		return FNDCBinder::ResolveFieldPath(Root, FName(Path), OutLeaf, OutOffset);
	};

	const FProperty* Leaf = nullptr;
	int32 Offset = INDEX_NONE;

	// The regression guard for the whole change: a path of one segment has to land exactly where the
	// ContainerPtrToValuePtr it replaced landed, or every binding written before paths existed moves.
	TestTrue(TEXT("a one-segment path resolves"), Resolve(TEXT("Direct"), Leaf, Offset));
	const FProperty* const DirectProp = Root->FindPropertyByName(TEXT("Direct"));
	if (!DirectProp)
	{
		AddError(TEXT("the nested test struct no longer has the member this test is written against"));
		return false;
	}
	TestEqual(TEXT("and names the same property a plain lookup would"), (const void*)Leaf, (const void*)DirectProp);
	TestEqual(TEXT("and lands where ContainerPtrToValuePtr lands"),
		(const void*)(Base + Offset), (const void*)DirectProp->ContainerPtrToValuePtr<void>(&Value));

	// A nested path is the sum of the member offsets along it, which is why it costs one addition at
	// write time however deep it goes.
	TestTrue(TEXT("a two-segment path resolves"), Resolve(TEXT("Mid.Leaf"), Leaf, Offset));
	TestEqual(TEXT("and lands on the member it names"), (const void*)(Base + Offset), (const void*)&Value.Mid.Leaf);

	TestTrue(TEXT("a three-segment path resolves"), Resolve(TEXT("Mid.Leaf.Point"), Leaf, Offset));
	TestEqual(TEXT("and lands on the leaf it names"), (const void*)(Base + Offset), (const void*)&Value.Mid.Leaf.Point);
	TestTrue(TEXT("and the leaf is the property, not the struct it came through"),
		Leaf != nullptr && Leaf->GetFName() == TEXT("Point"));

	// The engine's own struct members are properties too, so a path may end inside one.
	TestTrue(TEXT("a four-segment path resolves"), Resolve(TEXT("Mid.Leaf.Point.X"), Leaf, Offset));
	TestEqual(TEXT("and lands on the scalar it names"), (const void*)(Base + Offset), (const void*)&Value.Mid.Leaf.Point.X);

	// MaxFieldPathDepth, asked the only way that means anything: the path that is one segment too long.
	TestFalse(TEXT("a path past the depth limit is refused"), Resolve(TEXT("Outer.Mid.Leaf.Point.X"), Leaf, Offset));
	TestTrue(TEXT("while the same path one segment shorter resolves"), Resolve(TEXT("Outer.Mid.Leaf.Point"), Leaf, Offset));

	// What a path may not walk into, and why each one is not an oversight: an array has no index in the
	// path, an object would be a load and a null check rather than a fixed offset, and an editor-only
	// member exists here and would not exist in a cooked build.
	TestFalse(TEXT("a path does not enter an array"), Resolve(TEXT("Many.Point"), Leaf, Offset));
	TestFalse(TEXT("a path does not enter an object"), Resolve(TEXT("Object.Name"), Leaf, Offset));
#if WITH_EDITORONLY_DATA
	TestFalse(TEXT("a path does not enter an editor-only member"), Resolve(TEXT("EditorOnlyMid.Leaf"), Leaf, Offset));
	TestFalse(TEXT("nor name one as its leaf"), Resolve(TEXT("EditorOnlyMid"), Leaf, Offset));
#endif

	// Broken paths, and the failure that has to leave the answer alone.
	Leaf = DirectProp;
	Offset = 123;
	TestFalse(TEXT("a segment that names nothing is refused"), Resolve(TEXT("Mid.NoSuchMember"), Leaf, Offset));
	TestNull(TEXT("and the refusal clears the leaf"), Leaf);
	TestEqual(TEXT("and the offset with it"), Offset, 0);

	TestFalse(TEXT("an empty segment is refused"), Resolve(TEXT("Mid..Leaf"), Leaf, Offset));
	TestFalse(TEXT("a trailing dot is refused"), Resolve(TEXT("Mid."), Leaf, Offset));
	TestFalse(TEXT("an empty path is refused"), Resolve(TEXT(""), Leaf, Offset));
	TestFalse(TEXT("a null root is refused"),
		FNDCBinder::ResolveFieldPath(nullptr, TEXT("Direct"), Leaf, Offset));

	// And the writer's own accessor answers the same, since it is this walk with the root filled in.
	const FNDCBinder Writer = MakeWriter(const_cast<UScriptStruct*>(Root));
	TestTrue(TEXT("the writer resolves a nested path through its event data type"),
		Writer.FindEventDataField(TEXT("Mid.Leaf.Point")) != nullptr);
	TestNull(TEXT("and refuses the same broken one"), Writer.FindEventDataField(TEXT("Mid.NoSuchMember")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderBindingFlagsTest,
	"NDCBinder.BoundFunctions.Flags",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderBindingFlagsTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

	const FNDCBinder Writer = MakeWriter(FNDCBinderTestContext::StaticStruct());

	//~ Cast to int32: TestEqual has no overload for a scoped enum.
	TestEqual(TEXT("a const function of the right shape passes the flag gate"),
		static_cast<int32>(FNDCBinder::GetBindingRejection(FindTestFunction(TEXT("EventDataOnly")))),
		static_cast<int32>(ENDCBindingRejection::None));

	TestEqual(TEXT("a null function is reported as missing"),
		static_cast<int32>(FNDCBinder::GetBindingRejection(nullptr)),
		static_cast<int32>(ENDCBindingRejection::Missing));

	// The rule with teeth: shape and return type are both correct here, so only the flag gate can
	// reject it — and it has to, because a cue notify calls its bindings on a shared CDO.
	TestEqual(TEXT("a non-const, non-pure function is rejected as impure"),
		static_cast<int32>(FNDCBinder::GetBindingRejection(FindTestFunction(TEXT("NotConst")))),
		static_cast<int32>(ENDCBindingRejection::NotPure));
	TestFalse(TEXT("and the value-binding check rejects it too, not just the gate"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("NotConst")), ENDCVariableType::Vector));

#if WITH_EDITOR
	TestEqual(TEXT("an editor-only function is rejected"),
		static_cast<int32>(FNDCBinder::GetBindingRejection(FindTestFunction(TEXT("EditorOnlyFunction")))),
		static_cast<int32>(ENDCBindingRejection::EditorOnly));
	TestFalse(TEXT("an editor-only function is not a valid value binding"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("EditorOnlyFunction")), ENDCVariableType::Vector));
#endif

	// The gate guards every binding kind by the same rule, not just value rows. That uniformity is
	// what the removal of the whole-context binding bought: there is no longer a binding that is
	// allowed to mutate, so there is no second rule to keep in step with this one.
	TestFalse(TEXT("an impure function is not a valid context field binding either"),
		Writer.IsValidContextFieldFunction(FindTestFunction(TEXT("NotConst")),
			FNDCAccessContext::StaticStruct()->FindPropertyByName(TEXT("Location"))));

	// Every rejection an author can hit has to say something, or the editor tooltip renders blank.
	for (const ENDCBindingRejection Rejection : {
			ENDCBindingRejection::Missing, ENDCBindingRejection::Networked, ENDCBindingRejection::EditorOnly,
			ENDCBindingRejection::DelegateSignature, ENDCBindingRejection::NotPure })
	{
		TestTrue(TEXT("every rejection has an explanation"),
			FCString::Strlen(FNDCBinder::DescribeRejection(Rejection)) > 0);
	}
	TestEqual(TEXT("None explains nothing"),
		static_cast<int32>(FCString::Strlen(FNDCBinder::DescribeRejection(ENDCBindingRejection::None))), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderContextFieldTest,
	"NDCBinder.AccessContext.FieldBindings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderContextFieldTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

	const FNDCBinder Writer = MakeWriter(FNDCBinderTestContext::StaticStruct());
	const UScriptStruct* ContextType = FNDCAccessContext::StaticStruct();

	const FProperty* OwningComponent = ContextType->FindPropertyByName(TEXT("OwningComponent"));
	const FProperty* Location        = ContextType->FindPropertyByName(TEXT("Location"));
	const FProperty* OverrideLoc     = ContextType->FindPropertyByName(TEXT("bOverrideLocation"));
	const FProperty* SystemToSpawn   = ContextType->FindPropertyByName(TEXT("SystemToSpawn"));
	const FProperty* SpawnedSystems  = ContextType->FindPropertyByName(TEXT("SpawnedSystems"));

	if (!OwningComponent || !Location || !OverrideLoc || !SystemToSpawn || !SpawnedSystems)
	{
		AddError(TEXT("FNDCAccessContext no longer has the fields these bindings are written against"));
		return false;
	}

#if WITH_EDITORONLY_DATA
	// Only Niagara's declared inputs are offered. An output is the channel's answer back to the
	// caller, and a row driving one would be talking over it.
	TSet<FName> Offered;
	FNDCBinder::ForEachContextInputField(ContextType, [&Offered](FProperty& Field)
	{
		Offered.Add(Field.GetFName());
	});
	TestTrue(TEXT("the owning component is offered"), Offered.Contains(OwningComponent->GetFName()));
	TestTrue(TEXT("the location is offered"), Offered.Contains(Location->GetFName()));
	TestTrue(TEXT("the location override flag is offered"), Offered.Contains(OverrideLoc->GetFName()));
	TestFalse(TEXT("an output field is not offered"), Offered.Contains(SpawnedSystems->GetFName()));
	TestFalse(TEXT("a non-context struct offers nothing"), ([]
	{
		bool bVisited = false;
		FNDCBinder::ForEachContextInputField(FNDCBinderTestContext::StaticStruct(),
			[&bVisited](FProperty&) { bVisited = true; });
		return bVisited;
	}()));
#endif

	// A context row is an ordinary read binding: an ordinary getter of the field's own type.
	TestTrue(TEXT("an object getter drives an object field"),
		Writer.IsValidContextFieldFunction(FindTestFunction(TEXT("ReturnsComponent")), OwningComponent));
	TestTrue(TEXT("a vector getter drives a vector field"),
		Writer.IsValidContextFieldFunction(FindTestFunction(TEXT("ReturnsVector")), Location));
	// The flags on a context are bitfields (uint32 b... : 1), not native bools.
	TestTrue(TEXT("a bool getter drives a bitfield flag"),
		Writer.IsValidContextFieldFunction(FindTestFunction(TEXT("ReturnsBool")), OverrideLoc));
	// SystemToSpawn is declared as a bare UObject*, so the type half accepts anything derived from
	// UObject. The narrowing to Niagara systems lives in AllowedClasses metadata and is asked
	// separately, below — the write stores by type in every configuration, and metadata does not
	// survive a cook.
	TestTrue(TEXT("a system getter drives the system field"),
		Writer.IsValidContextFieldFunction(FindTestFunction(TEXT("RecordCall")), SystemToSpawn));

#if WITH_EDITORONLY_DATA
	// The other half of that question, and the one a bind menu asks. A field whose declared type is
	// wider than the types it wants would otherwise offer every object getter on the class.
	auto ReturnPropOf = [](const TCHAR* Name) -> const FProperty*
	{
		const UFunction* Func = FindTestFunction(Name);
		return Func ? Func->GetReturnProperty() : nullptr;
	};
	TestTrue(TEXT("a system getter satisfies the field's allowed classes"),
		FNDCBinder::IsSourceAllowedByFieldClasses(ReturnPropOf(TEXT("RecordCall")), SystemToSpawn));
	TestFalse(TEXT("a component getter does not satisfy them"),
		FNDCBinder::IsSourceAllowedByFieldClasses(ReturnPropOf(TEXT("ReturnsComponent")), SystemToSpawn));
	// The case this exists for: assignable by type, and still not what the field asked for.
	TestFalse(TEXT("a getter promising only UObject does not satisfy them"),
		FNDCBinder::IsSourceAllowedByFieldClasses(ReturnPropOf(TEXT("ReturnsBareObject")), SystemToSpawn));
	// A field that names no classes is unrestricted, not restricted to nothing.
	TestTrue(TEXT("a field with no allowed classes takes any assignable object"),
		FNDCBinder::IsSourceAllowedByFieldClasses(ReturnPropOf(TEXT("ReturnsComponent")), OwningComponent));
	TestTrue(TEXT("a non-object field is not restricted by classes"),
		FNDCBinder::IsSourceAllowedByFieldClasses(ReturnPropOf(TEXT("ReturnsVector")), Location));
#endif

	// Nothing coerces. A wrong type is a wrong binding, not a value quietly converted on the way in.
	TestFalse(TEXT("a colour does not drive a vector field"),
		Writer.IsValidContextFieldFunction(FindTestFunction(TEXT("ReturnsColor")), Location));
	TestFalse(TEXT("a bool does not drive a vector field"),
		Writer.IsValidContextFieldFunction(FindTestFunction(TEXT("ReturnsBool")), Location));
	TestFalse(TEXT("a vector does not drive an object field"),
		Writer.IsValidContextFieldFunction(FindTestFunction(TEXT("ReturnsVector")), OwningComponent));
	TestFalse(TEXT("an int does not drive a bitfield flag"),
		Writer.IsValidContextFieldFunction(FindTestFunction(TEXT("ReturnsInt")), OverrideLoc));
	TestFalse(TEXT("a missing function drives nothing"),
		Writer.IsValidContextFieldFunction(nullptr, Location));
	TestFalse(TEXT("no field means no binding"),
		Writer.IsValidContextFieldFunction(FindTestFunction(TEXT("ReturnsVector")), nullptr));

	// The signature rule is the same one every other binding follows.
	TestFalse(TEXT("an event data parameter of the wrong type is rejected here too"),
		Writer.IsValidContextFieldFunction(FindTestFunction(TEXT("EventDataOfWrongType")), Location));

	// Required only means something where a value can mean "there wasn't one".
	TestTrue(TEXT("an object field can be required"),
		FNDCBinder::CanContextFieldBeRequired(OwningComponent));
	TestFalse(TEXT("a vector field cannot be required"),
		FNDCBinder::CanContextFieldBeRequired(Location));
	TestFalse(TEXT("a flag cannot be required"),
		FNDCBinder::CanContextFieldBeRequired(OverrideLoc));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderContextTypeTest,
	"NDCBinder.BoundFunctions.GetEventDataType()",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderContextTypeTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

	UFunction* EventDataFunc = FindTestFunction(TEXT("EventDataOnly"));

	// The event data parameter is only legal because the owner declared that exact struct.
	const FNDCBinder Matching = MakeWriter(FNDCBinderTestContext::StaticStruct());
	TestTrue(TEXT("the declared event data type is accepted"),
		Matching.IsValidValueFunction(EventDataFunc, ENDCVariableType::Vector));

	const FNDCBinder Mismatched = MakeWriter(FNDCBinderTestOtherContext::StaticStruct());
	TestFalse(TEXT("a different declared event data type rejects the function"),
		Mismatched.IsValidValueFunction(EventDataFunc, ENDCVariableType::Vector));

	const FNDCBinder NoEventData = MakeWriter(nullptr);
	TestFalse(TEXT("no declared event data type means no parameter at all"),
		NoEventData.IsValidValueFunction(EventDataFunc, ENDCVariableType::Vector));
	TestTrue(TEXT("no declared event data type still accepts the no-parameter shape"),
		NoEventData.IsValidValueFunction(FindTestFunction(TEXT("NoParams")), ENDCVariableType::Vector));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderReturnTypeTest,
	"NDCBinder.BoundFunctions.ReturnType",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderReturnTypeTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

	const FNDCBinder Writer = MakeWriter(FNDCBinderTestContext::StaticStruct());

	TestTrue(TEXT("bool matches Bool"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("ReturnsBool")), ENDCVariableType::Bool));
	TestTrue(TEXT("int32 matches Int32"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("ReturnsInt")), ENDCVariableType::Int32));
	TestTrue(TEXT("double matches Float"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("ReturnsDouble")), ENDCVariableType::Float));
	TestTrue(TEXT("FLinearColor matches LinearColor"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("ReturnsColor")), ENDCVariableType::LinearColor));
	TestTrue(TEXT("FQuat matches Quat"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("ReturnsQuat")), ENDCVariableType::Quat));
	TestTrue(TEXT("FVector matches Position as well as Vector"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("NoParams")), ENDCVariableType::Position));

	// A return type that does not match the channel variable is not a candidate.
	TestFalse(TEXT("bool does not match Vector"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("ReturnsBool")), ENDCVariableType::Vector));
	TestFalse(TEXT("FVector does not match Bool"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("NoParams")), ENDCVariableType::Bool));
	TestFalse(TEXT("FQuat does not match LinearColor"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("ReturnsQuat")), ENDCVariableType::LinearColor));
	TestFalse(TEXT("an Unsupported binding never matches"),
		Writer.IsValidValueFunction(FindTestFunction(TEXT("NoParams")), ENDCVariableType::Unsupported));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderCallMarshallingTest,
	"NDCBinder.BoundFunctions.CallMarshalling",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderCallMarshallingTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

	UNDCBinderTestFunctionHost* Host = NewObject<UNDCBinderTestFunctionHost>();

	FNDCBinderTestContext EventData;
	EventData.Location = FVector(10.0, 20.0, 30.0);
	EventData.Tags = { TEXT("Alpha"), TEXT("Beta") };

	// Applying one context row is the shortest public path that actually calls a bound function.
	// RecordCall answers null, which a non-required row treats as no opinion — so the row runs the
	// call and writes nothing, which is exactly the isolation this test wants.
	const FNDCBinder Writer = MakeWriter(FNDCBinderTestContext::StaticStruct());
	FNDCAccessContextInst Context = MakeContext();
	FNDCContextBinding Row = FNDCContextBinding::Make(TEXT("SystemToSpawn"), TEXT("RecordCall"));

	UNDCBinderTestFunctionHost::ResetCallRecord();
	Writer.ApplyContextBinding(Row, Context, Host, FConstStructView::Make(EventData));

	TestTrue(TEXT("the bound function was called"), UNDCBinderTestFunctionHost::bWasCalled);
	TestEqual(TEXT("the event data value arrives intact"),
		UNDCBinderTestFunctionHost::LastContext.Location, EventData.Location);
	// The heap-owning member is the one that would break if the parms block were mishandled.
	TestEqual(TEXT("the event data's array arrives intact"),
		UNDCBinderTestFunctionHost::LastContext.Tags, EventData.Tags);

	// An empty view means the function still runs, with a default-constructed parameter.
	UNDCBinderTestFunctionHost::ResetCallRecord();
	Writer.ApplyContextBinding(Row, Context, Host, FConstStructView());
	TestTrue(TEXT("no event data still calls the function"), UNDCBinderTestFunctionHost::bWasCalled);
	TestEqual(TEXT("no event data leaves the parameter default-constructed"),
		UNDCBinderTestFunctionHost::LastContext.Tags.Num(), 0);

	// A const reference parameter is passed as a bitwise alias of the caller's struct rather than a
	// deep copy, so it exercises the other half of the marshalling — and it is what real bound
	// functions declare. Getting the ownership wrong here would corrupt the caller's struct.
	FNDCContextBinding RefRow = FNDCContextBinding::Make(TEXT("SystemToSpawn"), TEXT("RecordCallByRef"));

	UNDCBinderTestFunctionHost::ResetCallRecord();
	Writer.ApplyContextBinding(RefRow, Context, Host, FConstStructView::Make(EventData));

	TestTrue(TEXT("a by-reference bound function is called"), UNDCBinderTestFunctionHost::bWasCalled);
	TestEqual(TEXT("by reference, the event data value arrives intact"),
		UNDCBinderTestFunctionHost::LastContext.Location, EventData.Location);
	TestEqual(TEXT("by reference, the event data's array arrives intact"),
		UNDCBinderTestFunctionHost::LastContext.Tags, EventData.Tags);

	// Called repeatedly, because a stale alias or a wrongly destroyed one only shows up the second time.
	for (int32 Pass = 0; Pass < 3; ++Pass)
	{
		UNDCBinderTestFunctionHost::ResetCallRecord();
		Writer.ApplyContextBinding(RefRow, Context, Host, FConstStructView::Make(EventData));
	}
	TestEqual(TEXT("the caller's struct survives repeated by-reference calls"), EventData.Tags.Num(), 2);
	TestEqual(TEXT("the caller's struct still holds its values"), EventData.Tags[0], FName(TEXT("Alpha")));
	TestEqual(TEXT("by reference, the last call still saw the data"),
		UNDCBinderTestFunctionHost::LastContext.Tags, EventData.Tags);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderContextApplyTest,
	"NDCBinder.AccessContext.ApplyRow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderContextApplyTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

	// The broken-row cases below each log once; they are the point of those assertions, not noise.
	AddExpectedMessagePlain(TEXT("the access context has no such field"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	AddExpectedMessagePlain(TEXT("does not match the binding"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);

	UNDCBinderTestFunctionHost* Host = NewObject<UNDCBinderTestFunctionHost>();
	const FNDCBinder Writer = MakeWriter(FNDCBinderTestContext::StaticStruct());

	FNDCBinderTestContext EventData;
	EventData.Location = FVector(4.0, 5.0, 6.0);

	// A plain value row overwrites its field and nothing else.
	{
		FNDCAccessContextInst Context = MakeContext();
		FNDCContextBinding Row = FNDCContextBinding::Make(TEXT("Location"), TEXT("ReturnsVector"));
		TestTrue(TEXT("a value row does not decline the write"),
			Writer.ApplyContextBinding(Row, Context, Host, FConstStructView::Make(EventData)));
		TestEqual(TEXT("the answer lands in the field"),
			Context.GetChecked<FNDCAccessContext>().Location, EventData.Location);
		TestFalse(TEXT("and no flag is touched without one named"),
			(bool)Context.GetChecked<FNDCAccessContext>().bOverrideLocation);
	}

	// A Transient gate cannot be authored — it does not serialize, so the panel has no checkbox for
	// it — which leaves the write as the only thing that can set it. bOverrideLocation is one, and
	// Location is dead without it: GetLocation() returns the owning component's location instead.
	{
		FNDCAccessContextInst Context = MakeContext();
		FNDCContextBinding Row = FNDCContextBinding::Make(
			TEXT("Location"), TEXT("ReturnsVector"), /*bRequired=*/ false, TEXT("bOverrideLocation"));
		Writer.ApplyContextBinding(Row, Context, Host, FConstStructView::Make(EventData));
		TestTrue(TEXT("writing a field switches on a gate that could not have been authored"),
			(bool)Context.GetChecked<FNDCAccessContext>().bOverrideLocation);
	}

	// An authorable gate is the opposite: it is a checkbox on the field's own row, so it belongs to
	// whoever ticked it. A write that set it behind their back would make that checkbox a lie, so a
	// bound field whose gate is clear is written and then ignored by the channel — and the panel
	// marks that rather than quietly repairing it.
	{
		FNDCAccessContextInst Context = MakeContext();
		FNDCAccessContext& Ctx = Context.GetChecked<FNDCAccessContext>();

		FNDCContextBinding Row = FNDCContextBinding::Make(
			TEXT("SystemToSpawn"), TEXT("ReturnsComponent"), /*bRequired=*/ false, TEXT("bOverrideSystemToSpawn"));
		UNDCBinderTestFunctionHost::ComponentToReturn.Reset(NewObject<USceneComponent>());

		Writer.ApplyContextBinding(Row, Context, Host, FConstStructView::Make(EventData));
		TestEqual(TEXT("the bound value is still written"),
			Ctx.SystemToSpawn.Get(), (UObject*)UNDCBinderTestFunctionHost::ComponentToReturn.Get());
		TestFalse(TEXT("but an authorable gate is left exactly as authored"), (bool)Ctx.bOverrideSystemToSpawn);

		UNDCBinderTestFunctionHost::ComponentToReturn.Reset();
	}

	// A null object answer, on a row that is not required, is "no opinion": the authored value and
	// its flag both survive. This is what lets a per-surface system getter fall back to the panel.
	{
		FNDCAccessContextInst Context = MakeContext();
		FNDCAccessContext& Ctx = Context.GetChecked<FNDCAccessContext>();
		Ctx.SystemToSpawn = Host; // stand-in for an authored default

		FNDCContextBinding Row = FNDCContextBinding::Make(
			TEXT("SystemToSpawn"), TEXT("RecordCall"), /*bRequired=*/ false, TEXT("bOverrideSystemToSpawn"));
		TestTrue(TEXT("a null answer does not decline the write when the row is optional"),
			Writer.ApplyContextBinding(Row, Context, Host, FConstStructView::Make(EventData)));
		TestEqual(TEXT("a null answer leaves the authored value alone"),
			Ctx.SystemToSpawn.Get(), (UObject*)Host);
		TestFalse(TEXT("and leaves its flag alone too"), (bool)Ctx.bOverrideSystemToSpawn);
	}

	// The same answer on a required row declines the write instead. Three of the four cues that used
	// the removed whole-context binding used it for exactly this.
	{
		FNDCAccessContextInst Context = MakeContext();
		FNDCContextBinding Row = FNDCContextBinding::Make(TEXT("SystemToSpawn"), TEXT("RecordCall"), /*bRequired=*/ true);
		TestFalse(TEXT("a null answer declines the write when the row is required"),
			Writer.ApplyContextBinding(Row, Context, Host, FConstStructView::Make(EventData)));
	}

	// A row that cannot run must not be able to turn a write off: a typo in a function name would
	// otherwise stop an effect from ever playing and read as a content bug.
	{
		FNDCAccessContextInst Context = MakeContext();
		FNDCContextBinding GoneField = FNDCContextBinding::Make(TEXT("NoSuchField"), TEXT("ReturnsVector"), /*bRequired=*/ true);
		TestTrue(TEXT("a row naming a field this context does not have is skipped, not fatal"),
			Writer.ApplyContextBinding(GoneField, Context, Host, FConstStructView::Make(EventData)));

		FNDCContextBinding WrongType = FNDCContextBinding::Make(TEXT("Location"), TEXT("ReturnsColor"), /*bRequired=*/ true);
		TestTrue(TEXT("a row bound to an unusable function is skipped, not fatal"),
			Writer.ApplyContextBinding(WrongType, Context, Host, FConstStructView::Make(EventData)));
		TestTrue(TEXT("and it leaves the authored value alone"),
			Context.GetChecked<FNDCAccessContext>().Location.IsZero());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderFunctionCacheTest,
	"NDCBinder.BoundFunctions.PerClassCache",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderFunctionCacheTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

	// One writer driven against two unrelated classes that both implement the bound name. The
	// resolved function is remembered per class, so this is what would break if the entry leaked.
	UNDCBinderTestFunctionHost* Host = NewObject<UNDCBinderTestFunctionHost>();
	UNDCBinderTestOtherHost* OtherHost = NewObject<UNDCBinderTestOtherHost>();

	const FNDCBinder Writer = MakeWriter(FNDCBinderTestContext::StaticStruct());
	FNDCAccessContextInst Context = MakeContext();

	// One row, reused across calls: the cache lives on it, so a fresh row per call would test nothing.
	FNDCContextBinding Row = FNDCContextBinding::Make(TEXT("SystemToSpawn"), TEXT("RecordCall"));

	const FNDCBinderTestContext EventData{};

	auto CallOn = [&Writer, &Row, &Context, &EventData](UObject* Owner)
	{
		UNDCBinderTestFunctionHost::ResetCallRecord();
		UNDCBinderTestOtherHost::ResetCallRecord();
		Writer.ApplyContextBinding(Row, Context, Owner, FConstStructView::Make(EventData));
	};

	CallOn(Host);
	TestTrue(TEXT("the first class resolves"), UNDCBinderTestFunctionHost::bWasCalled);

	// Same writer, different class: the remembered entry must not answer for it.
	CallOn(OtherHost);
	TestTrue(TEXT("a second class resolves its own function"), UNDCBinderTestOtherHost::bWasCalled);
	TestFalse(TEXT("the first class's function is not called for the second"), UNDCBinderTestFunctionHost::bWasCalled);

	// ...and going back re-resolves rather than staying stuck on the last class seen.
	CallOn(Host);
	TestTrue(TEXT("the first class resolves again"), UNDCBinderTestFunctionHost::bWasCalled);
	TestFalse(TEXT("the second class's function is not called for the first"), UNDCBinderTestOtherHost::bWasCalled);

	// Repeated calls on one class go through the cache and must still call.
	CallOn(Host);
	TestTrue(TEXT("a cached entry still calls the function"), UNDCBinderTestFunctionHost::bWasCalled);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderCacheInvalidationTest,
	"NDCBinder.BoundFunctions.ConfigChangeInvalidatesCache",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderCacheInvalidationTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

	// Each rejection below logs once, by design — that is what "the binding stopped resolving" looks
	// like from the outside.
	AddExpectedMessagePlain(TEXT("does not match the binding"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);

	UNDCBinderTestFunctionHost* Host = NewObject<UNDCBinderTestFunctionHost>();
	const FNDCBinderTestContext EventData{};

	FNDCBinder Writer = MakeWriter(FNDCBinderTestContext::StaticStruct());
	FNDCAccessContextInst Context = MakeContext();
	FNDCContextBinding Row = FNDCContextBinding::Make(TEXT("SystemToSpawn"), TEXT("RecordCall"));

	UNDCBinderTestFunctionHost::ResetCallRecord();
	Writer.ApplyContextBinding(Row, Context, Host, FConstStructView::Make(EventData));
	TestTrue(TEXT("the binding resolves while the event data type matches"), UNDCBinderTestFunctionHost::bWasCalled);

	// RecordCall's parameter is no longer the declared type, so it must stop being called.
	Writer.SetEventDataTypeUnchecked(FNDCBinderTestOtherContext::StaticStruct());

	UNDCBinderTestFunctionHost::ResetCallRecord();
	Writer.ApplyContextBinding(Row, Context, Host, FConstStructView::Make(EventData));
	TestFalse(TEXT("changing the event data type invalidates the resolved function"),
		UNDCBinderTestFunctionHost::bWasCalled);

	// ...and putting it back makes the binding live again rather than staying rejected.
	Writer.SetEventDataTypeUnchecked(FNDCBinderTestContext::StaticStruct());

	UNDCBinderTestFunctionHost::ResetCallRecord();
	Writer.ApplyContextBinding(Row, Context, Host, FConstStructView::Make(EventData));
	TestTrue(TEXT("restoring the event data type revives the binding"), UNDCBinderTestFunctionHost::bWasCalled);

	// The bound name is part of the key too: rebinding to something else must not reuse the answer.
	Row.BoundFunction = TEXT("NoParams"); // returns FVector, which no object field accepts

	UNDCBinderTestFunctionHost::ResetCallRecord();
	Writer.ApplyContextBinding(Row, Context, Host, FConstStructView::Make(EventData));
	TestFalse(TEXT("rebinding to another name invalidates the resolved function"),
		UNDCBinderTestFunctionHost::bWasCalled);

	// And so is the field's own context type: the same name can mean a different property on another
	// context, and the answer was validated against the field that name used to mean.
	Row.BoundFunction = TEXT("RecordCall");
	UNDCBinderTestFunctionHost::ResetCallRecord();
	Writer.ApplyContextBinding(Row, Context, Host, FConstStructView::Make(EventData));
	TestTrue(TEXT("the binding resolves again on the context it was validated against"),
		UNDCBinderTestFunctionHost::bWasCalled);

	FNDCAccessContextInst LegacyContext;
	LegacyContext.AccessContext.InitializeAs(FNDCAccessContextLegacy::StaticStruct());
	UNDCBinderTestFunctionHost::ResetCallRecord();
	Writer.ApplyContextBinding(Row, LegacyContext, Host, FConstStructView::Make(EventData));
	TestFalse(TEXT("a context type without that field does not reuse the remembered answer"),
		UNDCBinderTestFunctionHost::bWasCalled);

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderEventDataBindingTest,
	"NDCBinder.BoundFunctions.EventDataFields",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderEventDataBindingTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

	// A row bound to a field this struct does not have logs once; that is the assertion, not noise.
	AddExpectedMessagePlain(TEXT("the event data has no such field"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	AddExpectedMessagePlain(TEXT("does not fit this context field"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);

	const FNDCBinder Writer = MakeWriter(FNDCBinderTestContext::StaticStruct());

	// The struct's own members are what a row may read — nothing about the owner is involved, which
	// is the whole point: the event data is the part that differs from one write to the next.
	TSet<FName> Offered;
	Writer.ForEachEventDataField([&Offered](FProperty& Field) { Offered.Add(Field.GetFName()); });
	TestTrue(TEXT("a struct member is offered"), Offered.Contains(TEXT("Location")));
	TestTrue(TEXT("so is one that owns heap data"), Offered.Contains(TEXT("Tags")));

	TestEqual(TEXT("a writer with no event data type offers nothing"), ([]
	{
		int32 Count = 0;
		MakeWriter(nullptr).ForEachEventDataField([&Count](FProperty&) { ++Count; });
		return Count;
	}()), 0);

	const FProperty* LocationField = Writer.FindEventDataField(TEXT("Location"));
	const FProperty* TagsField = Writer.FindEventDataField(TEXT("Tags"));
	if (!LocationField || !TagsField)
	{
		AddError(TEXT("the test event data struct no longer has the fields these bindings are written against"));
		return false;
	}
	TestNull(TEXT("a field that is not there resolves to nothing"),
		Writer.FindEventDataField(TEXT("NoSuchField")));

	// Type matching is the same rule a function's return value goes through, so it answers the same.
	TestTrue(TEXT("a vector member fits a vector channel variable"),
		FNDCBinder::IsPropertyCompatibleWithVariableType(LocationField, ENDCVariableType::Vector));
	TestTrue(TEXT("and a position one, which is a vector"),
		FNDCBinder::IsPropertyCompatibleWithVariableType(LocationField, ENDCVariableType::Position));
	TestFalse(TEXT("but not a bool one"),
		FNDCBinder::IsPropertyCompatibleWithVariableType(LocationField, ENDCVariableType::Bool));
	TestFalse(TEXT("and an array member fits nothing the channel can write"),
		FNDCBinder::IsPropertyCompatibleWithVariableType(TagsField, ENDCVariableType::Vector));

	// The same field against a context field, through the check the write itself uses.
	const FProperty* ContextLocation = FNDCAccessContext::StaticStruct()->FindPropertyByName(TEXT("Location"));
	const FProperty* ContextOwner = FNDCAccessContext::StaticStruct()->FindPropertyByName(TEXT("OwningComponent"));
	TestTrue(TEXT("a vector member drives a vector context field"),
		FNDCBinder::CanAssignPropertyToField(LocationField, ContextLocation));
	TestFalse(TEXT("but not an object one"),
		FNDCBinder::CanAssignPropertyToField(LocationField, ContextOwner));

	// And the value actually arrives, with no owner involved at all — an event data row needs none.
	FNDCBinderTestContext EventData;
	EventData.Location = FVector(11.0, 22.0, 33.0);

	FNDCAccessContextInst Context = MakeContext();
	FNDCContextBinding Row = FNDCContextBinding::MakeFromEventData(
		TEXT("Location"), TEXT("Location"), /*bRequired=*/ false, TEXT("bOverrideLocation"));
	TestTrue(TEXT("an event data row does not decline the write"),
		Writer.ApplyContextBinding(Row, Context, /*FunctionOwner=*/ nullptr, FConstStructView::Make(EventData)));
	TestEqual(TEXT("the member's value lands in the context field"),
		Context.GetChecked<FNDCAccessContext>().Location, EventData.Location);
	TestTrue(TEXT("and its transient gate is set, as for any other row"),
		(bool)Context.GetChecked<FNDCAccessContext>().bOverrideLocation);

	// A row naming a field that is gone is skipped, never fatal — the same rule every broken binding
	// follows, so a rename cannot silently stop an effect from playing.
	{
		FNDCAccessContextInst GoneContext = MakeContext();
		FNDCContextBinding GoneRow = FNDCContextBinding::MakeFromEventData(
			TEXT("Location"), TEXT("NoSuchField"), /*bRequired=*/ true);
		TestTrue(TEXT("a row bound to a departed field is skipped, not fatal"),
			Writer.ApplyContextBinding(GoneRow, GoneContext, nullptr, FConstStructView::Make(EventData)));
		TestTrue(TEXT("and it leaves the authored value alone"),
			GoneContext.GetChecked<FNDCAccessContext>().Location.IsZero());
	}

	// A field that exists but does not fit. This used to be caught by a check that ran before the
	// store; the store now answers it on its own, so this is the case that proves the refusal — and
	// the log line that goes with it — did not disappear along with that check.
	{
		FNDCAccessContextInst WrongContext = MakeContext();
		FNDCAccessContext& WrongCtx = WrongContext.GetChecked<FNDCAccessContext>();
		WrongCtx.OwningComponent = nullptr;

		FNDCContextBinding WrongType = FNDCContextBinding::MakeFromEventData(
			TEXT("OwningComponent"), TEXT("Location"), /*bRequired=*/ true);
		TestTrue(TEXT("a vector cannot drive an object field, and the row is skipped rather than fatal"),
			Writer.ApplyContextBinding(WrongType, WrongContext, nullptr, FConstStructView::Make(EventData)));
		TestNull(TEXT("and nothing was stored into the field"), WrongCtx.OwningComponent.Get());
	}

	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderNiagaraTypeMappingTest,
	"NDCBinder.ChannelVariables.TypeMapping",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderNiagaraTypeMappingTest::RunTest(const FString& Parameters)
{
	auto Mapped = [](const FNiagaraTypeDefinition& TypeDef)
	{
		return FNDCVariableBinding::VariableTypeFromNiagaraType(TypeDef);
	};

	TestEqual(TEXT("bool"), Mapped(FNiagaraTypeDefinition::GetBoolDef()), ENDCVariableType::Bool);
	TestEqual(TEXT("int"), Mapped(FNiagaraTypeDefinition::GetIntDef()), ENDCVariableType::Int32);
	TestEqual(TEXT("float"), Mapped(FNiagaraTypeDefinition::GetFloatDef()), ENDCVariableType::Float);
	// An editor-created "float" channel variable is really a double.
	TestEqual(TEXT("double"), Mapped(FNiagaraTypeHelper::GetDoubleDef()), ENDCVariableType::Float);
	TestEqual(TEXT("vec2"), Mapped(FNiagaraTypeDefinition::GetVec2Def()), ENDCVariableType::Vector2D);
	TestEqual(TEXT("vec3"), Mapped(FNiagaraTypeDefinition::GetVec3Def()), ENDCVariableType::Vector);
	TestEqual(TEXT("vec4"), Mapped(FNiagaraTypeDefinition::GetVec4Def()), ENDCVariableType::Vector4);
	TestEqual(TEXT("quat"), Mapped(FNiagaraTypeDefinition::GetQuatDef()), ENDCVariableType::Quat);
	TestEqual(TEXT("color"), Mapped(FNiagaraTypeDefinition::GetColorDef()), ENDCVariableType::LinearColor);
	TestEqual(TEXT("position"), Mapped(FNiagaraTypeDefinition::GetPositionDef()), ENDCVariableType::Position);
	TestEqual(TEXT("id"), Mapped(FNiagaraTypeDefinition::GetIDDef()), ENDCVariableType::ID);
	TestEqual(TEXT("spawn info"), Mapped(FNiagaraTypeDefinition(FNiagaraSpawnInfo::StaticStruct())), ENDCVariableType::SpawnInfo);

	// A channel variable the writer has no Write* overload for is reported, not silently mapped.
	TestEqual(TEXT("an unwritable type maps to Unsupported"),
		Mapped(FNiagaraTypeDefinition::GetMatrix4Def()), ENDCVariableType::Unsupported);

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderBlueprintSurfaceTest,
	"NDCBinder.Api.BlueprintSurface",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderBlueprintSurfaceTest::RunTest(const FString& Parameters)
{
	// A writer's configuration is authored in the details panel and read at write time. None of it is
	// reachable from a Blueprint graph, and that is a decision rather than an omission.
	//
	// The reason is in FNDCBoundFunctionCache and FNDCFieldCache: their keys deliberately do not
	// include the row's own names, because "the name is a member of the row that owns the cache and
	// cannot change without the row changing". Rebinding a row at runtime would break exactly that
	// assumption — the writer would go on calling the function the row used to name, with nothing to
	// say why. And a GameplayCueNotify writes from its CDO, so an edit would not even be local to one
	// instance.
	//
	// This is checked rather than commented because it is one BlueprintReadWrite away from being
	// untrue, and adding one is the kind of convenience that looks harmless in review.
	//
	// The visibility flags are the deliberate exception: they are arguments to each write rather than
	// state anything is cached against, so changing one from a graph is safe in a way changing a row
	// is not. Named here one by one, so a fourth has to be added on purpose.
	static const TSet<FName> BlueprintWritable =
	{
		TEXT("bVisibleToGame"),
		TEXT("bVisibleToCPU"),
		TEXT("bVisibleToGPU"),
	};

	const UScriptStruct* const Structs[] =
	{
		FNDCBinder::StaticStruct(),
		FNDCVariableBinding::StaticStruct(),
		FNDCContextBinding::StaticStruct(),
	};

	for (const UScriptStruct* Struct : Structs)
	{
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const bool bExpected = BlueprintWritable.Contains(It->GetFName());
			TestEqual(
				FString::Printf(TEXT("%s::%s reaches Blueprint graphs exactly as intended"),
					*Struct->GetName(), *It->GetName()),
				It->HasAnyPropertyFlags(CPF_BlueprintVisible), bExpected);
		}
	}

	// And the one node that does take a writer takes it to write WITH: a const reference, so the graph
	// cannot be handed an edit even if a property ever became visible.
	const UFunction* const WriteNode =
		UNDCBinderLibrary::StaticClass()->FindFunctionByName(TEXT("WriteToDataChannel"));
	if (!WriteNode)
	{
		AddError(TEXT("the Blueprint write node is gone, so what it takes cannot be checked"));
		return false;
	}

	// The pin an author connects the binder to, by name: it is what the node shows, so renaming it
	// silently would orphan the connection in every graph that placed the node.
	const FProperty* const BinderParam = WriteNode->FindPropertyByName(TEXT("Binder"));
	if (!BinderParam)
	{
		AddError(TEXT("the Blueprint write node no longer takes a parameter named Binder"));
		return false;
	}
	TestTrue(TEXT("the Blueprint write node takes the binder by const reference"),
		BinderParam->HasAllPropertyFlags(CPF_ConstParm | CPF_ReferenceParm));

	return true;
}

//~ Both of these drive ValidateBindings and SyncBindingsWithChannel, which exist only under
//~ WITH_EDITOR — like every other editor-only case in this file, and for the same reason: this module
//~ has to keep BUILDING outside the editor, because the benchmark it also carries has to run in a
//~ configuration a shipped game is built in. Without this guard it does not, which is how a
//~ Development game build failed on these two lines.
#if WITH_EDITOR

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderValidationDriftTest,
	"NDCBinder.Validation.Drift",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderValidationDriftTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

	// Drift is the failure this whole report exists for: the asset still holds the row it was
	// authored with, and what the row names is gone. Write time says nothing — the value is simply
	// skipped — so this is the half that has to be an ERROR, because an error is what fails the
	// owning Blueprint's compile.
	const UClass* const Host = UNDCBinderTestFunctionHost::StaticClass();

	FNDCBinder Writer = MakeWriter(FNDCBinderTestContext::StaticStruct());
	Writer.DataChannel = MakeChannelAsset({
		{ TEXT("Position"), FNiagaraTypeDefinition::GetVec3Def() },
		{ TEXT("Size"), FNiagaraTypeDefinition::GetFloatDef() },
	});
	if (!Writer.GetChannel() || !Writer.GetContextType())
	{
		AddError(TEXT("the test channel could not be built, so nothing below is being checked"));
		return false;
	}
	Writer.SyncBindingsWithChannel();

	TArray<FNDCVariableBinding>& Rows = Writer.GetMutableBindingsUnchecked();
	if (Rows.Num() != 2)
	{
		AddError(TEXT("the sync did not produce a row per channel variable"));
		return false;
	}

	// One of each kind of bound name, because each is resolved against something different: a
	// function against the owner's class, a field against the writer's Event Data Type, a context
	// row's field against the channel's access context type.
	Rows[0].Source = ENDCValueSource::Function;
	Rows[0].BoundFunction = TEXT("DepartedGetter");
	Rows[1].Source = ENDCValueSource::EventData;
	Rows[1].BoundEventDataField = TEXT("DepartedField");
	Writer.GetMutableContextBindingsUnchecked().Add(
		FNDCContextBinding::Make(TEXT("DepartedContextField"), TEXT("NoParams")));

	FDataValidationContext Context;
	const EDataValidationResult Result = Writer.ValidateBindings(Host, TEXT("NDCBinder"), Context);

	TestEqual(TEXT("a writer holding rows that cannot run fails validation"),
		Result, EDataValidationResult::Invalid);
	TestEqual(TEXT("each row that cannot run is one error"), static_cast<int32>(Context.GetNumErrors()), 3);
	TestEqual(TEXT("and none of them is softened into a warning"), static_cast<int32>(Context.GetNumWarnings()), 0);

	// The messages are read in a compiler log, where there is no row to click on and no panel to look
	// at, so each has to carry both what broke and which writer it belongs to.
	FString AllText;
	for (const FDataValidationContext::FIssue& Issue : Context.GetIssues())
	{
		AllText += Issue.Message.ToString();
	}
	TestTrue(TEXT("a message names the missing function"), AllText.Contains(TEXT("DepartedGetter")));
	TestTrue(TEXT("a message names the missing event data field"), AllText.Contains(TEXT("DepartedField")));
	TestTrue(TEXT("a message names the missing context field"), AllText.Contains(TEXT("DepartedContextField")));
	TestFalse(TEXT("every message names the writer it came from"),
		Context.GetIssues().ContainsByPredicate([](const FDataValidationContext::FIssue& Issue)
		{
			return !Issue.Message.ToString().Contains(TEXT("NDCBinder"));
		}));

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderValidationWideEnumTest,
	"NDCBinder.Validation.WideEnum",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderValidationWideEnumTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

#if WITH_EDITOR
	// The channel keeps an enum as an int and would hold any of these values; the writer carries a
	// byte, so the entries above 255 arrive wrapped. Nothing about authoring such a row looks wrong,
	// which is why it is worth a warning — and only a warning, because the entries that do fit keep
	// working and failing the compile over them would be wrong.
	const UClass* const Host = UNDCBinderTestFunctionHost::StaticClass();

	FNDCBinder Writer = MakeWriter(FNDCBinderTestContext::StaticStruct());
	Writer.DataChannel = MakeChannelAsset({
		{ TEXT("Wide"),   FNiagaraTypeDefinition(StaticEnum<ENDCBinderTestWideEnum>()) },
		{ TEXT("Narrow"), FNiagaraTypeDefinition(StaticEnum<ENDCBinderTestEnum>()) },
	});
	if (!Writer.GetChannel())
	{
		AddError(TEXT("the test channel could not be built, so nothing below is being checked"));
		return false;
	}
	Writer.SyncBindingsWithChannel();
	TestEqual(TEXT("one row per channel variable"), Writer.GetBindings().Num(), 2);

	FDataValidationContext Context;
	const EDataValidationResult Result = Writer.ValidateBindings(Host, TEXT("NDCBinder"), Context);

	TestEqual(TEXT("a wide enumeration does not fail the compile"), Result, EDataValidationResult::Valid);
	TestEqual(TEXT("and is no error"), static_cast<int32>(Context.GetNumErrors()), 0);
	TestEqual(TEXT("one warning, for the wide row and not the narrow one"),
		static_cast<int32>(Context.GetNumWarnings()), 1);
	if (Context.GetIssues().Num() == 1)
	{
		const FString Message = Context.GetIssues()[0].Message.ToString();
		TestTrue(TEXT("the warning names the row"), Message.Contains(TEXT("Wide")));
		TestTrue(TEXT("and the entry that does not fit"), Message.Contains(TEXT("300")));
	}
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderValidationUnfinishedTest,
	"NDCBinder.Validation.Unfinished",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderValidationUnfinishedTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

	// The other half of the split, and the reason there is a split at all: an error fails a compile,
	// a Blueprint compiles on every edit, and every asset passes through half-authored on its way to
	// being authored. Both states below are that, and neither may be an error.
	const UClass* const Host = UNDCBinderTestFunctionHost::StaticClass();

	{
		// Nothing picked at all. There is not even a set of rows to judge yet, since the channel is
		// what says which variables exist and what the access context is.
		FNDCBinder Writer = MakeWriter(FNDCBinderTestContext::StaticStruct());

		FDataValidationContext Context;
		const EDataValidationResult Result = Writer.ValidateBindings(Host, TEXT("NDCBinder"), Context);

		TestEqual(TEXT("a writer with no Data Channel passes"), Result, EDataValidationResult::Valid);
		TestEqual(TEXT("saying so as no error"), static_cast<int32>(Context.GetNumErrors()), 0);
		TestEqual(TEXT("and as one warning rather than one per row"), static_cast<int32>(Context.GetNumWarnings()), 1);
		TestTrue(TEXT("the warning says what is not picked"),
			Context.GetIssues().Num() == 1 && Context.GetIssues()[0].Message.ToString().Contains(TEXT("Data Channel")));
	}

	{
		// A row switched to read a function, with the function not chosen yet: the state between
		// picking a source and picking what it reads.
		FNDCBinder Writer = MakeWriter(FNDCBinderTestContext::StaticStruct());
		Writer.DataChannel = MakeChannelAsset({ { TEXT("Position"), FNiagaraTypeDefinition::GetVec3Def() } });
		if (!Writer.GetChannel())
		{
			AddError(TEXT("the test channel could not be built, so nothing below is being checked"));
			return false;
		}
		Writer.SyncBindingsWithChannel();
		Writer.GetMutableBindingsUnchecked()[0].Source = ENDCValueSource::Function;

		FDataValidationContext Context;
		const EDataValidationResult Result = Writer.ValidateBindings(Host, TEXT("NDCBinder"), Context);

		TestEqual(TEXT("a row with nothing picked yet passes"), Result, EDataValidationResult::Valid);
		TestEqual(TEXT("as no error"), static_cast<int32>(Context.GetNumErrors()), 0);
		TestEqual(TEXT("and as one warning"), static_cast<int32>(Context.GetNumWarnings()), 1);
	}

	{
		// And the writer the two above are the unfinished version of: everything picked, everything
		// resolvable, nothing to say about it at all.
		FNDCBinder Writer = MakeWriter(FNDCBinderTestContext::StaticStruct());
		Writer.DataChannel = MakeChannelAsset({ { TEXT("Position"), FNiagaraTypeDefinition::GetVec3Def() } });
		Writer.SyncBindingsWithChannel();
		Writer.GetMutableBindingsUnchecked()[0].Source = ENDCValueSource::Function;
		Writer.GetMutableBindingsUnchecked()[0].BoundFunction = TEXT("NoParams");

		FDataValidationContext Context;
		const EDataValidationResult Result = Writer.ValidateBindings(Host, TEXT("NDCBinder"), Context);

		TestEqual(TEXT("a fully authored writer passes"), Result, EDataValidationResult::Valid);
		TestEqual(TEXT("with no error"), static_cast<int32>(Context.GetNumErrors()), 0);
		TestEqual(TEXT("and no warning"), static_cast<int32>(Context.GetNumWarnings()), 0);
	}

	return true;
}

#endif // WITH_EDITOR


/**
 * The whole pipeline, once, with the values read back out of the channel's buffer.
 *
 * Every other test here takes a write apart — a signature, a cache key, a resolved offset — and not
 * one of them writes anything, because WriteToChannel needs a UWorld and a Niagara handler and no
 * test had either. So the thing this plugin exists to do was covered by eight cue assets working in
 * the game, which is not a test that runs anywhere.
 *
 * The world is made here rather than borrowed: it makes this the same test in the editor, in a
 * commandlet and in a packaged build, and nothing it writes lands in a world anything else is using.
 *
 * Two elements, deliberately. They differ only in the event data handed to each, which is what proves
 * the per-element form carries it — to an event data row and through a bound function alike. That is
 * also the only coverage the multi-element API has.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderEndToEndWriteTest,
	"NDCBinder.Write.EndToEnd",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderEndToEndWriteTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

	//~ Unnamed on purpose: the same editor session can run this twice, and a destroyed world is only
	//~ marked for collection, so a fixed name would collide with the one the previous run left behind.
	UWorld* const World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld=*/ false);
	if (!World)
	{
		AddError(TEXT("could not create a world to write in"));
		return false;
	}

	FNiagaraWorldManager* const WorldMan = FNiagaraWorldManager::Get(World);
	if (!WorldMan)
	{
		AddError(TEXT("the new world has no Niagara world manager, so nothing can be written to a channel in it"));
		World->DestroyWorld(/*bInformEngineOfWorld=*/ false);
		return false;
	}

	// One variable per way a row can get its value, plus the three scalar kinds, so the read-back
	// covers the switch in WriteBindingsInternal rather than one arm of it.
	UNiagaraDataChannelAsset* const Asset = MakeChannelAsset({
		{ TEXT("FromEventData"), FNiagaraTypeDefinition::GetVec3Def() },
		{ TEXT("FromFunction"),  FNiagaraTypeDefinition::GetVec3Def() },
		{ TEXT("FromConstant"),  FNiagaraTypeDefinition::GetVec3Def() },
		{ TEXT("Count"),         FNiagaraTypeDefinition::GetIntDef() },
		{ TEXT("Scale"),         FNiagaraTypeDefinition::GetFloatDef() },
		{ TEXT("Flag"),          FNiagaraTypeDefinition::GetBoolDef() },
	});
	UNiagaraDataChannel* const Channel = Asset ? Asset->Get() : nullptr;
	if (!Channel)
	{
		AddError(TEXT("could not build a channel to write to"));
		World->DestroyWorld(/*bInformEngineOfWorld=*/ false);
		return false;
	}
	Asset->AddToRoot();

	// Without this the world has no handler for the channel and every write below returns false.
	WorldMan->InitDataChannel(Channel, /*bForce=*/ true);

	UNDCBinderTestFunctionHost* const Host = NewObject<UNDCBinderTestFunctionHost>(GetTransientPackage());
	Host->AddToRoot();

	FNDCBinder Writer = MakeWriter(FNDCBinderTestContext::StaticStruct());
	Writer.DataChannel = Asset;
	{
		TArray<FNDCVariableBinding>& Rows = Writer.GetMutableBindingsUnchecked();
		Rows.Add(FNDCVariableBinding::MakeEventDataBinding(TEXT("FromEventData"), ENDCVariableType::Vector, TEXT("Location")));
		//~ Returns the event data's own Location, which is what makes element 1 prove something.
		Rows.Add(FNDCVariableBinding::MakeFunctionBinding(TEXT("FromFunction"), ENDCVariableType::Vector, TEXT("ConstEventDataRef")));

		FNDCVariableBinding Constant;
		Constant.VarName = TEXT("FromConstant");
		Constant.Type = ENDCVariableType::Vector;
		Constant.VectorValue = FVector(9.0, 8.0, 7.0);
		Rows.Add(Constant);

		Rows.Add(FNDCVariableBinding::MakeFunctionBinding(TEXT("Count"), ENDCVariableType::Int32, TEXT("ReturnsInt")));
		Rows.Add(FNDCVariableBinding::MakeFunctionBinding(TEXT("Scale"), ENDCVariableType::Float, TEXT("ReturnsDouble")));
		Rows.Add(FNDCVariableBinding::MakeFunctionBinding(TEXT("Flag"), ENDCVariableType::Bool, TEXT("ReturnsBool")));
		// A row for a variable this channel does not have. It must be skipped without taking the
		// rest of the write with it, which is the whole "never destructive" policy at write time.
		Rows.Add(FNDCVariableBinding::MakeFunctionBinding(TEXT("Departed"), ENDCVariableType::Vector, TEXT("NoParams")));
	}

	FNDCBinderTestContext First;
	First.Location = FVector(11.0, 12.0, 13.0);
	FNDCBinderTestContext Second;
	Second.Location = FVector(21.0, 22.0, 23.0);

	FNDCWriteScope Scope;
	auto Cleanup = [&Scope, WorldMan, Channel, Asset, Host, World]()
	{
		Scope.End();
		WorldMan->RemoveDataChannel(Channel);
		Asset->RemoveFromRoot();
		Host->RemoveFromRoot();
		World->DestroyWorld(/*bInformEngineOfWorld=*/ false);
	};

	FNDCAccessContextInst Context;
	if (!TestTrue(TEXT("the access context resolves"), Writer.ResolveAccessContext(Context, Host, FConstStructView::Make(First))))
	{
		Cleanup();
		return false;
	}

	if (!TestTrue(TEXT("a two-element write opens"), Writer.BeginWrite(Scope, World, Context, /*Count=*/ 2, Host)))
	{
		Cleanup();
		return false;
	}

	Writer.WriteBindings(Scope, /*Index=*/ 0, Host, FConstStructView::Make(First));
	Writer.WriteBindings(Scope, /*Index=*/ 1, Host, FConstStructView::Make(Second));

	const FNiagaraDataChannelGameDataPtr& Data = Scope.GetData();
	if (!TestTrue(TEXT("the write has a destination buffer"), Data.IsValid()))
	{
		Cleanup();
		return false;
	}

	// Read back through the same buffer indices the write used: the row order is the order they were
	// added, and VarOffsets is parallel to it.
	const int32 Base = Scope.GetStartIndex();
	TestEqual(TEXT("one resolved offset per row"), Writer.VarOffsets.Num(), Writer.GetBindings().Num());
	TestEqual(TEXT("the row for a departed variable resolves to nothing"), Writer.VarOffsets.Last(), (int32)INDEX_NONE);

	auto ReadVector = [&](int32 RowIndex, int32 Element)
	{
		FVector Value = FVector::ZeroVector;
		Data->Read<FVector>(Writer.VarOffsets[RowIndex], Base + Element, Value, /*bPreviousFrame=*/ false);
		return Value;
	};

	TestEqual(TEXT("element 0: the event data row carries that element's value"), ReadVector(0, 0), First.Location);
	TestEqual(TEXT("element 0: the bound function was given that element's event data"), ReadVector(1, 0), First.Location);
	TestEqual(TEXT("element 0: the constant row writes its constant"), ReadVector(2, 0), FVector(9.0, 8.0, 7.0));

	// The point of the second element: same rows, same writer, different event data.
	TestEqual(TEXT("element 1: the event data row carries the OTHER element's value"), ReadVector(0, 1), Second.Location);
	TestEqual(TEXT("element 1: and so does the bound function"), ReadVector(1, 1), Second.Location);
	TestEqual(TEXT("element 1: the constant row is unchanged by any of it"), ReadVector(2, 1), FVector(9.0, 8.0, 7.0));

	int32 Count = 0;
	Data->Read<int32>(Writer.VarOffsets[3], Base, Count, /*bPreviousFrame=*/ false);
	TestEqual(TEXT("an int row writes its function's return value"), Count, 7);

	double Scale = 0.0;
	Data->Read<double>(Writer.VarOffsets[4], Base, Scale, /*bPreviousFrame=*/ false);
	TestEqual(TEXT("a float row writes its function's return value"), Scale, 0.5);

	FNiagaraBool Flag;
	Data->Read<FNiagaraBool>(Writer.VarOffsets[5], Base, Flag, /*bPreviousFrame=*/ false);
	TestTrue(TEXT("a bool row writes its function's return value"), Flag.GetValue());

	// The buffer grew by exactly the two elements asked for, and by nothing the skipped row added.
	TestEqual(TEXT("the write claimed exactly two elements"), Data->Num(), Base + 2);

	Scope.End();

	// And the one-element convenience, which is what every cue in the project actually calls.
	TestTrue(TEXT("the single-element form writes too"),
		Writer.WriteToChannel(World, Host, FConstStructView::Make(First)));

	Cleanup();
	return true;
}


/**
 * Many elements in one write, from C++ and from the node a Blueprint would use.
 *
 * What has to be true for a batch to be worth anything is that each element gets its OWN event data —
 * a batch that handed the same payload to all of them would look identical from the outside, cost the
 * same and write the wrong thing. So the writer here binds a row to a function that keeps what it was
 * handed, and the assertion is the call record: one call per element, in order, each with that
 * element's value.
 *
 * The convenience form cannot be read back the way NDCBinder.Write.EndToEnd reads its scope — it owns
 * its scope and closes it — which is exactly why the record exists.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderManyElementsWriteTest,
	"NDCBinder.Write.ManyElements",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderManyElementsWriteTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderTestsPrivate;

	UWorld* const World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld=*/ false);
	if (!World)
	{
		AddError(TEXT("could not create a world to write in"));
		return false;
	}

	FNiagaraWorldManager* const WorldMan = FNiagaraWorldManager::Get(World);
	UNiagaraDataChannelAsset* const Asset = MakeChannelAsset({
		{ TEXT("FromFunction"),  FNiagaraTypeDefinition::GetVec3Def() },
		{ TEXT("FromEventData"), FNiagaraTypeDefinition::GetVec3Def() },
	});
	UNiagaraDataChannel* const Channel = Asset ? Asset->Get() : nullptr;
	if (!WorldMan || !Channel)
	{
		AddError(TEXT("could not set up a world with a channel to write to"));
		World->DestroyWorld(/*bInformEngineOfWorld=*/ false);
		return false;
	}
	Asset->AddToRoot();
	WorldMan->InitDataChannel(Channel, /*bForce=*/ true);

	UNDCBinderTestFunctionHost* const Host = NewObject<UNDCBinderTestFunctionHost>(GetTransientPackage());
	Host->AddToRoot();
	// What the Blueprint node will take its world from.
	Host->WorldForTests = World;

	FNDCBinder Writer = MakeWriter(FNDCBinderTestContext::StaticStruct());
	Writer.DataChannel = Asset;
	{
		TArray<FNDCVariableBinding>& Rows = Writer.GetMutableBindingsUnchecked();
		Rows.Add(FNDCVariableBinding::MakeFunctionBinding(TEXT("FromFunction"), ENDCVariableType::Vector, TEXT("RecordAndReturnLocation")));
		Rows.Add(FNDCVariableBinding::MakeEventDataBinding(TEXT("FromEventData"), ENDCVariableType::Vector, TEXT("Location")));
	}

	FNDCBinderTestContext First;
	First.Location = FVector(1.0, 0.0, 0.0);
	FNDCBinderTestContext Second;
	Second.Location = FVector(2.0, 0.0, 0.0);
	FNDCBinderTestContext Third;
	Third.Location = FVector(3.0, 0.0, 0.0);

	auto Cleanup = [WorldMan, Channel, Asset, Host, World]()
	{
		UNDCBinderTestFunctionHost::ResetCallRecord();
		WorldMan->RemoveDataChannel(Channel);
		Asset->RemoveFromRoot();
		Host->RemoveFromRoot();
		World->DestroyWorld(/*bInformEngineOfWorld=*/ false);
	};

	const TArray<FConstStructView> Views =
	{
		FConstStructView::Make(First),
		FConstStructView::Make(Second),
		FConstStructView::Make(Third),
	};

	UNDCBinderTestFunctionHost::ResetCallRecord();
	if (!TestTrue(TEXT("a three-element write goes through"), Writer.WriteToChannel(World, Host, Views)))
	{
		Cleanup();
		return false;
	}

	const TArray<FVector>& Record = UNDCBinderTestFunctionHost::RecordedLocations;
	if (!TestEqual(TEXT("the bound row ran once per element"), Record.Num(), 3))
	{
		Cleanup();
		return false;
	}
	// The whole claim: each element was given its own payload, in order.
	TestEqual(TEXT("element 0 was handed its own event data"), Record[0], First.Location);
	TestEqual(TEXT("element 1 was handed its own event data"), Record[1], Second.Location);
	TestEqual(TEXT("element 2 was handed its own event data"), Record[2], Third.Location);

	// The single-element form still means exactly one element, through the same code.
	UNDCBinderTestFunctionHost::ResetCallRecord();
	TestTrue(TEXT("the single-element form still writes"),
		Writer.WriteToChannel(World, Host, FConstStructView::Make(Second)));
	TestEqual(TEXT("and runs the row exactly once"), Record.Num(), 1);
	if (Record.Num() == 1)
	{
		TestEqual(TEXT("with the event data it was given"), Record[0], Second.Location);
	}

	// Nothing to write is not a write.
	UNDCBinderTestFunctionHost::ResetCallRecord();
	TestFalse(TEXT("an empty batch writes nothing"),
		Writer.WriteToChannel(World, Host, TConstArrayView<FConstStructView>()));
	TestEqual(TEXT("and calls nothing"), Record.Num(), 0);

	//~ The Blueprint node reaches the same place, but it cannot be called from here: it is a custom
	//~ thunk, and only compiled bytecode can hand one a wildcard array. What it does before reaching
	//~ this code is covered by NDCBinder.Api.WildcardEventData.

	Cleanup();
	return true;
}


/**
 * What the Blueprint node's wildcard Event Data pin resolves to, and that resolving it copies nothing.
 *
 * The pin exists to spare a graph the box: connecting a cue's Parameters straight to it should hand
 * the write the graph's own struct, not a deep copy of it. That claim is an ADDRESS — the view has to
 * point at the caller's value — which is what this asserts, because nothing else would notice a copy
 * creeping back in.
 *
 * The thunk that reads the pin cannot be called from here: a custom thunk takes its arguments off the
 * VM stack, so only compiled Blueprint bytecode can feed it. This covers the half with a decision in
 * it; the stack reading is the engine's own pattern, and a graph that calls the node is what proves
 * that half.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderWildcardEventDataTest,
	"NDCBinder.Api.WildcardEventData",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderWildcardEventDataTest::RunTest(const FString& Parameters)
{
	FNDCBinderTestContext Payload;
	Payload.Location = FVector(4.0, 5.0, 6.0);
	Payload.Tags = { TEXT("Alpha") };

	// A struct connected as itself: viewed exactly where it lives.
	{
		const FConstStructView View = UNDCBinderLibrary::UnwrapEventData(
			FNDCBinderTestContext::StaticStruct(), reinterpret_cast<const uint8*>(&Payload));

		TestEqual(TEXT("a plain struct keeps its own type"),
			(const void*)View.GetScriptStruct(), (const void*)FNDCBinderTestContext::StaticStruct());
		// The whole point of the wildcard: the write reads the caller's memory, not a copy of it.
		TestEqual(TEXT("and is viewed at the caller's own address, not copied"),
			(const void*)View.GetMemory(), (const void*)&Payload);
	}

	// A boxed struct: unwrapped, so the rows see the payload rather than the box.
	{
		const FInstancedStruct Boxed = FInstancedStruct::Make(Payload);
		const FConstStructView View = UNDCBinderLibrary::UnwrapEventData(
			TBaseStructure<FInstancedStruct>::Get(), reinterpret_cast<const uint8*>(&Boxed));

		TestEqual(TEXT("an instanced struct resolves to what is inside it"),
			(const void*)View.GetScriptStruct(), (const void*)FNDCBinderTestContext::StaticStruct());
		TestEqual(TEXT("at the boxed value's address, still without a copy"),
			(const void*)View.GetMemory(), (const void*)Boxed.GetMemory());
		TestNotEqual(TEXT("and not as the box itself"),
			(const void*)View.GetScriptStruct(), (const void*)TBaseStructure<FInstancedStruct>::Get());
	}

	// An unconnected pin. The writer treats an invalid view as "no event data", which is what a writer
	// whose bound functions take no parameter wants.
	{
		TestFalse(TEXT("no type means no event data"),
			UNDCBinderLibrary::UnwrapEventData(nullptr, reinterpret_cast<const uint8*>(&Payload)).IsValid());
		TestFalse(TEXT("no address means no event data"),
			UNDCBinderLibrary::UnwrapEventData(FNDCBinderTestContext::StaticStruct(), nullptr).IsValid());
	}

	//~ The batched node's half of the same question. A real FArrayProperty is not something a test can
	//~ build, so these borrow one from a struct that has the array declared on it.
	const UScriptStruct* const NestedStruct = FNDCBinderTestNestedContext::StaticStruct();
	FNDCBinderTestNestedContext Nested;
	Nested.Many.AddDefaulted(3);
	Nested.Many[0].Point = FVector(1.0, 0.0, 0.0);
	Nested.Many[1].Point = FVector(2.0, 0.0, 0.0);
	Nested.Many[2].Point = FVector(3.0, 0.0, 0.0);

	// An array of plain structs: one view per element, each where the element already is.
	{
		const FArrayProperty* const ManyProperty = CastField<FArrayProperty>(NestedStruct->FindPropertyByName(TEXT("Many")));
		if (!ManyProperty)
		{
			AddError(TEXT("the test struct no longer has the array these assertions are written against"));
			return false;
		}

		FNDCEventDataViews Views;
		TestTrue(TEXT("an array of structs is readable"),
			UNDCBinderLibrary::CollectEventDataViews(ManyProperty, ManyProperty->ContainerPtrToValuePtr<void>(&Nested), Views));
		if (TestEqual(TEXT("one view per element"), Views.Num(), 3))
		{
			for (int32 Index = 0; Index < 3; ++Index)
			{
				TestEqual(*FString::Printf(TEXT("element %d keeps its type"), Index),
					(const void*)Views[Index].GetScriptStruct(), (const void*)FNDCBinderTestLeaf::StaticStruct());
				// Again the address: the batch must read the graph's array, not a copy of it.
				TestEqual(*FString::Printf(TEXT("element %d is viewed in the array itself"), Index),
					(const void*)Views[Index].GetMemory(), (const void*)&Nested.Many[Index]);
			}
		}
	}

	// An array of instanced structs: unwrapped element by element, so a payload that arrived boxed
	// costs nothing extra here either.
	{
		Nested.Boxed.Add(FInstancedStruct::Make(Payload));
		Nested.Boxed.Add(FInstancedStruct::Make(Payload));

		const FArrayProperty* const BoxedProperty = CastField<FArrayProperty>(NestedStruct->FindPropertyByName(TEXT("Boxed")));
		FNDCEventDataViews Views;
		TestTrue(TEXT("an array of instanced structs is readable"),
			UNDCBinderLibrary::CollectEventDataViews(BoxedProperty, BoxedProperty ? BoxedProperty->ContainerPtrToValuePtr<void>(&Nested) : nullptr, Views));
		if (TestEqual(TEXT("one view per box"), Views.Num(), 2))
		{
			TestEqual(TEXT("resolving to what is inside the box"),
				(const void*)Views[0].GetScriptStruct(), (const void*)FNDCBinderTestContext::StaticStruct());
			TestEqual(TEXT("at the boxed value's own address"),
				(const void*)Views[0].GetMemory(), (const void*)Nested.Boxed[0].GetMemory());
		}
	}

	// An array of something that is not a struct: the one way a wildcard array can be connected wrong.
	{
		const FArrayProperty* const TagsProperty =
			CastField<FArrayProperty>(FNDCBinderTestContext::StaticStruct()->FindPropertyByName(TEXT("Tags")));
		FNDCEventDataViews Views;
		TestFalse(TEXT("an array of names is refused"),
			UNDCBinderLibrary::CollectEventDataViews(TagsProperty, TagsProperty ? TagsProperty->ContainerPtrToValuePtr<void>(&Payload) : nullptr, Views));
		TestEqual(TEXT("and produces no views"), Views.Num(), 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
