# NDC Binder

Binds a **Niagara Data Channel** write to data instead of a graph.

Embed one `FNDCBinder` in any object that pushes data to an NDC. The details panel reflects the
channel's own inputs into rows — its variables under *Payload*, its access context under *Access
Context* — and every row is either a constant you type in or a binding to a field of the event data or
a function on the owning class, through the same chain-icon binding widget UMG uses.

Both halves are bound the same way, which is where the name comes from: what a binder holds is
bindings. Where the values go is authored exactly like what the values are.

The plugin depends on Niagara and nothing else. It never names a gameplay framework of its own, so the
same binder works from a GameplayCue, a Gameplay Ability, a component or a plain actor.

## Quick start

```cpp
// MyEffectActor.h
UPROPERTY(EditDefaultsOnly, Category = "Niagara Data Channel")
FNDCBinder NDCBinder;

UFUNCTION(BlueprintNativeEvent, Category = "Niagara Data Channel")
FVector GetSpawnPosition(const FMyEventData& EventData) const;
```

```cpp
// MyEffectActor.cpp
AMyEffectActor::AMyEffectActor()
{
    // The struct bound functions take as their parameter. Optional — leave it unset and bound
    // functions take none at all.
    NDCBinder.InitEventDataType(FMyEventData::StaticStruct());

    // Optional native default payload. Rows are normally authored in the details panel and live on
    // the asset; declaring them here is for a native class that wants its children to inherit a
    // working set. The Init prefix is the point: these may only be called while the owning object is
    // being constructed, and an ensure says so if they are not.
    NDCBinder.InitBindings({
        FNDCVariableBinding::MakeFunctionBinding(
            TEXT("SpawnPosition"), ENDCVariableType::Position,
            GET_FUNCTION_NAME_CHECKED(AMyEffectActor, GetSpawnPosition)),
    });

    // The same, for a field of the access context — here, where the write is bucketed.
    NDCBinder.InitContextBindings({
        FNDCContextBinding::Make(
            GET_MEMBER_NAME_CHECKED(FNDCAccessContext, Location),
            GET_FUNCTION_NAME_CHECKED(AMyEffectActor, GetSpawnPosition)),
    });
}

void AMyEffectActor::Fire(const FMyEventData& EventData)
{
    // Everything a bound function needs travels in the one struct you declared. The writer has no
    // opinion of its own about who or what the write is "about".
    NDCBinder.WriteToChannel(GetWorld(), this, FConstStructView::Make(EventData));
}
```

Assign the Data Channel asset in the details panel; the binding rows appear as soon as you do.

## Bound functions

A bindable function returns the value for one channel variable and takes one of two shapes:

```cpp
T Func();
T Func(const FEventData& EventData);   // only when EventDataType is set
```

`T` must match the channel variable's type. Declare them `BlueprintNativeEvent` and Blueprint children
can override the default without touching C++. The bind menu only offers functions that actually match,
and in a multi-selection only those every selected class implements.

**Take the event data by const reference.** By value is accepted and works, but it is a deep copy of
the struct into the parameter block on every call — for `FGameplayCueParameters` that is two tag
containers and a shared pointer, per bound row, per write. A const reference is handed over as an
alias instead: `ProcessEvent` neither copies it back nor destroys it, so the call pays a memcpy of the
struct's own bytes and nothing else. Measured on a struct owning a single array, by value cost **+130 ns
per call** (×1.41); that difference is allocator work, so it does not shrink in a shipped build the way
the rest of a bound call does.

A **mutable** reference is refused outright — a binding is a read — which is why the shape is
`const FEventData&` or by value, and nothing in between.

In Blueprint this is not something the graph editor can express: a struct pin is by value, and its
*Pass-by-Reference* checkbox produces the mutable reference that is refused. So **Create Binding**
authors the pin with both flags set, and `NDCBinder.Editor.GeneratedBindingSignature` compiles a
function from it and asserts the parameter comes out a const reference. A getter written by hand — or
by a script driving the Blueprint API, which has no const flag either — gets the by-value shape unless
it inherits its signature from a native `BlueprintNativeEvent`.

### The binding must be a read

A binding is stored as a bare name and resolved by reflection at write time, so the rules are enforced
when the write happens, not only in the picker. A function is bindable only if it is:

