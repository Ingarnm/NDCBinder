// NDCBinderCustomizationTests.cpp
//
// Tests for the details panel's own logic: the bind menu, the rows the panel does and does not draw,
// the hint it shows when an Event Data Type is missing, the signature of the function its
// Create Binding button writes, and which of the plugin's types a Blueprint graph is allowed to
// build for itself.
//
// They live in the EDITOR module, not the test module, because what they check sits in the
// customization's private namespace and the automation-test module is a DeveloperTool one that an
// editor module must not depend on. NDCBinderCustomizationInternal.h is the seam.
//
// The classes with the function shapes these need come from NDCBinderTests BY PATH rather than by
// linking to it, for the same reason.
//
// Each of these was checked by falsification: put the bug back and watch it fail.

#include "NDCBinderCustomizationInternal.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_MakeStruct.h"
#include "Engine/Blueprint.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "NDCBinder.h"
#include "NiagaraDataChannelAccessContext.h"
#include "NiagaraDataChannel_GameplayBurst.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

/**
 * The bind menu's event data section, built for real.
 *
 * It exists because the two ways this section has broken were both invisible to every other test here
 * and to a validation sweep over every asset in the project: a section opened inside the engine's own
 * one (an assertion on the first click), and a delegate left unbound that SPropertyBinding reads as
 * "deny everything" (an empty menu, silently). Both are answered by the same question — does building
 * this menu produce entries — which is what this asks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderEventDataMenuTest,
	"NDCBinder.Editor.EventDataMenu",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderEventDataMenuTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderCustomizationPrivate;

	// Any struct with a member worth binding and a struct member to open a submenu on. The access
	// context is one this module already knows about, and its Location is an FVector — a bindable leaf
	// and a struct to go into at the same time.
	FNDCBinder Writer;
	Writer.SetEventDataTypeUnchecked(FNDCAccessContext::StaticStruct());

	const TFunction<bool(const FNDCBinder&, const FProperty*)> AcceptsAnything =
		[](const FNDCBinder&, const FProperty*) { return true; };
	const TFunction<void(FBoundTo)> IgnorePick = [](FBoundTo) {};

	FMenuBuilder MenuBuilder(/*bInShouldCloseWindowAfterMenuSelection=*/ true, nullptr);
	AddEventDataEntries(MenuBuilder, Writer, AcceptsAnything, IgnorePick);

	const int32 NumBlocks = MenuBuilder.GetMultiBox()->GetBlocks().Num();
	if (!TestTrue(TEXT("building the event data section produces entries"), NumBlocks > 0))
	{
		AddError(TEXT("the section came out empty — a filter delegate is refusing everything, or none is bound at all"));
		return false;
	}

	// Making the widget is what catches an unbalanced section: FMenuBuilder asserts on one, and an
	// assertion inside AddEventDataEntries would already have taken the line above with it.
	TestNotNull(TEXT("and the menu widget is built from them"), &MenuBuilder.MakeWidget().Get());

	// A writer with no event data type has nothing to offer and must not pretend otherwise.
	FNDCBinder Undeclared;
	FMenuBuilder EmptyBuilder(/*bInShouldCloseWindowAfterMenuSelection=*/ true, nullptr);
	AddEventDataEntries(EmptyBuilder, Undeclared, AcceptsAnything, IgnorePick);
	TestEqual(TEXT("a writer with no event data type adds nothing"),
		EmptyBuilder.GetMultiBox()->GetBlocks().Num(), 0);

	return true;
}


