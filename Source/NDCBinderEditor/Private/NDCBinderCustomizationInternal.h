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

		//~ Both halves, for the same reason they travel together: two rows reading the same name from
		//~ different sources are not bound to the same thing.
		bool operator==(const FBoundTo& Other) const { return Source == Other.Source && Name == Other.Name; }
		bool operator!=(const FBoundTo& Other) const { return !(*this == Other); }
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

	/**
	 * The whole of what pasting into a payload row does, short of writing it: read the clipboard,
	 * refuse it when it is not a row, and hand the incoming row the target's identity back.
	 *
	 * Apart from the panel because the repair carries the invariant worth holding — VarName, Type and
	 * EnumDef are the channel's and must never arrive from a clipboard — and a context menu is not
	 * something a test can press.
	 */
	/**
	 * Which member holds a row's constant, for the type the channel gave it — none for the two types
	 * that have no constant. The value editor and the row's Copy both ask, so a row shows and copies
	 * the same field.
	 */
	FName ValuePropertyNameFor(ENDCVariableType Type);

	/**
	 * The clipboard text for a whole row — the other half of MakePastedRowText, and the reason both
	 * are here: the two have to be each other's inverse, and for a while they were not.
	 */
	FString MakeCopiedRowText(const FNDCVariableBinding& Row);

	bool MakePastedRowText(const FString& Clipboard, FName VarName, ENDCVariableType Type, UEnum* EnumDef, FString& OutText);

	/**
	 * The row a pasted VALUE should write, or false when the clipboard holds no value for it.
	 *
	 * Apart from the panel because it decides two things worth holding: that a pasted value unbinds
	 * the row, and that text which says nothing to the row's value member is not a paste at all.
	 */
	bool MakePastedValueText(const FNDCVariableBinding& Current, FName ValuePropName, const FString& Clipboard, FString& OutText);

	/**
	 * A binding on its own, as clipboard text, and back.
	 *
	 * Written in the SAME format a whole row copies as, so the plugin has one clipboard shape and not
	 * two that look alike — which also means a row copied anywhere can be pasted onto a context row
	 * that has nothing but a binding to receive.
	 */
	FString MakeBindingText(FBoundTo Bound);
	FBoundTo ParseBindingText(const FString& Text);

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