* **`const` (C++) or Pure (Blueprint).** A cue notify runs on its CDO — `GameplayCueNotify_Burst` and
  `_Static` are not instanced — so a bound function that writes to a member would be mutating state
  shared by every use of that cue in the game, from inside something that reads like a getter.

  How much this enforces, honestly: a `const` C++ method is checked by the compiler, and a pure
  Blueprint graph has no exec pins so it cannot hold a Set node at all. But a **Blueprint override of
  a `const` native event inherits `FUNC_Const`** (it is part of `FUNC_FuncInherit`, and the Kismet
  compiler masks the override's flags from the overridden function) while its own graph does have exec
  pins, and nothing checks what it writes. So this rule keeps an obviously mutating function out of a
  binding; it is not a guarantee that a bound function cannot mutate.

  There are no exceptions to this rule. Every binding the writer has is a read — a payload value, a
  context field, the handler system — so the same gate governs all of them.
* **not editor-only.** Otherwise the binding works through all of editor testing and resolves to
  nothing once the game is cooked.
* **not replicated, and not a delegate signature.** Neither has a body that `ProcessEvent` will run
  the way a binding needs.

Everything else is fair game — `BlueprintNativeEvent`, `BlueprintImplementableEvent`, a plain
`UFUNCTION()`, static, inherited, or an interface function. **Create Binding** generates functions that
are pure and const, so anything the plugin makes for you already complies.

A function that breaks a rule is rejected in the bind menu, flagged on the row with the specific
reason, fails the compile of the Blueprint holding the row, is failed by **Validate Assets**, and is
skipped at write time with one log line naming the rule — so it never fails silently, and it never
fails differently in a packaged build than in the editor. See *When a row goes stale* below.

Two things can be bound this way, and they are the same kind of thing:

| Row | Returns | Purpose |
| --- | --- | --- |
| a payload row | the channel variable's type | one value of the payload |
| an access context row | the context field's type | one field of the access context |

### Or bind the event data directly

Either kind of row can read a **field of the event data struct** instead of calling anything. The bind
menu lists those fields above the functions, under the struct's own name.

Prefer it. There is no reflected call, no parameter block and nothing to destroy — just the member —
and most bound functions in practice are `return EventData.Something;`, which is boilerplate this
removes. A field binding needs no owner at all.

```cpp
// Instead of this:
FVector UMyCue::GetImpactPosition_Implementation(const FGameplayCueParameters& P) const { return P.Location; }

// bind the row to FGameplayCueParameters::Location and delete the function.
```

The type rule is the same one a function's return value goes through, so the two agree by
construction. What a field binding gives up is Blueprint overriding: a `BlueprintNativeEvent` getter
can be overridden in a child, a field read can only be rebound — which the panel supports on the row
itself.

A field may be **nested**. The menu opens a submenu for each struct member, so a row can read
`EffectContext.Origin` as easily as `Location`, up to four segments deep. That submenu is the engine's
own — the same `IPropertyAccessEditor` menu StateTree's bindings use — so it looks and behaves like
one, and it hides a struct that has nothing bindable under it.

Nesting costs nothing at write time. A path of struct members is a fixed sum of member offsets, so it
is added up once, when the row is first resolved, and the write is the same single addition a direct
member always was. That is also why a path stops at a struct: an object in the middle would have to be
loaded and null-checked on every write, and an array element would need an index the path does not
carry. Both of those are what a bound function is for.

It is deliberately the **event data** and not the owner's properties. A binding is only worth having
if it varies per write, and an owner's often cannot: a GameplayCueNotify executes on its CDO, so
reading one of its properties yields the class default — exactly what the row's constant already is.

### Access context rows

Every input field of the access context is a row under **Access Context**: the value editor Unreal
builds for its type, and a bind button. Author the value, or bind a getter for it — the choice is per
field, and the two sit on the same row so there is no second list to keep in step. The payload rows
sit apart under **Payload**, because one list says what is written and the other where it goes.

A few fields are deliberately not listed: outputs (the channel's answers back, `SpawnedSystems` and
friends), the `bOverride*` flags (drawn inline on the row they gate — see below), and fields the
write provably cannot act on.

That last list is a setting — **Project Settings → Plugins → NDC Binder** — and each entry names
a context type as well as a field, so a rule reaches the type that declares the field and everything
below it, and nothing else. It ships with one entry, `bReturnExistingSystems`, whose only effect
anywhere in the engine is on an output the write path discards. Hiding a field takes away its row and
not its value: the context type's own default still stands, and C++ can still set it.

Some fields have no value editor, only a bind button, and say `not set — bind to compute it per write`.
Those are the ones Niagara marks `Transient` — `Owning Component`, `Location` — which are not
serialized, so there is no constant to author. Computing them per write is the only thing that was
ever possible for them.

A bound field's getter is an ordinary one. `USceneComponent* GetWeaponComponent(FGameplayCueParameters)`
drives Owning Component; `FVector GetImpactPosition(FGameplayCueParameters)` drives Location. No node
and no context type is involved, which is why the same getter usually already exists for the payload.

Nothing coerces: a wrong return type is a wrong binding, not a value quietly converted on the way in.
The two conversions that cannot hide a mistake are allowed — a child struct sliced to its parent
(`FNiagaraPosition` → `FVector`) and a soft object reference resolved.

**Required** (object-valued fields only) is how a write declines itself:

* answer non-null → the field takes it;
* answer null, **Required** → the whole write is skipped — "there is nothing to attach to here";
* answer null, not Required → the authored value stands, untouched.

That last line is what makes a per-surface system getter work: an unmapped surface returns null and
the panel's own System To Spawn is what goes out.

### Gated fields and their checkboxes

Some context fields do nothing until a companion bool says to use them: `System To Spawn` needs
`bOverrideSystemToSpawn`, `Cell Size Override` needs `bOverrideCellSize`, `Location` needs
`bOverrideLocation`. Unreal draws that bool as the **checkbox to the left of the field's name**, so
those flags get no row of their own.

**The checkbox decides, bound or not.** A write never ticks it for you. Bind a field whose checkbox is
clear and the field is written and then ignored by the channel — so the panel puts a warning marker on
the row saying exactly that, rather than quietly repairing it behind your back. The switch stays where
you can see it and means what it says.

The one exception is a flag Niagara marks `Transient`. It does not serialize, so there is no checkbox
and no way to author it — `bOverrideLocation` is the case in practice. There the write sets it, because
nothing else can, and the field it gates has no checkbox either, so nothing on screen misstates it.

Niagara states the pairing two different ways, so both are read: `EditCondition` metadata where it has
it (`System To Spawn`, `Cell Size Override`, `System Bounds Padding`), and the `bOverride<Field>` naming
convention where it does not — the only thing linking `Location` to `bOverrideLocation`. That pair
matters: `FNDCAccessContext::GetLocation()` returns the owning component's location instead whenever
the flag is clear.

The flag is left alone on the null-answer path above, so declining to answer still falls back to
exactly what the panel shows.

Rows are **sparse**: a field only has one once something is bound to it, and unbinding removes it
again. An unbound field costs nothing at write time, not even a skipped iteration.

With nothing bound, the write goes out on the default access context exactly as authored. The writer
cannot read a position out of a struct it knows nothing about and will not invent one, so a channel
that buckets spatially (Islands, Map) either needs a location set in the panel or a row bound to it.

## Blueprint

The writer works from a Blueprint-only actor or component, with no C++ class behind it.

1. Add a variable of type **NDC Binder**. Its details panel is the same one C++ owners get.
2. Assign the **Data Channel**. Binding rows appear immediately.
3. If your bound functions take event data, set **Advanced → Event Data Type** to that struct. This is
   the one thing a C++ owner does in its constructor and a Blueprint has to declare here instead; the
   bind menu says so explicitly when it is missing.
4. Write the functions as ordinary Blueprint functions — `T Func()` or `T Func(<your struct> EventData)`
   — then bind the rows to them.
5. Call **Write With NDC Binder**. `Owner` defaults to Self; connect your struct straight into
   `Event Data` — the pin is a wildcard and reads the graph's own struct where it sits, with no
   **Make Instanced Struct** in between. To push several elements in one write, use **Write Many To
   Data Channel**, whose `Event Data` is a wildcard array.

Placement is authored the same way, under **Access Context**. Each input field of the channel's own
context — Location, Owning Component, System To Spawn — is a row with its own bind button, so a
Blueprint drives placement with the same kind of pure getter it uses for the payload: a
`USceneComponent* GetMuzzle()`, an `FVector GetImpactPoint()`. There is no node to place and no
context value to build, which is the point: see *Access context rows* above for what is listed, what
**Required** does, and how the `bOverride` checkboxes behave.

Leave `Event Data` unconnected and it settles as an `FInstancedStruct` holding nothing — the shape a
writer with only constant and function rows wants. See *Notes* for why the pin is a wildcard rather
than an `FInstancedStruct`, and what that saves.

### What a graph may build

Owning a writer is supported; assembling one is not. A writer is configuration — rows authored in a
details panel and read at write time — so the variable above is the whole of what a Blueprint creates,
and the plugin closes the rest off in the engine's own menus:

| | Variable / pin | Make & Break | Split pin |
| --- | --- | --- | --- |
| `FNDCBinder` | yes | no | no |
| `FNDCVariableBinding`, `FNDCContextBinding` | pins only | no | no |
| `ENDCVariableType`, `ENDCValueSource` | no | — | — |

Three different specifiers, because the engine gates the three cases in three different places; the
header says which and why, above `ENDCVariableType`. All of it gates editor menus rather than the
runtime, so a node someone placed before still compiles.

## When a row goes stale

A row whose variable the channel no longer has is kept, shown greyed, and skipped at write time —
put the variable back and the binding works again. When you are sure it is not coming back, **Remove**
at the top of *Payload* takes out every row this channel has no place for, access context rows
included. It appears only when there is something to remove, and it is undoable.

A row stores a bare name — a function, an event data field, a context field — so it outlives whatever
it names. Rename a getter in C++, drop a member from the event data struct, change the channel's
access context, and the row is still there naming something that is gone. The write path does not
fail loudly for that: the row is skipped and the effect plays slightly wrong.

Five things report it, and one of them stops work:

| Where | What it does |
| --- | --- |
| the bind menu | never offers a function that breaks a rule |
| the row | marked in the details panel with the specific reason |
| **the Blueprint compiler** | **an unresolvable row fails the compile of the asset holding it** |
| Validate Assets, the DataValidation commandlet | fails the asset, so a build stops on it |
| the write | skips the row, logging once per owner class and row |

The compile failure asks nothing of the class that owns a writer. The plugin registers one Blueprint
compiler extension, for every Blueprint type there is, and it reports through the same
`FNDCBinder::ValidateBindings` the asset validator uses. Put a writer on any class — C++ or
Blueprint-only — and a stale row in an asset carrying it stops compiling: no override, no forwarding,
nothing to remember.

`UObject::IsDataValid` is the engine's usual home for this, and the compiler runs it too, but it is a
virtual on the *owner*: every consumer would have to forward to the writer by hand, and one that
forgot would lose the report without being told. You can still call `ValidateBindings` from your own
`IsDataValid` if you want the check somewhere else as well — but not for the compile, or every row
gets reported twice.

**An error and a warning are different claims.** An error is drift: a row names something that is
gone or no longer fits, which nobody asked for, so it fails the compile. A warning is an unfinished
asset — no Data Channel assigned yet, a row set to read a function with no function picked — and that
is a state every asset passes through while it is being authored, with a Blueprint compiling on every
edit along the way.

Two limits. A compile **on load** is skipped on purpose: dependencies are still coming in, so a
binding to a function declared on a parent Blueprint can read as missing when it is merely not
compiled yet — the engine leaves its own validation out of load for the same reason. And a data-only
Blueprint is not fully compiled on load at all. So a C++ rename surfaces when something actually
compiles the asset — opening it, or **Compile All Blueprints** — while Validate Assets and the
commandlet are what catch it in a build where nobody opened anything.

## Supported variable types

One entry per `UNiagaraDataChannelWriter::Write*` overload: bool, int32, float, vector2D, vector,
vector4, quat, linear color, position, enum, spawn info and id. `SpawnInfo` and `ID` have no constant
editor and are writable from a bound function only; a row left unbound is skipped so the channel
default stands. A variable of any other type shows as unsupported and is skipped.

That list is shorter than what a channel can hold, and the ceiling is the engine's, not this
plugin's. A channel accepts almost any type — `FNiagaraDataChannelVariable::IsAllowedType` is a deny
list, refusing only data interfaces, object types, parameter maps, generic numerics, halves and
matrices — but the only way into a game-data buffer is `FNiagaraDataChannelVariableBuffer::Write<T>`,
a template that asserts `sizeof(T)` against the variable's recorded size and assigns through a typed
pointer. There is no per-element byte-wise entry point: the buffers come out of
`FNiagaraDataChannelGameData` as a const view, and the one raw path, `SetFromSimCache`, replaces a
whole variable rather than one element. A writer can therefore only fill types it names at compile
time, which is exactly why Niagara's own `UNiagaraDataChannelWriter` stops at the same twelve.

A **static array** member cannot be bound at all — neither as a row's source nor as a step in a path.
`FVector Corners[2]` is a single vector property to every type test, and both the offset a path
resolves to and the value a store reads would be element zero: an answer the author did not ask for,
on a row that looks exactly like a correct one. Bind a getter returning the element you mean.

An **enum** row is bound by its own enum: a getter or event data member of a *different* enumeration is
refused, since the two would agree on an integer and disagree on everything else, and the channel — which
stores an int — could not tell them apart afterwards. A plain `uint8` is still accepted, deliberately: an
author returning a raw index is saying which index they mean. (A context field is stricter and refuses the
raw byte, because there the destination is a typed property rather than an int.) The channel stores an
int, but the value is carried as a byte the whole way down, `WriteEnum` included, so an enumeration with
entries past 255 writes those entries wrapped — validation warns about a row typed that wide rather than
failing it, since the entries that fit keep working.

## The default access context

**NDC → Access Context** in the details panel is a real access context of the channel's own type,
edited as ordinary rows. It is the starting point for every write: the writer copies it, applies the
rows bound to its fields, and hands it to the channel.

Its type follows the Data Channel asset, so there is no struct picker to get wrong. Pick a different
channel and the panel retypes itself on the next sync.

Only the fields Niagara marks as **inputs** appear. The rest are the channel's answers back to the
caller — `SpawnedSystems` and friends — and writing them would be talking over it.

**System To Spawn** lives here, like every other field: author it for a fixed handler system, or bind
it for one picked per write. There is no separate override property, because there is nothing a
separate one could say that a row on this field cannot.


## What it costs

Measured in a **Shipping** build, against the same work written by hand. Numbers are ns/op on one
desktop machine — read the ratios and the order of magnitude, not the digits. Every case is timed
four times over and the lowest kept, inside the harness rather than by whoever runs it: noise on a
desktop only ever adds, so the fastest run is the one least interrupted.

| per row | hand-written | bound to an event data field | bound to a function |
| --- | --- | --- | --- |
| | 2.3 ns | 12.8 ns | 72.0 ns |

| one write, five rows | hand-written | all rows on event data | all rows on functions |
| --- | --- | --- | --- |
| | 29.5 ns | 70.1 ns (×2.4) | 366.5 ns (×12.4) |

The second table is **arithmetic, not a measurement**: five times the per-row figure plus the context
seed. A measured write is in *What a whole write costs* below, and it is a different shape of answer.

These cover the reflection this plugin adds — resolving a binding and getting a value out of it. What
it costs to put that value into the channel is a separate figure, and a larger one; see below.

A five-row write costs **0.002% of a 60 Hz frame**. It takes 475 of them in a single frame to reach
1% — more than 28,000 writes a second. The multiplier is large only because the thing it multiplies
is a direct call the compiler inlines to nothing.

What a bound row does *not* do any more is look itself up. A row names its field, its event data
member and its gate flag by FName, and `UStruct::FindPropertyByName` walks the struct's property list
comparing names; a gated row bound to event data was doing three of those walks per write, plus the
property-kind chain that decides how to store the value. None of those answers can differ between two
writes, so `FNDCFieldCache` resolves them on the first one and the rest read a pointer. That is the
same move Epic makes in `FPropertyBindingBindingCollection`, which resolves a binding once into an
`FPropertyBindingCopyInfo` and copies through that rather than through the path — none of which is
reachable from outside it, but the idea is. Measured against the same four-run floor, it took an event
data row from 17.4 ns to 12.1 and a function row from 84.4 to 72.7 when it landed, with every unbound
control in the benchmark unchanged to within 0.2 ns.

### What a whole write costs

Everything above measures what this plugin adds *over* writing the same thing by hand, which is the
right question about the abstraction and the wrong one about a frame: it says nothing about what a
write costs. The rest of a write is `FNDCWriterBase::BeginWrite` — a world lookup, the Niagara world
manager, the channel's handler, `FindData`, `GetGameDataForWriteGT` and a `SetNum` across every
variable buffer — and it is not this plugin's code, which is exactly why it was left out and why
nobody could say whether the nanoseconds above mattered.

They are smaller than it. One real `WriteToChannel` on a five-variable channel, five rows, in a
**Development Editor build** (see the caveat under *Measure in a Shipping build*):

| one write, five rows | all rows on event data | all rows on functions |
| --- | --- | --- |
| whole `WriteToChannel` | 352.1 ns | 1736.2 ns |
| of which: open + close, no rows | 192.1 ns (55%) | 192.1 ns (11%) |
| of which: access context | 20.4 ns (6%) | 20.4 ns (1%) |
| of which: the five rows | 139.6 ns (40%) | ~1523 ns (88%) |

So for a cue whose rows read event data, **over half of a write is Niagara's own per-write path** and
nothing in this plugin can make it cheaper. For a cue whose rows call functions, the rows are nine
tenths of it — which is the case for calling functions only when a value cannot come from event data,
stated as a measurement rather than a preference.

The Global channel used here is the cheapest kind, so ~190 ns is a **floor** for Niagara's share: a
GameplayBurst channel resolves an attachment and a spatial bucket inside `FindData` on top of it.

**One write of N beats N writes of one**, because that floor is paid per write and not per element:

| eight elements | ns | |
| --- | --- | --- |
| eight writes of one | 2854.4 ns | |
| one `WriteToChannel` taking all eight | 1092.0 ns | ×2.61 |
| the same by driving the scope by hand | 1171.4 ns | ×2.44 |

The call is the faster of the two on purpose: `WriteToChannel` asks its per-write questions once and
then writes the rows unchecked, where a hand-driven scope goes through the public `WriteBindings` and
re-asks them for every element.

That is what the batched form is for — a shotgun's pellets, a chain of hits, anything a caller has
all of at once:

```cpp
Writer.WriteToChannel(World, this, Payloads);   // TConstArrayView<FConstStructView>
```

and from a graph, **Write Many With NDC Binder**, taking an array of instanced structs. The rows do
not change: each element's are evaluated against its own payload, so a row reading an event data
field or calling a getter gets that element's value. What the batch shares is the access context,
built once from the first element — which also means the **context rows run once for the whole
batch**, a saving on top of the 2.5× above.

So a batch is a set of elements that agree about where the write goes and what it attaches to.
Elements that disagree are separate writes: Niagara takes one context per write, and a required
context row answering null declines all of them together.

`BeginWrite` + `WriteBindings` is the same thing one level down, for a caller that wants the scope
itself — to interleave its own per-element values, or to keep what the channel writes back.

### Where a row actually goes

Writing a row by name — every `Write*` on `UNiagaraDataChannelWriter`, and so every write this plugin
used to make — ends in `FNiagaraDataChannelGameData::FindVariableBuffer`, which walks the channel's
variables comparing an `FName` and a type, per row, per write. That cost is not in the table above at
all, and it is larger than everything in it:

| one row's store | 5-variable channel | 20-variable channel |
| --- | --- | --- |
| by name | 53.9 ns | 175.9 ns |
| by buffer index | 2.3 ns | 2.3 ns |

So the writer does not write by name. Each row is resolved to its buffer index once per channel
layout — the same question `FindVariableBuffer` answers, asked once instead of per write, keeping its
concession that a channel may declare an enum where the writer sends an int — and every write after
that is the store alone. Niagara hands out a new layout when a channel's variables change, so
comparing the layout pointer is the whole invalidation.

Owning the destination buffer is what makes that possible, and `UNiagaraDataChannelWriter` keeps its
buffer private. `FNDCWriteScope` derives from Niagara's `FNDCWriterBase` instead, whose
`BeginWrite` has an overload taking an `FNDCAccessContextInst` — so nothing about the access context
model changes, and the scope holds both the buffer and the channel's write guard until it ends.

### Against the Blueprint way of doing it

The real alternative is a Blueprint graph — `Make NDC Access Context` into `Write Data Channel`, as
`/Game/NiagaraExamples/Blueprints/TriggerImpact` does it. The make node expands to one
`SetSinglePropertyInNDCAccessContextInstance` per exposed pin — a `FindPropertyByName` and a generic
copy, uncached, per write — and the write node "is just a placeholder and calls into
`CreateDataChannelWriter_WithContext` and its individual write functions from the BP node"
(`NiagaraDataChannelFunctionLibrary.cpp`), one `Write*` per payload pin.

Those `Write*` calls are by name. Both sides of that, measured end to end against Epic's own example —
`TriggerImpact`'s `ImpactNDC` function, whose write node carries eight payload pins (Position, Normal,
HitVelocity, HitDirection, SurfaceType, Distance, IsLocal, SpawnCountScale) — writing the same eight
values to the same channel in the same world:

