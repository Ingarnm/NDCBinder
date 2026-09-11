// NDCBinderCustomizationInternal.h
//
// The part of the details panel's innards that something other than the panel itself needs to reach:
// today that is NDCBinderCustomizationTests.cpp, and nothing else.
//
// It exists because of where those tests have to live. What they check sits in the customization's
// private namespace, and the module that owns the automation tests is a DeveloperTool one that an
// editor module must not depend on — so the tests live in this module, next to what they test, and
// this header is the seam between them rather than a second copy of anything.
//
// Everything here keeps its documentation at the DEFINITION, in the .cpp where the reasoning belongs.
// What is written here is only which of them cross a file boundary, and why each one is worth testing
// apart from the panel that uses it.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"
#include "NDCBinder.h"

class FMenuBuilder;
class UBlueprint;
class UEdGraph;
class UEnum;
class UScriptStruct;

namespace NDCBinderCustomizationPrivate
{
	/**
	 * What a row is bound to: the source and the name it goes with, always together.
	 *
	 * A row stores the two names apart so switching a row between a function and a field and back does
	 * not lose what it was bound to, which leaves the source enum as the only thing that says which
	 * name is live. Every question about a binding needs both halves, so they travel as one.
	 */
	struct FBoundTo
	{
		ENDCValueSource Source = ENDCValueSource::Constant;
		FName Name;

		bool IsBound() const { return Source != ENDCValueSource::Constant && !Name.IsNone(); }
		bool IsFunction() const { return Source == ENDCValueSource::Function && !Name.IsNone(); }
		bool IsEventData() const { return Source == ENDCValueSource::EventData && !Name.IsNone(); }
	};

	/** The bind menu's event data section. Tested by building it for real; see the test for why. */
	void AddEventDataEntries(
		FMenuBuilder& MenuBuilder,
		const FNDCBinder& Writer,
		const TFunction<bool(const FNDCBinder&, const FProperty*)>& AcceptsField,
		const TFunction<void(FBoundTo)>& OnPick);

	/** True when the settings say this panel draws no row for this field of this context type. */
	bool IsHiddenContextField(const UScriptStruct* ContextType, const FProperty& Field);

	/** How many functions on the class would bind if an Event Data Type were declared. */
	int32 CountFunctionsNeedingEventDataType(
		const FNDCBinder& Writer,
		const UClass* LookupClass,
		const TFunction<bool(const FNDCBinder&, const UFunction*)>& Accepts);

	/** The struct a writer's broken rows are all asking for, and how many of them ask. */
	UScriptStruct* FindExpectedEventDataType(const FNDCBinder& InWriter, const UClass* OwnerClass, int32& OutNumAffected);

	/** The return pin a generated getter needs for a row of this type. */
	FEdGraphPinType GetValueReturnPinType(ENDCVariableType Type, UEnum* EnumDef);

	/** The event data parameter pin a generated getter needs: a const reference, and why it must be. */
	FEdGraphPinType MakeEventDataPinType(UScriptStruct* EventDataType);

	/**
	 * Adds the function graph a binding needs — the signature and nothing else — and returns it.
	 *
	 * Reachable from here so the test can build one without a details panel to hang it on, which is
	 * what makes it a test of the real generator rather than of a second copy of it.
	 */
	UEdGraph* AddBindingFunctionGraph(
		UBlueprint* Blueprint,
		const FString& SuggestedName,
		const FEdGraphPinType& ReturnPinType,
		UScriptStruct* EventDataType,
		const FEdGraphPinType* EventDataPinTypeOverride = nullptr);
}
