// NDCBinderEditorSettings.cpp

#include "NDCBinderEditorSettings.h"

#include "NiagaraDataChannelAccessContext.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(NDCBinderEditorSettings)

UNDCBinderEditorSettings::UNDCBinderEditorSettings()
{
	// The one rule the plugin ships with.
	//
	// bReturnExistingSystems is declared an input on FNDCAccessContext, but its only effect anywhere in
	// the engine is to append entries to SpawnedSystems (NiagaraDataChannel_Map.cpp, FindOrAddEntry) —
	// an OUTPUT. It changes nothing about the write itself, and WriteToChannel borrows the channel's
	// scratch context and discards it, so on the path this panel drives the flag is provably inert. It
	// matters only to a caller who runs ResolveAccessContext + WriteWithContext by hand and reads the
	// context back afterwards.
	//
	// Still a judgement call rather than a rule Niagara states: its metadata knows Input, Output and
	// Transient and nothing else, so there is no classification to read here. What would retire this
	// entry is Niagara marking such inputs, or this writer growing a way to surface SpawnedSystems from
	// WriteToChannel — at which point the flag stops being inert and the row should come back.
	//
	// Seeded here rather than in an ini so a fresh install of the plugin behaves the same with no
	// config at all; a project that wants the row back deletes the entry in Project Settings.
	FNDCHiddenContextField& ReturnExistingSystems = HiddenContextFields.AddDefaulted_GetRef();
	ReturnExistingSystems.ContextType = FNDCAccessContext::StaticStruct();
	ReturnExistingSystems.FieldName = TEXT("bReturnExistingSystems");
}

bool UNDCBinderEditorSettings::IsContextFieldHidden(const UScriptStruct* ContextType, const FProperty& Field) const
{
	if (!ContextType)
	{
		return false;
	}

	const FName FieldName = Field.GetFName();
	for (const FNDCHiddenContextField& Entry : HiddenContextFields)
	{
		// IsChildOf rather than equality: a rule is written against the type that DECLARES the field,
		// and every context derived from it inherits the field and the reason together. FNDCAccessContext
		// declares bReturnExistingSystems; the Map and GameplayBurst contexts derive from it.
		if (Entry.FieldName == FieldName && Entry.ContextType && ContextType->IsChildOf(Entry.ContextType))
		{
			return true;
		}
	}
	return false;
}
