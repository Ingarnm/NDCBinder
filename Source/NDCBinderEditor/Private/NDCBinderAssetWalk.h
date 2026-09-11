// NDCBinderAssetWalk.h
//
// Finding the writers an object carries values for. Shared by the two things that report a row that
// cannot run: UNDCBinderValidator (an asset, on Save and in the commandlet) and
// UNDCBinderCompilerExtension (a Blueprint, while it compiles). Neither knows where a writer is
// declared — that is the point of the plugin — so both find them by reflection.
//
// What the two callers do NOT share is which objects to look at, and it is not a detail: the
// validator reads the live class default object, while during a compile that object has been moved
// aside and only the pre-compile copy is safe to read. Each assembles its own set and calls in here
// per object.
//
// The component templates are the exception: they are the same three places on either side, reached
// through a different owner. That walk lives here too, taking the three as arguments, because the two
// copies of it were identical down to the comment and would have drifted the first time one of them
// learned about a fourth place.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "NDCBinder.h"
#include "UObject/UnrealType.h"

namespace NDCBinderAssetWalk
{
	/**
	 * The property path of the value being iterated, as "Outer.Inner".
	 *
	 * An array in a path is two properties of the same name — the array and its inner — and printing
	 * both reads as a nesting that is not there, so a repeat is dropped. What a path cannot carry is
	 * the index, so several writers in one array all name the array; the row named in the rest of the
	 * message is what tells them apart.
	 */
	inline FString DescribeValuePath(const FPropertyValueIterator& It)
	{
		TArray<const FProperty*> Chain;
		It.GetPropertyChain(Chain);

		FString Path;
		FString Last;
		// The chain arrives innermost first.
		for (int32 Index = Chain.Num() - 1; Index >= 0; --Index)
		{
			const FString Part = Chain[Index]->GetName();
			if (Part == Last)
			{
				continue;
			}
			Last = Part;
			Path += Path.IsEmpty() ? Part : TEXT(".") + Part;
		}
		return Path;
	}

	/**
	 * Every writer reachable from the object's properties, however deeply nested — a direct member,
	 * one inside another struct, one in an array. The body is handed the writer, its property path
	 * under Prefix, and the object it was found on.
	 *
	 * TPropertyValueIterator is the engine's own walk for exactly this. The hand-rolled TFieldIterator
	 * it replaces saw only direct members, so a writer one level down passed validation by never being
	 * looked at.
	 */
	template <typename BodyType>
	void ForEachWriterOn(UObject* Object, const FString& Prefix, BodyType&& Body)
	{
		if (!Object)
		{
			return;
		}
		for (TPropertyValueIterator<FStructProperty> It(Object->GetClass(), Object); It; ++It)
		{
			if (It.Key()->Struct != FNDCBinder::StaticStruct())
			{
				continue;
			}
			Body(*static_cast<const FNDCBinder*>(It.Value()),
				Prefix + DescribeValuePath(It), Object);
		}
	}

	/**
	 * Every writer on the component templates a Blueprint carries, from all three places it keeps them.
	 *
	 * A component added in the Blueprint editor keeps its authored values on a template rather than on
	 * the owning actor's defaults, so a writer configured there is invisible to a walk over those
	 * defaults. The three places are the construction script's nodes, the loose ComponentTemplates
	 * array, and the inheritable component handler — which holds a template of its own for each
	 * inherited component whose defaults this Blueprint overrides, carrying values the parent's
	 * template does not have.
	 *
	 * The arguments are the three, rather than the object holding them, because that object is exactly
	 * what the two callers disagree about: the validator reads them off the generated class, while a
	 * compile has to read them off the UBlueprint. Bindings on a template also resolve against the
	 * COMPONENT's class, which is why Body is handed the object each writer was found on.
	 */
	template <typename BodyType>
	void ForEachWriterOnComponentTemplates(
		const USimpleConstructionScript* SCS,
		TConstArrayView<TObjectPtr<UActorComponent>> ComponentTemplates,
		const UInheritableComponentHandler* Handler,
		BodyType&& Body)
	{
		auto VisitTemplate = [&Body](UActorComponent* Template)
		{
			if (Template)
			{
				ForEachWriterOn(Template, Template->GetName() + TEXT("."), Body);
			}
		};

		if (SCS)
		{
			for (const USCS_Node* Node : SCS->GetAllNodes())
			{
				VisitTemplate(Node ? Node->ComponentTemplate : nullptr);
			}
		}
		for (UActorComponent* Template : ComponentTemplates)
		{
			VisitTemplate(Template);
		}
		if (Handler)
		{
			TArray<UActorComponent*> Overrides;
			Handler->GetAllTemplates(Overrides);
			for (UActorComponent* Template : Overrides)
			{
				VisitTemplate(Template);
			}
		}
	}
}
