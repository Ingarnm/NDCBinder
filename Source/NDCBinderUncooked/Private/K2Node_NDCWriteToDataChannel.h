// K2Node_NDCWriteToDataChannel.h
//
// The Write To Data Channel node, for the one thing a plain call node cannot do: leave its Event Data
// pin unconnected.
//
// The pin is a wildcard so that a graph can hand the write its own struct without boxing it. But a
// wildcard that nothing is connected to has no type, and FKismetCompilerContext::ValidateNoWildcardPinsInGraph
// refuses to compile a graph containing one — "the type of Event Data is undetermined". That is wrong
// for this node: a writer whose bound functions take no parameter has no event data to give it, and
// saying so by connecting something would be a ceremony with no meaning.
//
// So the pin keeps its wildcard type in the editor, where it is what lets anything connect, and
// becomes an empty instanced struct during expansion, which is how the write already spells "no event
// data". ExpandNode is early enough: ProcessOneFunctionGraph runs the expansion step long before
// PrecompileFunction looks for wildcards.

#pragma once

#include "CoreMinimal.h"
#include "K2Node_CallFunction.h"

#include "K2Node_NDCWriteToDataChannel.generated.h"

UCLASS()
class UK2Node_NDCWriteToDataChannel : public UK2Node_CallFunction
{
	GENERATED_BODY()

public:
	UK2Node_NDCWriteToDataChannel(const FObjectInitializer& ObjectInitializer);

	//~ UK2Node
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual void ExpandNode(class FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;
	//~ End UK2Node
};
