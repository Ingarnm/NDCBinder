// NDCBinderLibrary.cpp

#include "NDCBinderLibrary.h"

#include "Blueprint/BlueprintExceptionInfo.h"
#include "Engine/World.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(NDCBinderLibrary)

#define LOCTEXT_NAMESPACE "NDCBinderLibrary"

FConstStructView UNDCBinderLibrary::UnwrapEventData(const UScriptStruct* Struct, const uint8* Address)
{
	if (!Struct || !Address)
	{
		return FConstStructView();
	}
	if (Struct == TBaseStructure<FInstancedStruct>::Get())
	{
		const FInstancedStruct& Boxed = *reinterpret_cast<const FInstancedStruct*>(Address);
		return FConstStructView(Boxed.GetScriptStruct(), Boxed.GetMemory());
	}
	return FConstStructView(Struct, Address);
}

bool UNDCBinderLibrary::WriteToDataChannel(const FNDCBinder& Binder, UObject* Owner, const int32& EventData)
{
	// Never reached: the node goes through execWriteToDataChannel, which is what reads the wildcard.
	// The declaration exists so the class does not have to be NoExport.
	checkNoEntry();
	return false;
}

DEFINE_FUNCTION(UNDCBinderLibrary::execWriteToDataChannel)
{
	P_GET_STRUCT_REF(FNDCBinder, Binder);
	P_GET_OBJECT(UObject, Owner);

	// The wildcard. Stepped as a bare FProperty so that connecting something that is not a struct
	// lands here as a clear message rather than as a misread stack.
	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.MostRecentPropertyContainer = nullptr;
	Stack.StepCompiledIn<FProperty>(nullptr);

	const FProperty* const EventDataProperty = Stack.MostRecentProperty;
	const uint8* const EventDataAddress = Stack.MostRecentPropertyAddress;

	P_FINISH;

	bool bWrote = false;
	P_NATIVE_BEGIN;
	{
		const FStructProperty* const EventDataStruct = CastField<FStructProperty>(EventDataProperty);
		if (EventDataProperty && !EventDataStruct)
		{
			// A connected pin of the wrong shape. Loud, because nothing else will ever say it: the pin
			// is a wildcard, so this cannot be caught when the graph is compiled.
			FBlueprintExceptionInfo ExceptionInfo(
				EBlueprintExceptionType::AbortExecution,
				//~ An array is worth naming on its own. An input wildcard accepts containers by default
				//~ (UK2Node::DoesInputWildcardPinAcceptArray), so plugging one in here is something the
				//~ graph editor allows without a word — and the author wanted the other node.
				CastField<FArrayProperty>(EventDataProperty)
					? FText(LOCTEXT("NDCBinderEventDataIsAnArray", "Write With NDC Binder writes one element and was handed an array. Use Write Many With NDC Binder, which writes one element per entry."))
					: FText::Format(
						LOCTEXT("NDCBinderEventDataNotAStruct", "Write With NDC Binder: Event Data must be a struct, but a {0} was connected."),
						FText::FromString(EventDataProperty->GetClass()->GetName())));
			FBlueprintCoreDelegates::ThrowScriptException(P_THIS, Stack, ExceptionInfo);
		}
		else
		{
			// An unconnected pin is an empty view, which is what a writer whose bound functions take no
			// parameter wants — the same thing C++ passes when it has no event data.
			UWorld* const World = Owner ? Owner->GetWorld() : nullptr;
			bWrote = Binder.WriteToChannel(World, Owner,
				UnwrapEventData(EventDataStruct ? EventDataStruct->Struct : nullptr, EventDataAddress));
		}
	}
	P_NATIVE_END;

	*static_cast<bool*>(RESULT_PARAM) = bWrote;
}

bool UNDCBinderLibrary::CollectEventDataViews(const FArrayProperty* ArrayProperty, const void* ArrayAddress, FNDCEventDataViews& OutViews)
{
	OutViews.Reset();
	if (!ArrayProperty || !ArrayAddress)
	{
		return false;
	}

	// The one thing a wildcard array can be connected wrong: elements that are not structs at all.
	const FStructProperty* const ElementStruct = CastField<FStructProperty>(ArrayProperty->Inner);
	if (!ElementStruct)
	{
		return false;
	}

	// FScriptArrayHelper wants a mutable address; nothing below writes through it, and the view it
	// produces is const.
	FScriptArrayHelper Helper(ArrayProperty, const_cast<void*>(ArrayAddress));
	OutViews.Reserve(Helper.Num());
	for (int32 Index = 0; Index < Helper.Num(); ++Index)
	{
		OutViews.Add(UnwrapEventData(ElementStruct->Struct, Helper.GetElementPtr(Index)));
	}
	return true;
}

bool UNDCBinderLibrary::WriteManyToDataChannel(const FNDCBinder& Binder, UObject* Owner, const TArray<int32>& EventData)
{
	// Never reached: the node goes through execWriteManyToDataChannel, which is what reads the array
	// off the stack. The declaration exists so the class does not have to be NoExport.
	checkNoEntry();
	return false;
}

DEFINE_FUNCTION(UNDCBinderLibrary::execWriteManyToDataChannel)
{
	P_GET_STRUCT_REF(FNDCBinder, Binder);
	P_GET_OBJECT(UObject, Owner);

	// The wildcard array, stepped the way every array node in the engine steps one.
	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.MostRecentPropertyContainer = nullptr;
	Stack.StepCompiledIn<FArrayProperty>(nullptr);
	const void* const ArrayAddress = Stack.MostRecentPropertyAddress;
	const FArrayProperty* const ArrayProperty = CastField<FArrayProperty>(Stack.MostRecentProperty);
	if (!ArrayProperty)
	{
		// The engine's own signal for "this node was handed something that is not an array"; it leaves
		// the VM in a state the caller can recover from, which throwing here would not.
		Stack.bArrayContextFailed = true;
		return;
	}

	P_FINISH;

	bool bWrote = false;
	P_NATIVE_BEGIN;
	{
		FNDCEventDataViews Views;
		if (!CollectEventDataViews(ArrayProperty, ArrayAddress, Views))
		{
			// Elements that are not structs. Loud, because a wildcard cannot be caught at compile time.
			FBlueprintExceptionInfo ExceptionInfo(
				EBlueprintExceptionType::AbortExecution,
				FText::Format(
					LOCTEXT("NDCBinderEventDataArrayNotStructs", "Write Many With NDC Binder: Event Data must be an array of structs, but an array of {0} was connected."),
					FText::FromString(ArrayProperty->Inner ? ArrayProperty->Inner->GetClass()->GetName() : TEXT("nothing"))));
			FBlueprintCoreDelegates::ThrowScriptException(P_THIS, Stack, ExceptionInfo);
		}
		else
		{
			UWorld* const World = Owner ? Owner->GetWorld() : nullptr;
			bWrote = Binder.WriteToChannel(World, Owner, Views);
		}
	}
	P_NATIVE_END;

	*static_cast<bool*>(RESULT_PARAM) = bWrote;
}

#undef LOCTEXT_NAMESPACE