/**
 * The one rule the plugin ships with, and the shape of the key it is written in.
 *
 * The key is the point: a bare name — which this used to be — hides a field of that name on ANY
 * context, and misses the same field under another name on another one. So what is asserted here is
 * not "bReturnExistingSystems is hidden" but where the rule does and does not reach.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderHiddenContextFieldTest,
	"NDCBinder.Editor.HiddenContextFields",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderHiddenContextFieldTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderCustomizationPrivate;

	const UScriptStruct* const Context = FNDCAccessContext::StaticStruct();
	const UScriptStruct* const Base = FNDCAccessContextBase::StaticStruct();
	const UScriptStruct* const Burst = FNDCAccessContext_GameplayBurst::StaticStruct();

	const FProperty* const Hidden = Context->FindPropertyByName(TEXT("bReturnExistingSystems"));
	const FProperty* const Shown = Context->FindPropertyByName(TEXT("Location"));
	if (!Hidden || !Shown)
	{
		AddError(TEXT("FNDCAccessContext no longer has the fields this test is written against"));
		return false;
	}

	TestTrue(TEXT("the shipped rule hides the field on the context that declares it"),
		IsHiddenContextField(Context, *Hidden));
	// The rule is written against the declaring type, so everything derived from it inherits both the
	// field and the reason. This is the half a bare name got right by accident.
	TestTrue(TEXT("and on a context derived from it"),
		IsHiddenContextField(Burst, *Hidden));
	// And this is the half it got wrong: a rule does not reach a type that is not below the one it
	// names, even when the name would have matched.
	TestFalse(TEXT("but not on the base, which does not declare it"),
		IsHiddenContextField(Base, *Hidden));

	TestFalse(TEXT("another field of the same context is untouched"),
		IsHiddenContextField(Context, *Shown));
	TestFalse(TEXT("and no context hides nothing at all"),
		IsHiddenContextField(nullptr, *Hidden));

	return true;
}

/**
 * The three hints that tell an author their bindings need an Event Data Type.
 *
 * All three were dead for months — the helper they asked returned null for every possible signature —
 * and nothing noticed, because the only thing that could have noticed was a panel nobody was testing.
 * The helper itself is covered in the runtime tests now; this covers the answers built on it.
 *
 * The class with the right function shapes lives in the test module, found by path rather than linked:
 * an editor module has no business depending on a DeveloperTool one.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderEventDataTypeHintTest,
	"NDCBinder.Editor.EventDataTypeHint",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderEventDataTypeHintTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderCustomizationPrivate;

	const UClass* const Host = FindObject<UClass>(nullptr, TEXT("/Script/NDCBinderTests.NDCBinderTestFunctionHost"));
	UScriptStruct* const EventData = FindObject<UScriptStruct>(nullptr, TEXT("/Script/NDCBinderTests.NDCBinderTestContext"));
	if (!Host || !EventData)
	{
		AddInfo(TEXT("NDCBinderTests is not loaded, so there is no class with the signatures this needs — skipped"));
		return true;
	}

	const TFunction<bool(const FNDCBinder&, const UFunction*)> AcceptsVector =
		[](const FNDCBinder& W, const UFunction* F) { return W.IsValidValueFunction(F, ENDCVariableType::Vector); };

	// The hint in an empty bind menu: "N functions could bind if you declared an Event Data Type".
	FNDCBinder Undeclared;
	const int32 NumBlocked = CountFunctionsNeedingEventDataType(Undeclared, Host, AcceptsVector);
	TestTrue(TEXT("a writer with no event data type is told how many functions that costs it"), NumBlocked > 0);

	// And says nothing once it is declared: a rejection then has some other cause, and offering this
	// fix would send the author to a setting that is already correct.
	FNDCBinder Declared;
	Declared.SetEventDataTypeUnchecked(EventData);
	TestEqual(TEXT("and is told nothing once it is declared"),
		CountFunctionsNeedingEventDataType(Declared, Host, AcceptsVector), 0);

	// The banner over the rows: a bound row that would work if the type were declared names the type.
	FNDCBinder Bound;
	Bound.GetMutableBindingsUnchecked() = { FNDCVariableBinding::MakeFunctionBinding(TEXT("Position"), ENDCVariableType::Vector, TEXT("EventDataOnly")) };

	int32 NumAffected = INDEX_NONE;
	UScriptStruct* const Expected = FindExpectedEventDataType(Bound, Host, NumAffected);
	TestEqual(TEXT("a broken row names the struct that would revive it"),
		(const void*)Expected, (const void*)EventData);
	TestEqual(TEXT("and says how many rows want it"), NumAffected, 1);

	// With the right type declared the row is simply fine, and the banner has nothing to say.
	Bound.SetEventDataTypeUnchecked(EventData);
	NumAffected = INDEX_NONE;
	TestNull(TEXT("a working row asks for nothing"), FindExpectedEventDataType(Bound, Host, NumAffected));
	TestEqual(TEXT("and affects no rows"), NumAffected, 0);

	return true;
}


/**
 * What a generated getter's event data parameter compiles down to.
 *
 * The one thing this asserts is invisible everywhere else: whether the parameter arrives as a const
 * reference, which the call path aliases, or by value, which it deep copies once per bound row per
 * write. Both compile, both bind, both produce identical results, and a designer reading the graph
 * cannot tell them apart — so nothing but this test would notice the flags in MakeEventDataPinType
 * going away.
 *
 * The other two shapes are here as the reason it cannot be left to the author: by value is what a
 * hand-made pin gives, and a reference WITHOUT const — the only thing the Blueprint UI's
 * "Pass-by-Reference" checkbox can produce — is not bindable at all.
 *
 * A whole Blueprint is compiled rather than the pin type inspected, because the pin flags are not
 * the claim; what the Kismet compiler does with them is.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderGeneratedBindingSignatureTest,
	"NDCBinder.Editor.GeneratedBindingSignature",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderGeneratedBindingSignatureTest::RunTest(const FString& Parameters)
{
	using namespace NDCBinderCustomizationPrivate;

	UScriptStruct* const EventData = FNDCAccessContext::StaticStruct();

	// Uniquely named: the same editor session can run this test twice, and CreateBlueprint would be
	// making a second object of one name in one outer.
	const FName BlueprintName = MakeUniqueObjectName(
		GetTransientPackage(), UBlueprint::StaticClass(), TEXT("NDCBinderGeneratedBindingTestBP"));
	UBlueprint* const Blueprint = FKismetEditorUtilities::CreateBlueprint(
		UObject::StaticClass(), GetTransientPackage(), BlueprintName, BPTYPE_Normal);
	if (!Blueprint)
	{
		AddError(TEXT("could not create a Blueprint to generate a binding function on"));
		return false;
	}

	// The by-value and mutable-reference shapes the generator deliberately does not produce.
	FEdGraphPinType ByValue;
	ByValue.PinCategory = UEdGraphSchema_K2::PC_Struct;
	ByValue.PinSubCategoryObject = EventData;

	FEdGraphPinType MutableRef = ByValue;
	MutableRef.bIsReference = true;

	const FEdGraphPinType ReturnType = GetValueReturnPinType(ENDCVariableType::Vector, nullptr);

	// Generated, then the two controls, all through the same builder the panel uses.
	const UEdGraph* const GeneratedGraph = AddBindingFunctionGraph(Blueprint, TEXT("GeneratedGetter"), ReturnType, EventData);
	const UEdGraph* const ByValueGraph = AddBindingFunctionGraph(Blueprint, TEXT("ByValueGetter"), ReturnType, EventData, &ByValue);
	const UEdGraph* const MutableGraph = AddBindingFunctionGraph(Blueprint, TEXT("MutableRefGetter"), ReturnType, EventData, &MutableRef);
	if (!GeneratedGraph || !ByValueGraph || !MutableGraph)
	{
		AddError(TEXT("one of the function graphs did not come out, so nothing below proves anything"));
		return false;
	}

	// A function's name is its graph's; the compile below is what turns one into the other.
	const FName Generated = GeneratedGraph->GetFName();
	const FName ByValueName = ByValueGraph->GetFName();
	const FName MutableName = MutableGraph->GetFName();

	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestNotNull(TEXT("the Blueprint compiled"), Blueprint->GeneratedClass.Get()))
	{
		return false;
	}

	// The first parameter of a compiled function, or null.
	auto FindEventDataParm = [&Blueprint](FName FunctionName) -> const FProperty*
	{
		const UFunction* const Func = Blueprint->GeneratedClass->FindFunctionByName(FunctionName);
		if (!Func)
		{
			return nullptr;
		}
		for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (!It->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				return *It;
			}
		}
		return nullptr;
	};

	const FProperty* const GeneratedParm = FindEventDataParm(Generated);
	if (!TestNotNull(TEXT("the generated function has an event data parameter"), GeneratedParm))
	{
		return false;
	}

	// The claim. ByValue here would mean a deep copy of the event data struct on every bound call.
	TestEqual(TEXT("the generated event data parameter is a const reference, so the call aliases it"),
		(int32)FNDCBinder::GetEventDataPassing(GeneratedParm),
		(int32)ENDCEventDataPassing::ByConstReference);

	// And it is still a binding: the shape has to pass the same signature check the write path applies.
	FNDCBinder Writer;
	Writer.SetEventDataTypeUnchecked(EventData);
	const UFunction* const GeneratedFunc = Blueprint->GeneratedClass->FindFunctionByName(Generated);
	TestTrue(TEXT("and the generated function binds"),
		Writer.IsValidValueFunction(GeneratedFunc, ENDCVariableType::Vector));

	// The controls, which are what make the assertion above mean anything: the same builder with the
	// flags off produces the slow shape, and with only bIsReference produces one that cannot bind.
	if (const FProperty* const ByValueParm = FindEventDataParm(ByValueName))
	{
		TestEqual(TEXT("a plain struct pin compiles to a by-value parameter — the copy this avoids"),
			(int32)FNDCBinder::GetEventDataPassing(ByValueParm),
			(int32)ENDCEventDataPassing::ByValue);
	}
	else
	{
		AddError(TEXT("the by-value control has no parameter, so it proves nothing"));
	}

	if (const FProperty* const MutableParm = FindEventDataParm(MutableName))
	{
		TestEqual(TEXT("a reference pin without const compiles to a mutable reference"),
			(int32)FNDCBinder::GetEventDataPassing(MutableParm),
			(int32)ENDCEventDataPassing::ByMutableReference);
		TestFalse(TEXT("and is refused as a binding, which is why const cannot be left to the author"),
			Writer.IsValidValueFunction(Blueprint->GeneratedClass->FindFunctionByName(MutableName), ENDCVariableType::Vector));
	}
	else
	{
		AddError(TEXT("the mutable-reference control has no parameter, so it proves nothing"));
	}

	return true;
}
/**
 * Which of the plugin's types a Blueprint graph may create.
 *
 * Three cases gated three different ways — see the block above ENDCVariableType for why — and every
 * one of them is a single specifier away from silently coming back. The likely regression is someone
 * putting BlueprintType on a binding row "so it is easier to work with", which would put a Make node
 * for an internal row back in everyone's palette.
 *
 * The writer's Break side is checked through its metadata rather than through
 * UK2Node_BreakStruct::CanBeBroken, the one function of this group the engine does not export. That
 * turns out to be the better check anyway: it pins both halves of the mechanism — the gate is armed,
 * and what it names resolves to nothing — where the observable answer alone would not say why.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderGraphSurfaceTest,
	"NDCBinder.Editor.GraphSurface",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderGraphSurfaceTest::RunTest(const FString& Parameters)
{
	// The writer: ownable by a Blueprint, not assemblable in one.
	const UScriptStruct* const Writer = FNDCBinder::StaticStruct();
	TestTrue(TEXT("a Blueprint may still declare a writer variable of its own"),
		UEdGraphSchema_K2::IsAllowableBlueprintVariableType(Writer));
	TestFalse(TEXT("but cannot assemble one with a Make node"),
		UK2Node_MakeStruct::CanBeMade(Writer));
	TestTrue(TEXT("a writer pin cannot be split"),
		Writer->HasMetaData(FBlueprintMetadata::MD_NativeDisableSplitPin));

	for (const FName Gate : { FBlueprintMetadata::MD_NativeMakeFunction, FBlueprintMetadata::MD_NativeBreakFunction })
	{
		const FString Named = Writer->GetMetaData(Gate);
		TestFalse(FString::Printf(TEXT("the writer arms %s"), *Gate.ToString()), Named.IsEmpty());
		TestNull(FString::Printf(TEXT("and %s deliberately names nothing"), *Gate.ToString()),
			FindObject<UFunction>(nullptr, *Named, EFindObjectFlags::ExactClass));
	}

	// The rows: internal, and not offered anywhere a user picks a type.
	for (const UScriptStruct* const Row : { FNDCVariableBinding::StaticStruct(), FNDCContextBinding::StaticStruct() })
	{
		TestFalse(FString::Printf(TEXT("%s is not offered as a Blueprint variable type"), *Row->GetName()),
			UEdGraphSchema_K2::IsAllowableBlueprintVariableType(Row));
		TestFalse(FString::Printf(TEXT("%s has no Make node"), *Row->GetName()),
			UK2Node_MakeStruct::CanBeMade(Row));
		TestTrue(FString::Printf(TEXT("%s is still usable on a pin, which is what internal use means"), *Row->GetName()),
			UEdGraphSchema_K2::IsAllowableBlueprintVariableType(Row, /*bForInternalUse*/ true));
	}

	// The enums: no internal-use lever exists for a UEnum, so the only answer is not being a BP type.
	for (const UEnum* const Enum : { StaticEnum<ENDCVariableType>(), StaticEnum<ENDCValueSource>() })
	{
		TestFalse(FString::Printf(TEXT("%s is not offered as a Blueprint variable type"), *Enum->GetName()),
			UEdGraphSchema_K2::IsAllowableBlueprintVariableType(Enum));
	}

	return true;
}