| one write, eight payload values | Blueprint nodes | this writer |
| --- | --- | --- |
| **whole write, native half** | **971.2 ns** | **325.1 ns** (×3.0) |

What each side is doing for that: theirs constructs a fresh access context and sets each exposed input
on it by name, then makes a writer and calls one `Write*` per pin, each of which finds its buffer by
name. Ours seeds the authored context with one struct copy and stores eight values at buffer indices
resolved once for the channel's layout. Both pay Niagara's identical per-write path in the middle
(~190 ns of it, which is most of what is left on our side).

Two things make that an *understatement* of the gap. The context here had three exposed inputs rather
than the example's five, so their side was charged two property sets fewer. And the number is native
code only: a Blueprint graph also pays the VM to dispatch every one of those calls — about fifteen for
this write — and to assemble their arguments, none of which is counted. Nothing uncounted is on our
side.

An earlier version of this section added up the same comparison from separate per-piece numbers and
got ×2.2; measuring the whole write instead gives ×3. Also worth recording because it was the
assumption worth testing: the payload store does **not** cancel out between the two, which is where
most of the difference lives.

Two honest qualifications. The per-row figure above is measured on a context row and used as the
proxy for a payload row — they do comparable work, but it is a proxy. And a row bound to a *function*
costs 72.0 ns for the `ProcessEvent` alone, which a one-node Blueprint expression would beat; that is
why event data rows exist and why they are the shape to reach for.

