// K2Node_NDCWriteToDataChannel.cpp

#include "K2Node_NDCWriteToDataChannel.h"

#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraphSchema_K2.h"
#include "NDCBinderLibrary.h"
#include "StructUtils/InstancedStruct.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(K2Node_NDCWriteToDataChannel)

UK2Node_NDCWriteToDataChannel::UK2Node_NDCWriteToDataChannel(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Set here rather than when the node is placed, so a node loaded from an asset is already pointed
	// at its function before any pin is built from it.
	FunctionReference.SetExternalMember(
		GET_FUNCTION_NAME_CHECKED(UNDCBinderLibrary, WriteToDataChannel),
		UNDCBinderLibrary::StaticClass());
}

void UK2Node_NDCWriteToDataChannel::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
	// The plain call node for this function is kept out of the palette (BlueprintInternalUseOnly), so
	// this is the only way to place one and every placed node gets the behaviour below.
	const UClass* ActionKey = GetClass();
	if (ActionRegistrar.IsOpenForRegistration(ActionKey))
	{
		UBlueprintNodeSpawner* const Spawner = UBlueprintNodeSpawner::Create(GetClass());
		check(Spawner);
		ActionRegistrar.AddBlueprintAction(ActionKey, Spawner);
	}
}

void UK2Node_NDCWriteToDataChannel::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	// An unconnected wildcard is what the compiler refuses, and what this node has to allow: no event
	// data is a legitimate thing for a writer whose bound functions take no parameter. Giving the pin
	// a type here — rather than in the editor, where it would stop anything else from connecting —
	// leaves it an empty instanced struct, which the thunk already reads as "nothing was passed".
	if (UEdGraphPin* const EventDataPin = FindPin(TEXT("EventData"), EGPD_Input))
	{
		if (EventDataPin->LinkedTo.Num() == 0 && EventDataPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
		{
			EventDataPin->PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			EventDataPin->PinType.PinSubCategoryObject = TBaseStructure<FInstancedStruct>::Get();
			EventDataPin->PinType.ContainerType = EPinContainerType::None;
		}
	}

	Super::ExpandNode(CompilerContext, SourceGraph);
}
