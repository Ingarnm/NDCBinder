// NDCBinderLibrary.h
#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "StructUtils/InstancedStruct.h"
#include "NDCBinder.h"
#include "NDCBinderLibrary.generated.h"

/**
 * Blueprint access to FNDCBinder, so a Blueprint-only actor or component can own a writer and
 * drive it with no C++ class behind it.
 *
 * The C++ API takes event data as an FConstStructView — two pointers, no ownership — which carries no
 * reflection and so cannot be spelled as a Blueprint pin. The single-element node crosses that gap
 * without paying for it: its Event Data pin is a WILDCARD read straight off the VM stack, so the
 * write sees the graph's own struct at its own address, exactly as C++ does. The batched node reads
 * a wildcard ARRAY the same way, element by element, so it copies nothing either.
 *
 * There is deliberately nothing here for the access context. It is authored as a value in the
 * details panel, and the fields that vary per write are bound to ordinary pure getters returning
 * ordinary types — a USceneComponent*, an FVector — which need no node of their own.
 */
/**
 * The views a batched write is built from, one per array element.
 *
 * Inline, because the node they serve exists to stop paying per element: a batch measures 2.5x against
 * the same elements written one at a time, and handing it back a heap allocation per call spends part
 * of that on nothing. Thirty-two covers the batches a write is plausibly made of — the pellets of a
 * shot, the hits of a swing — and a longer one still works, on the heap, the way any TArray does once
 * it outgrows its inline room.
 */
using FNDCEventDataViews = TArray<FConstStructView, TInlineAllocator<32>>;

UCLASS()
class NDCBINDER_API UNDCBinderLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Writes one element to the writer's Data Channel: builds the access context from the authored
	 * default and the bound context rows, then writes every binding row.
	 *
	 * Event Data is the single argument bound functions receive, and its struct type must match the
	 * writer's Event Data Type — leave it unconnected when they take none. Owner is the object the
	 * bound functions are resolved on, normally Self. Returns false when nothing was written: no
	 * channel assigned, or a required context row answered null and declined the write.
	 *
	 * THE EVENT DATA PIN IS A WILDCARD, AND NOTHING IS COPIED INTO IT. The thunk reads the property and
	 * its address off the VM stack and hands the write a view of the graph's own value — the same thing
	 * C++ passes. Connect a cue's Parameters straight to it: boxing that into an FInstancedStruct first
	 * would deep copy the struct (for FGameplayCueParameters, two tag containers and a shared pointer)
	 * on every write, for a value nothing is allowed to change.
	 *
	 * An FInstancedStruct connected here is unwrapped rather than written as itself, so a payload that
	 * already arrived boxed — from a save, a data asset, another subsystem — still works and still
	 * copies nothing.
	 *
	 * What the wildcard cannot do is check the type: a writer's Event Data Type is data, not something
	 * a pin knows. A mismatch is the same runtime warning it has always been, just reached without a
	 * copy first.
	 */
	//~ Const, and by reference rather than by value: a Blueprint is handed the writer to write WITH,
	//~ never to change. A mutable reference would have granted the graph an edit it has no node to
	//~ make today — every configuration property is EditAnywhere and none is BlueprintVisible — but
	//~ the signature is what states the rule, and a signature that says "may modify" invites one.
	//~ Internal use only, and placed by UK2Node_NDCWriteToDataChannel instead: a plain call node leaves
	//~ an unconnected wildcard pin in the graph, which the compiler refuses outright, and "no event
	//~ data" has to stay sayable by connecting nothing. See that node.
	UFUNCTION(BlueprintCallable, CustomThunk, BlueprintInternalUseOnly, Category = "Niagara Data Channel|Binder",
		meta = (DefaultToSelf = "Owner", CustomStructureParam = "EventData", AutoCreateRefTerm = "EventData",
			DisplayName = "Write To Data Channel"))
	static bool WriteToDataChannel(const FNDCBinder& Writer, UObject* Owner, const int32& EventData);
	DECLARE_FUNCTION(execWriteToDataChannel);

	/**
	 * What the wildcard pin's value means as event data: itself, or — for an FInstancedStruct — what is
	 * inside it. Either way a VIEW, never a copy: the address is the graph's own value.
	 *
	 * Public because the thunk around it cannot be called from a test. A custom thunk reads its
	 * arguments off the VM stack, so only compiled Blueprint bytecode can feed it; this is the half
	 * with a decision in it, and it is testable on its own.
	 */
	static FConstStructView UnwrapEventData(const UScriptStruct* Struct, const uint8* Address);

	/**
	 * Writes ONE ELEMENT PER ENTRY in a single write — the pellets of one shot, the hits of one swing.
	 *
	 * Most of a write is paid per write rather than per element, so this is the cheap way to emit
	 * several at once: measured 2.5x against the same elements written one at a time. The rows are the
	 * same rows — each element's are evaluated against its own entry in the array — and what the whole
	 * write shares is the access context, built from the FIRST entry. Elements that do not agree about
	 * where the write goes and what it attaches to are separate writes, not a batch.
	 *
	 * An empty array writes nothing and returns false. See FNDCBinder::WriteToChannel.
	 *
	 * THE PIN IS A WILDCARD ARRAY, AND NOTHING IS COPIED. Connect the graph's own array of payloads —
	 * a TArray of any struct — and the thunk views each element where it already lives. An array of
	 * instanced structs works too and is unwrapped element by element, so a payload that arrived boxed
	 * still costs nothing extra here.
	 */
	//~ One call taking an array, rather than a scope handed between nodes: a scope owns a slice of
	//~ Niagara's buffer and the channel's write guard until it is destroyed, and a Blueprint graph that
	//~ branched away from its End node would hold both for the rest of the frame. One call cannot.
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "Niagara Data Channel|Binder",
		meta = (DefaultToSelf = "Owner", ArrayParm = "EventData", DisplayName = "Write Many To Data Channel"))
	static bool WriteManyToDataChannel(const FNDCBinder& Writer, UObject* Owner, const TArray<int32>& EventData);
	DECLARE_FUNCTION(execWriteManyToDataChannel);

	/**
	 * A view per element of an array, however the graph spelled it: the elements themselves, or what is
	 * inside them when they are instanced structs. Views, never copies — see UnwrapEventData.
	 *
	 * Public for the same reason: the thunk that reads the array off the VM stack cannot be called from
	 * a test, and this is the half that decides anything. Returns false when the array's elements are
	 * not structs at all, which is the one way a wildcard can be connected wrong.
	 */
	static bool CollectEventDataViews(const FArrayProperty* ArrayProperty, const void* ArrayAddress, FNDCEventDataViews& OutViews);
};