**Measure in a Shipping build or not at all.** The same benchmark reports around 300 ns for a
function row in a Development Editor build and 73 ns in Shipping — four times the truth. Two things cause the
gap: `FNDCBoundFunctionCache` re-resolves the function by name inside an `ensureMsgf` on every cache
hit under `WITH_EDITOR`, and `ProcessEvent` carries its script call-stack tracking until
`DO_BLUEPRINT_GUARD` goes away in Shipping and Test.

**What is not in these numbers**, because both implementations pay it identically: the bodies of the
bound getters, and the handler lookup that finds where a write goes.

**Re-running it.** The benchmark is a plain function, not an automation test, because a Shipping build
has no automation framework, no console and — on an installed engine — no logging to report through.
It writes to a file instead, so the same cases run in any configuration and the results compare:

```bash
LyraGame-Win64-Shipping.exe <Project>.uproject -ndcbench="C:/path/report.txt" -unattended -nullrhi
```

In the editor it is also exposed as `NDCBinder.Performance.BindingOverhead`, and there the report
covers the whole-write cases too: they need a world, and the automation test hands it `GWorld`. On the
command line the run waits for the first ticking game world for the same reason.

**What it takes to get a non-editor number in this project**, learned the slow way, because three of
the four obvious routes are closed:

* the module this lives in is a `DeveloperTool`, which UBT excludes from Shipping — and since these
  targets build in a *shared* build environment against an installed engine, no target setting can put
  it back. Temporarily setting the module's type to `Runtime` is the way in;
