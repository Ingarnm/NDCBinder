// NDCBinderValidation.cpp
//
// What a row that cannot run is worth saying about it, in one place. Two things in the editor module
// ask: UNDCBinderCompilerExtension, which turns an error into a FAILED BLUEPRINT COMPILE — the one
// report an author cannot walk past — and UNDCBinderValidator, which covers Save, the submit
// dialog and the DataValidation commandlet.
//
// It lives in the RUNTIME module, under WITH_EDITOR, because everything it needs is here: the rows,
// the rules that decide whether a function may be bound, the field lookup. The editor module owns
// when to ask and where to put the answer, not what the answer is.
//
// Deliberately NOT hung off UObject::IsDataValid, which is the engine's usual place for this: that
// is a virtual on the class that OWNS a writer, so every consumer would have to forward to it by
// hand and a consumer that forgot would lose the report without ever being told. Nothing is asked of
// a consumer here.
//
// Errors and warnings are different claims here, deliberately:
//
//  - an ERROR is DRIFT. A row names a function, an event data field or a context field that is gone
//    or no longer fits, so the row cannot run. Nothing at write time will say so — the value is
//    quietly skipped — and no author asked for that, so it is worth refusing to compile over.
//
//  - a WARNING is an UNFINISHED asset: no Data Channel picked yet, or a row set to read something
//    with nothing chosen. A Blueprint compiles on every edit, and half-authored is a state every
//    asset passes through on its way to being authored; failing the compile there would fight the
//    person doing the work.

#include "NDCBinder.h"

#if WITH_EDITOR

#include "Misc/DataValidation.h"
#include "NiagaraDataChannelAsset.h"

#define LOCTEXT_NAMESPACE "NDCBinderValidation"

namespace NDCBinderValidationPrivate
{
	/**
	 * The classes a context field will take, as a list to put in a message. Empty when it takes any.
	 *
	 * A field's declared type is not always what it wants — FNDCAccessContext::SystemToSpawn is a bare
	 * UObject narrowed by AllowedClasses metadata — and a binding refused for that reason has to say
	 * so, or the generic message below sends the author to the writer's Event Data Type instead.
	 */
	static FText DescribeFieldAllowedClasses(const FProperty* Field)
	{
		TArray<const UClass*> Allowed;
		FNDCBinder::GetFieldAllowedClasses(Field, Allowed);

		TArray<FText> Names;
		Names.Reserve(Allowed.Num());
		for (const UClass* Class : Allowed)
		{
			Names.Add(FText::FromString(Class->GetName()));
		}
		return FText::Join(LOCTEXT("AllowedClassSeparator", " or "), Names);
	}

	/**
	 * Where a bound path stops resolving, as a sentence.
	 *
	 * A path of one segment can only be missing, and saying so is the whole message. A longer one can
	 * break anywhere along it, and "has no field named 'Hit.ImpactPiont'" sends the author looking for
	 * a member with a dot in its name instead of at the segment that is actually wrong.
	 */
	static FText DescribeMissingFieldPath(const FNDCBinder& Writer, FName FieldPath)
	{
		const FText Declared = Writer.GetEventDataType()
			? FText::FromString(Writer.GetEventDataType()->GetName())
			: LOCTEXT("EventDataTypeNotSetForField", "not set");

		TArray<FString> Segments;
		FieldPath.ToString().ParseIntoArray(Segments, TEXT("."), /*InCullEmpty=*/ false);

		if (Segments.Num() > FNDCBinder::MaxFieldPathDepth)
		{
			return FText::Format(
				LOCTEXT("EventDataPathTooDeepFmt", "'{0}' goes {1} members deep and a bound path may go {2}"),
				FText::FromName(FieldPath), FText::AsNumber(Segments.Num()),
				FText::AsNumber(FNDCBinder::MaxFieldPathDepth));
		}

		// The longest prefix that still resolves: whatever comes after it is the segment that is wrong,
		// and the prefix is the struct the author should have been looking in.
		FString GoodPrefix;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			const FString Candidate = GoodPrefix.IsEmpty() ? Segments[Index] : GoodPrefix + TEXT(".") + Segments[Index];
			const FProperty* Leaf = nullptr;
			int32 Offset = 0;
			if (!FNDCBinder::ResolveFieldPath(Writer.GetEventDataType(), FName(*Candidate), Leaf, Offset))
			{
				if (GoodPrefix.IsEmpty())
				{
					// It fell over on the very first segment; the plain message below says it best.
					break;
				}
				return FText::Format(
					LOCTEXT("BrokenEventDataPathFmt", "the event data struct ({0}) has '{1}', but '{2}' is not a member of it"),
					Declared, FText::FromString(GoodPrefix), FText::FromString(Segments[Index]));
			}
			GoodPrefix = Candidate;
		}

