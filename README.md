# NDC Binder

Configures a Niagara Data Channel write in the details panel rather than in a graph.

`FNDCBinder` is a struct you embed in a UObject. The details panel reads the assigned channel and
produces one row per input: channel variables under *Payload*, access context fields under *Access
Context*. Each row holds a constant, or a binding to a field of the event data, or a binding to a
function on the owning class. Binding uses the engine's property-binding menu, the one UMG and
StateTree use.

![The details panel](Docs/details-panel.jpg)

## Requirements

| | |
| --- | --- |
| Engine | Built and tested against Unreal Engine 5.8 |
| Plugin dependencies | Niagara, DataValidation |
| Module dependencies | Core, CoreUObject, Engine, Niagara |
| Project | Must compile C++: the plugin ships as source, with no prebuilt binaries |

The binder makes no assumption about its owner. Embed it in a GameplayCue, a Gameplay Ability, an
actor component or a plain actor.

## Setting one up in Blueprint

The project compiles the plugin, but authoring a binder needs no C++ of your own.

**1. Add a variable** of type **NDC Binder**.

**2. Assign the Data Channel** on the binder's header row. Rows appear immediately.

**3. Set Advanced → Event Data Type** to the struct you will pass to the write, such as
`GameplayCueParameters`. Skip this if your getters take no parameter.

**4. Fill the rows.** The chain icon opens the menu. Each row takes one of:

| Source | Cost at write time |
| --- | --- |
| a constant, typed into the row | nothing runs |
| a field of the event data | one member read |
| a function on this Blueprint | one reflected call |

**Create Binding** in the same menu writes a function with the right signature, binds the row to it
and opens it.

**5. Call Write With NDC Binder** where the effect should fire.

![The write node in a graph](Docs/blueprint-graph.jpg)

| Pin | |
| --- | --- |
| `Binder` | your binder variable |
| `Owner` | defaults to Self; bound functions are called on it |
| `Event Data` | your struct, connected directly. The pin is a wildcard. Leave it unconnected if no row reads event data |

**Write Many With NDC Binder** emits several elements in one write. Its `Event Data` is a wildcard
array, and one write of N is cheaper than N writes of one.

## From C++

```cpp
UPROPERTY(EditDefaultsOnly, Category = "Niagara Data Channel")
FNDCBinder NDCBinder;

AMyEffectActor::AMyEffectActor()
{
    // Optional: the struct bound functions take. Leave it unset and they take none.
    NDCBinder.InitEventDataType(FMyEventData::StaticStruct());
}

void AMyEffectActor::Fire(const FMyEventData& EventData)
{
    NDCBinder.WriteToChannel(GetWorld(), this, FConstStructView::Make(EventData));
}
```

Rows are authored in the panel either way. Declare getters `BlueprintNativeEvent` so Blueprint
children can override them.

## Binding rules

A bound function takes one of two shapes:

```cpp
T Func();
T Func(const FEventData& EventData);   // only when Event Data Type is set
```

It must be `const` or Pure, and must not be editor-only, replicated, or a delegate signature.

`T` must match the row's type. Nothing coerces, apart from a child struct sliced to its parent and a
soft object reference resolved.

Row types are the twelve Niagara's own Blueprint writer covers: bool, int32, float, vector2D, vector,
vector4, quat, linear color, position, enum, spawn info and id. Any other channel variable shows as
unsupported and is skipped.

Take event data **by const reference**. By value copies the struct on every call, and a mutable
reference is refused. **Create Binding** writes the correct signature; the graph editor cannot express
it by hand.

Event data fields may be **nested**, so a row can read `EffectContext.Origin`. A path stops at a
struct. A **static array** member cannot be bound at all: bind a getter returning the element you
mean.

## Access Context

Each input field of the channel's access context gets a row with a value editor and a bind button.
Bound getters are ordinary: `USceneComponent* GetWeaponComponent(FGameplayCueParameters)` drives
Owning Component, `FVector GetImpactPosition(FGameplayCueParameters)` drives Location.

Fields Niagara marks `Transient`, such as `Owning Component` and `Location`, show only a bind button.
They do not serialize, so there is no constant to author.

**Required**, on object-valued fields, lets a write decline itself:

| Answer | Result |
| --- | --- |
| non-null | the field takes it |
| null, Required | the whole write is skipped |
| null, not Required | the authored value stands |

**Gated fields keep their checkbox.** A write never ticks `bOverrideSystemToSpawn` for you. Bind a
field whose checkbox is clear and the row is marked to say the channel will ignore it.

With nothing bound here, the write uses the access context as authored. A spatially bucketed channel
needs a location either set or bound.

## When a row goes stale

Rows store names, so renaming a getter or removing a channel variable leaves a row pointing at
nothing. Five things notice:

| Where | What it does |
| --- | --- |
| the bind menu | never offers a function that breaks a rule |
| the row | marked in the panel with the reason |
| **the Blueprint compiler** | **fails the compile of the asset holding the row** |
| Validate Assets, the commandlet | fails the asset, so a build stops |
| the write | skips the row and logs once per owner class and row |

Stale rows are kept rather than removed, so putting a variable back revives its row. **Remove**, at
the top of *Payload*, clears out rows the channel has no place for. It appears only when there is
something to remove, and it is undoable.

A broken binding never turns a write off, even a required one: the row is skipped and the authored
value stands. Only a genuine null answer declines a write.

## What it costs

Binding costs less than the write it feeds. A row on an event data field is a member read; a row on a
function is a reflected call, which is why event data bindings are the shape to reach for. Over half
of a write on a simple channel is Niagara's own per-write path, which nothing here changes. Emitting
several elements in one write does.

The benchmark ships with the plugin. It runs as `NDCBinder.Performance.BindingOverhead` in the editor,
or from the command line in any configuration, Shipping included, with `-ndcbench=<report file>`.

## Modules

| Module | Type | Contents |
| --- | --- | --- |
| `NDCBinder` | Runtime | `FNDCBinder`, `FNDCVariableBinding`, `UNDCBinderLibrary` |
| `NDCBinderEditor` | Editor | details customization, compiler extension, asset validator |
| `NDCBinderUncooked` | UncookedOnly | the write node |
| `NDCBinderTests` | DeveloperTool | automation suite and benchmark |

## License

MIT — see [LICENSE](LICENSE).