* the `Test` configuration cannot be built at all with an installed engine distribution;
* a non-editor build of this project loads its content from a Zen store, so `LyraGame.exe` exits at
  startup without one — a cook, or the store running, is a precondition for any of the above.

Hence the whole-write figures above are Development Editor ones, labelled as such. What that inflates
is narrow and known: the `ensureMsgf` re-lookup on every bound-function cache hit (17 ns per row, and
absent outside the editor) and `ProcessEvent`'s script call-stack tracking. Neither touches the
Niagara path or the event-data rows, so the shape of the answer — where a write spends its time —
holds; the function-row column is the one that shrinks.

## Notes

- Payload rows are synced from the channel asset and are never destroyed behind your back: remove a
  variable from the channel and its row is kept, marked stale, and skipped at write time. Only picking
  a *different* channel prunes rows, since their values would be meaningless for it. Context rows
  follow the same policy, without the sync: they are sparse, so there is no set to reconcile.
- A context row that cannot run — no such field, a function that is missing or unusable — is skipped
  with one log line and the authored value stands. A broken binding deliberately **cannot** turn a
  write off, even a required one: a typo in a function name would otherwise silently stop an effect
  from ever playing and read as a content bug. Only a real null answer declines a write.
- `WriteToChannel` takes either one payload or an array of them; the array form emits one element per
  entry in a single write, which is 2.5× cheaper than that many single writes because Niagara's
  per-write path and this writer's context pass are then paid once. Blueprint gets the same through
  **Write Many With NDC Binder**. `BeginWrite` + `WriteBindings` is the same thing with the scope left
  in the caller's hands.