/**
 * Which bool a context row flips, and where that answer comes from.
 *
 * Two paths, and Niagara's own context exercises both: SystemToSpawn states its gate outright in an
 * EditCondition, while Location states nothing and is paired by the bOverride<Field> convention —
 * which is not decoration, since FNDCAccessContext::GetLocation goes on returning the owning
 * component's location until that flag is set.
 *
 * The rule underneath both is that the flag must itself be an input of the context, and the negative
 * case is a real engine struct rather than a fixture: FNiagaraDataChannelSearchParameters has exactly
 * the Location/bOverrideLocation pair by name, with neither marked NDCAccessContextInput. Nothing may
 * come of that, or a writer would flip bools on structs that never invited it.
 *
 * Editor-only, because all of this is read from metadata. A row stores the answer when it is created
 * or re-synced, so a cooked build never asks the question — which is why the function is not compiled
 * there at all rather than answering it a second, looser way.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderContextEnableFlagTest,
	"NDCBinder.Editor.ContextEnableFlag",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNDCBinderContextEnableFlagTest::RunTest(const FString& Parameters)
{
	auto Flag = [](const UScriptStruct* Struct, const TCHAR* FieldName)
	{
		return FNDCBinder::ResolveContextFieldEnableFlag(Struct, Struct->FindPropertyByName(FieldName));
	};

	const UScriptStruct* const Context = FNDCAccessContext::StaticStruct();
	TestEqual(TEXT("a field that states its gate gets the one it states"),
		Flag(Context, TEXT("SystemToSpawn")), FName(TEXT("bOverrideSystemToSpawn")));
	TestEqual(TEXT("a field that states nothing is paired by the bOverride convention"),
		Flag(Context, TEXT("Location")), FName(TEXT("bOverrideLocation")));
	TestEqual(TEXT("a gate is not itself a field with a gate"),
		Flag(Context, TEXT("bOverrideLocation")), FName(NAME_None));
	TestEqual(TEXT("and an input with no gate at all resolves to nothing"),
		Flag(Context, TEXT("bReturnExistingSystems")), FName(NAME_None));

	TestEqual(TEXT("the convention alone is not enough: the flag has to be an input of the context"),
		Flag(FNiagaraDataChannelSearchParameters::StaticStruct(), TEXT("Location")), FName(NAME_None));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