		return FText::Format(
			LOCTEXT("MissingEventDataFieldFmt", "the event data struct ({0}) has no field named '{1}'"),
			Declared, FText::FromName(FieldPath));
	}

	/**
	 * What a bound event data field is worth right now, as a sentence, or empty when it is fine. Far
	 * less can go wrong than with a function — there is no class, no signature and no call — so there
	 * are only two answers: the field is gone, or its type no longer fits.
	 */
	static FText DescribeEventDataProblem(const FNDCBinder& Writer, FName FieldName, TFunctionRef<bool(const FProperty*)> Accepts, const FProperty* ContextField = nullptr)
	{
		const FProperty* Field = Writer.FindEventDataField(FieldName);
		if (!Field)
		{
			return DescribeMissingFieldPath(Writer, FieldName);
		}
		if (Accepts(Field))
		{
			return FText::GetEmpty();
		}
		if (const FText AllowedText = DescribeFieldAllowedClasses(ContextField); !AllowedText.IsEmpty()
			&& FNDCBinder::CanAssignPropertyToField(Field, ContextField))
		{
			// Assignable by type and refused by the field's own restriction: say which one, because
			// the message below reads as "wrong type" and the type is not what is wrong.
			return FText::Format(
				LOCTEXT("DisallowedEventDataClassFmt", "'{0}' is not one of the classes this field takes ({1})"),
				FText::FromName(FieldName), AllowedText);
		}
		return FText::Format(
			LOCTEXT("WrongEventDataFieldTypeFmt", "'{0}' is not of a type this row can take"),
			FText::FromName(FieldName));
	}

	/** What a bound name is worth on this class right now, as a sentence, or empty when it is fine. */
	static FText DescribeProblem(const FNDCBinder& Writer, const UClass* OwnerClass, FName FunctionName, TFunctionRef<bool(const UFunction*)> Accepts, const FProperty* ContextField = nullptr)
	{
		const UFunction* Func = OwnerClass->FindFunctionByName(FunctionName);
		if (!Func)
		{
			return FText::Format(LOCTEXT("MissingFunctionFmt", "no function named '{0}' exists on this class"), FText::FromName(FunctionName));
		}
		if (Accepts(Func))
		{
			return FText::GetEmpty();
		}

		// A flag rejection is about the function itself, not about this writer's Event Data Type, so
		// naming the declared struct here would point the author at the wrong property entirely.
		const ENDCBindingRejection Rejection = FNDCBinder::GetBindingRejection(Func);
		if (Rejection != ENDCBindingRejection::None)
		{
			return FText::Format(
				LOCTEXT("RejectedFunctionFmt", "'{0}' cannot be used as a binding: {1}"),
				FText::FromName(FunctionName), FText::FromString(FNDCBinder::DescribeRejection(Rejection)));
		}

		// Same reason as the branch above, for the other thing that is not the signature: a return
		// type the field refuses by class is not a signature problem, and naming the Event Data Type
		// would send the author to a property that is set correctly.
		if (const FText AllowedText = DescribeFieldAllowedClasses(ContextField); !AllowedText.IsEmpty()
			&& Writer.IsValidContextFieldFunction(Func, ContextField))
		{
			const FProperty* Return = Func->GetReturnProperty();
			const FObjectPropertyBase* ReturnObj = CastField<FObjectPropertyBase>(Return);
			return FText::Format(
				LOCTEXT("DisallowedReturnClassFmt", "'{0}' returns {1}, but this field only takes {2}"),
				FText::FromName(FunctionName),
				ReturnObj && ReturnObj->PropertyClass
					? FText::FromString(ReturnObj->PropertyClass->GetName())
					: LOCTEXT("NoReturnClass", "no object"),
				AllowedText);
		}

		const FText Declared = Writer.GetEventDataType()
			? FText::FromString(Writer.GetEventDataType()->GetName())
			: LOCTEXT("EventDataTypeNotSet", "not set");
		return FText::Format(
			LOCTEXT("WrongSignatureFmt", "'{0}' does not match what this writer accepts (Event Data Type is {1})"),
			FText::FromName(FunctionName), Declared);
	}
}