- The Blueprint node's **Event Data pin is a wildcard, and nothing is copied into it**: the thunk reads
  the property and its address off the VM stack, so the write sees the graph's own struct — connect a
  cue's Parameters straight to it. Boxing that into an `FInstancedStruct` first would deep copy the
  struct on every write; an `FInstancedStruct` connected there anyway is unwrapped, so a payload that
  arrived boxed still costs nothing extra. **Write Many With NDC Binder** takes a wildcard *array* the
  same way — the graph's own `TArray` of any struct, viewed element by element — so the 2.5× a batch
  is worth arrives from a graph whole, rather than paying a box per element on the way in.
- The access context is two-way: the channel writes back the handler systems a write spawned or
  joined (`SpawnedSystems`), and whether they were attached. `WriteToChannel` does not surface them —
  call `ResolveAccessContext` and `WriteWithContext` yourself and read the context afterwards. Because
  that is the only way to see them, `bReturnExistingSystems` — an input whose sole effect is to add
  entries to `SpawnedSystems` — is not offered as a row; it would change nothing a `WriteToChannel`
  caller could observe. That exclusion is by name, since Niagara's metadata knows only Input, Output
  and Transient and cannot express "input that configures an output".
- Bound functions are looked up and signature-checked once per owning class, not per write. The
  entry is keyed on every input to the answer, and it also re-checks that the remembered function is
  still owned by that class — which is what catches a Blueprint recompile, since that cleans the
  class in place rather than making a new one.
- Writes are game-thread only, and a write cannot be re-entered: if a bound function starts another
  write to the same channel, the nested one is skipped rather than corrupting the outer one, because
  Niagara hands out a single shared writer per channel.
- A write whose owner is already pending kill is skipped with a log line, rather than calling into it
  and quietly writing every bound row's default — which is what `ProcessEvent` would do on its own.
- Misconfiguration (no channel, missing function, wrong signature, mismatched event data) is reported
  once per owner class and binding under the `LogNDCBinder` category, not every frame. That record can
  be emptied (`ResetBindingWarnings`), which is what keeps the tests asserting on those lines from
  passing once and failing on the next run in the same process.

## Modules

| Module | Type | Contents |
| --- | --- | --- |
| `NDCBinder` | Runtime | `FNDCBinder`, `FNDCVariableBinding`, `UNDCBinderLibrary` |
| `NDCBinderEditor` | Editor | the details customization, the compiler extension, the asset validator |
| `NDCBinderUncooked` | UncookedOnly | `UK2Node_NDCWriteToDataChannel` — a K2Node has to load with the Blueprint compiler, which an Editor module does not |
| `NDCBinderTests` | DeveloperTool | automation tests for the reflection layer |

## Names

Niagara already owns the `FNDC*` prefix, and five of the types under it are writers of their own
(`FNDCWriterBase`, `FNDCVarWriter`, `FNDCScopedWriter`, `FNDCMapKeyWriter`, `FNDCExampleWriter`). So
`NDC` here is the subject, not a claim on the namespace, and everything this plugin declares carries
the plugin's own name after it:

- **`NDCBinder`** is the prefix for anything the plugin owns — modules, types, the log category, the
  automation suite, the API macro. Nothing is called `NDCWriter`: that name is one edit away from
  Niagara's own, and the two would sit next to each other in every completion list. Nor is anything
  called `NDCPayloadWriter` any more, which was the previous answer and named half of what this does —
  the access context is bound the same way and is not payload.
- **Binder** and **binding** are the plugin and its parts: an `FNDCBinder` holds `FNDCVariableBinding`
  rows for the channel's variables and `FNDCContextBinding` rows for its access context. The agent
  noun is the shape Epic uses for a framework named after what it does — Mover moves, Chooser chooses,
  and a Chooser holds `FChooserEnumPropertyBinding`s the same way.
- **Variable** is what the channel calls its fields, so a row that fills one is an `FNDCVariableBinding`
  carrying an `ENDCVariableType` — never a "param". Niagara's own panels say Variable; saying something
  else would be teaching a second word for the same thing.
- **Value** is where a row's content comes from: `ENDCValueSource` is a constant, a field of the event
  data, or a bound function. The variable is the destination, the value is what lands in it.
- **Payload** keeps its narrow meaning — the set of values one write pushes, one element of the
  channel's data or a batch of them — which is what it stopped meaning when it was the plugin's name.

The one deliberate exception is `UK2Node_NDCWriteToDataChannel`, which follows the engine's
`K2Node_<what the node does>` convention rather than the plugin's: it is named after the Blueprint node
an author sees, `Write With NDC Binder`.


## License

MIT — see [LICENSE](LICENSE).