EDataValidationResult FNDCBinder::ValidateBindings(const UClass* OwnerClass, const FString& WriterName, FDataValidationContext& Context) const
{
	using namespace NDCBinderValidationPrivate;

	// Bound names are resolved against this class, so without one there is nothing to judge a row by
	// — not "everything is fine", which is why this is NotValidated rather than Valid.
	if (!OwnerClass)
	{
		return EDataValidationResult::NotValidated;
	}

	const FText Where = FText::FromString(WriterName);
	int32 NumErrors = 0;

	//~ The two report shapes. Each carries what the write does INSTEAD, because that consequence is
	//~ the difference the author is actually looking for: a payload row that cannot run leaves its
	//~ channel variable untouched, while a context row that cannot run leaves the authored value
	//~ standing, which looks like success.
	const FText SkipsValue = LOCTEXT("ConsequenceSkipped", "The value is skipped at write time.");
	const FText KeepsDefault = LOCTEXT("ConsequenceKept", "The field keeps its authored value at write time.");
	const FText WritesNothing = LOCTEXT("UnfinishedSkipped", "no value is written for it");
	const FText StaysAuthored = LOCTEXT("UnfinishedKept", "the field keeps its authored value");
	const FText ReadsFunction = LOCTEXT("ReadsFunction", "read a function");
	const FText ReadsEventData = LOCTEXT("ReadsEventData", "read event data");

	auto Error = [&Context, &NumErrors, &Where](const FText& What, const FText& Problem, const FText& Consequence)
	{
		if (Problem.IsEmpty())
		{
			return;
		}
		++NumErrors;
		Context.AddError(FText::Format(
			LOCTEXT("RowFailsFmt", "{0}: {1} cannot run — {2}. {3}"),
			Where, What, Problem, Consequence));
	};

	auto Unfinished = [&Context, &Where](const FText& What, const FText& Reads, const FText& Instead)
	{
		Context.AddWarning(FText::Format(
			LOCTEXT("RowUnfinishedFmt", "{0}: {1} is set to {2}, but nothing is picked yet, so {3}."),
			Where, What, Reads, Instead));
	};

	// Context rows first: a broken one is silent at write time, so this pass is the only thing that
	// ever says so. They can only be judged against the channel's access context type, so what to do
	// about not having one is the first question — and the two ways of not having one are not the
	// same claim. No channel at all is an asset still being authored; a channel that cannot produce a
	// context is a channel that changed under an asset which had one.
	const UScriptStruct* ContextType = GetContextType();
	if (!DataChannel)
	{
		Context.AddWarning(FText::Format(
			LOCTEXT("NoChannelFmt", "{0}: no Data Channel is assigned yet, so nothing is written at all and its access context rows cannot be checked."),
			Where));
	}
	else if (!ContextType)
	{
		++NumErrors;
		Context.AddError(FText::Format(
			LOCTEXT("ChannelWithoutContextFmt", "{0}: the assigned Data Channel ({1}) provides no access context type, so no row can be written."),
			Where, FText::FromString(DataChannel->GetPathName())));
	}

	if (ContextType)
	{
		for (const FNDCContextBinding& Binding : ContextBindings)
		{
			const FText What = FText::Format(
				LOCTEXT("ContextFieldWhatFmt", "access context field '{0}'"), FText::FromName(Binding.FieldName));

			const FProperty* Field = ContextType->FindPropertyByName(Binding.FieldName);
			if (!Field)
			{
				Error(What, LOCTEXT("ContextFieldGone", "this channel's access context has no such field"), KeepsDefault);
				continue;
			}
			if (Binding.Source == ENDCValueSource::EventData)
			{
				if (Binding.BoundEventDataField.IsNone())
				{
					Unfinished(What, ReadsEventData, StaysAuthored);
					continue;
				}
				Error(What, DescribeEventDataProblem(*this, Binding.BoundEventDataField,
					[Field](const FProperty* Source)
					{
						return FNDCBinder::CanAssignPropertyToField(Source, Field)
							&& FNDCBinder::IsSourceAllowedByFieldClasses(Source, Field);
					}, Field), KeepsDefault);
				continue;
			}
			if (Binding.Source != ENDCValueSource::Function)
			{
				continue;
			}
			if (Binding.BoundFunction.IsNone())
			{
				Unfinished(What, ReadsFunction, StaysAuthored);
				continue;
			}
			Error(What, DescribeProblem(*this, OwnerClass, Binding.BoundFunction,
				[this, Field](const UFunction* Func)
				{
					return IsValidContextFieldFunction(Func, Field)
						&& FNDCBinder::IsSourceAllowedByFieldClasses(Func ? Func->GetReturnProperty() : nullptr, Field);
				}, Field), KeepsDefault);
		}
	}

	for (const FNDCVariableBinding& Binding : Bindings)
	{
		const FText What = FText::Format(LOCTEXT("BindingWhatFmt", "binding '{0}'"), FText::FromName(Binding.VarName));

		// A channel stores an enum as an int, but every path that fills one carries a byte: Niagara's
		// own WriteEnum takes a uint8, this row's constant is one, and the return reader narrows to
		// one. An enumeration with an entry past 255 therefore writes that entry wrapped — and
		// nothing on the way in looks wrong, which is the only reason this is worth saying out loud.
		// Independent of Source: the constant picker offers those entries too. Counted the way that
		// picker counts, _MAX excluded, so the two agree on which entries are real.
		if (Binding.Type == ENDCVariableType::Enum && Binding.EnumDef)
		{
			int64 Widest = 0;
			for (int32 NameIndex = 0; NameIndex < Binding.EnumDef->NumEnums(); ++NameIndex)
			{
				if (Binding.EnumDef->GetNameStringByIndex(NameIndex).EndsWith(TEXT("_MAX")))
				{
					continue;
				}
				Widest = FMath::Max(Widest, Binding.EnumDef->GetValueByIndex(NameIndex));
			}
			if (Widest > (int64)MAX_uint8)
			{
				Context.AddWarning(FText::Format(
					LOCTEXT("EnumTooWideFmt",
						"{0}: {1} is typed by enumeration '{2}', whose entries reach {3}. An enum is carried as a byte here, as it is by Niagara's own WriteEnum, so an entry above 255 is written wrapped."),
					Where, What, FText::FromString(Binding.EnumDef->GetName()), FText::AsNumber(Widest)));
			}
		}

		if (Binding.Source == ENDCValueSource::EventData)
		{
			if (Binding.BoundEventDataField.IsNone())
			{
				Unfinished(What, ReadsEventData, WritesNothing);
				continue;
			}
			const ENDCVariableType ValueType = Binding.Type;
			const UEnum* ValueEnum = Binding.EnumDef;
			Error(What, DescribeEventDataProblem(*this, Binding.BoundEventDataField,
				[ValueType, ValueEnum](const FProperty* Source)
				{
					return FNDCBinder::IsPropertyCompatibleWithVariableType(Source, ValueType, ValueEnum);
				}), SkipsValue);
			continue;
		}
		if (Binding.Source != ENDCValueSource::Function)
		{
			continue;
		}
		if (Binding.BoundFunction.IsNone())
		{
			Unfinished(What, ReadsFunction, WritesNothing);
			continue;
		}
		Error(What, DescribeProblem(*this, OwnerClass, Binding.BoundFunction,
			[this, &Binding](const UFunction* Func) { return IsValidValueFunction(Func, Binding.Type, Binding.EnumDef); }), SkipsValue);
	}

	return NumErrors > 0 ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}

#undef LOCTEXT_NAMESPACE

#endif // WITH_EDITOR
