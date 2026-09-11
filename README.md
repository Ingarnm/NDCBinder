# NDC Binder

Binds a **Niagara Data Channel** write to data instead of a graph.

Embed one `FNDCBinder` in anything that pushes data to an NDC. The details panel reflects the
channel's own inputs into rows — its variables under *Payload*, its access context under *Access
Context* — and every row is either a constant you type in or a binding to a field of the event data
or a function on the owning class, through the same chain-icon binding widget UMG uses.

Both halves are bound the same way, which is where the name comes from: what a binder holds is
bindings. Where a write goes is authored exactly like what it carries.

![The details panel](Docs/details-panel.jpg)

The plugin depends on Niagara and nothing else, and names no gameplay framework of its own, so the
same binder works from a GameplayCue, a Gameplay Ability, a component or a plain actor.

## Setting one up in Blueprint

No C++ is required at any point.

**1. Add the binder.** On your Blueprint, add a variable of type **NDC Binder**. Everything below is
authored on it in the details panel.

**2. Assign the Data Channel.** The picker is on the binder's own header row. Binding rows appear the
moment you pick one — one per channel variable under *Payload*, one per access context input under
*Access Context*.

**3. Declare the event data type,** if your getters are going to take one. **Advanced → Event Data
Type**, set to the struct you will pass to the write — `GameplayCueParameters` for a cue, your own
struct otherwise. Leave it unset and bound functions simply take no parameter. The bind menu says so
explicitly when a getter needs it and it is missing.

**4. Fill the rows.** Each row offers three things, and the chain icon opens the menu:

* **a constant** — type the value into the row and nothing runs at write time;
* **a field of the event data** — listed at the top of the bind menu under the struct's name, nested
  members included. Prefer this: nothing is called, the member is simply read;
* **a function on this Blueprint** — listed below the fields. Only functions that actually fit the
  row are offered.

If no suitable function exists yet, **Create Binding** in the menu writes one with the right
signature and binds the row to it in one step, then opens it for you to fill in.

**5. Place the write.** Call **Write With NDC Binder** where the effect should fire.

![The write node in a graph](Docs/blueprint-graph.jpg)

* `Binder` — your binder variable.
* `Owner` — defaults to Self. It is what bound functions are called on.
* `Event Data` — connect your struct straight in. The pin is a wildcard and reads the graph's own
  struct where it already sits, so there is no **Make Instanced Struct** in between. Leave it
  unconnected when no row needs event data.

To emit several elements at once — a shotgun's pellets, a chain of hits — use **Write Many With NDC
Binder**, whose `Event Data` is a wildcard array. One write of N is meaningfully cheaper than N
writes of one, because the channel's per-write cost is then paid once.

## From C++

Embedding and calling is all there is to it; the rows themselves are authored in the panel.

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
    // The struct bound functions take as their parameter. Optional — leave it unset and they take none.
    NDCBinder.InitEventDataType(FMyEventData::StaticStruct());
}

void AMyEffectActor::Fire(const FMyEventData& EventData)
{
    NDCBinder.WriteToChannel(GetWorld(), this, FConstStructView::Make(EventData));
}
```

Declaring getters `BlueprintNativeEvent` lets Blueprint children override them without touching C++.
`WriteToChannel` also takes an array of payloads for the batched form.

## What a row can be bound to

### A field of the event data

The cheap one, and usually the right one. No reflected call, no parameter block — just the member.
Most bound getters in practice are `return EventData.Something;`, which is boilerplate this removes.

Fields may be **nested**: the menu opens a submenu per struct member, so a row reads
`EffectContext.Origin` as easily as `Location`. That submenu is the engine's own — the same
`IPropertyAccessEditor` menu StateTree uses. Nesting costs nothing at write time, because a path of
struct members is a fixed sum of offsets, added up once when the row is first resolved.

A path stops at a struct. An object in the middle would have to be loaded and null-checked on every
write, and an array element needs an index the path does not carry. Both are what a getter is for.

It is deliberately the *event data* and not the owner's properties: a binding is only worth having if
it varies per write, and a GameplayCueNotify executes on its CDO, so reading one of its properties
yields the class default — which is what the row's constant already is.

### A function

```cpp
T Func();
T Func(const FEventData& EventData);   // only when Event Data Type is set
```

`T` must match the row's type. Nothing coerces: a wrong return type is a wrong binding, not a value
quietly converted on the way in. The two conversions that cannot hide a mistake are allowed — a child
struct sliced to its parent (`FNiagaraPosition` → `FVector`) and a soft object reference resolved.

**Take the event data by const reference.** By value works but deep-copies the struct into the
parameter block on every call — for `FGameplayCueParameters` that is two tag containers and a shared
pointer, per row, per write. A const reference is handed over as an alias instead. A *mutable*
reference is refused outright: a binding is a read. Blueprint cannot express the const form by hand,
so **Create Binding** authors the pin correctly, and a test asserts it comes out that way.

A bindable function must also be:

* **`const` (C++) or Pure (Blueprint)** — a cue notify runs on its CDO, so a bound function that
  wrote to a member would mutate state shared by every use of that cue in the game;
* **not editor-only** — otherwise it works through all of editor testing and resolves to nothing once
  cooked;
* **not replicated, and not a delegate signature** — neither has a body `ProcessEvent` will run the
  way a binding needs.

Anything else is fair game: `BlueprintNativeEvent`, `BlueprintImplementableEvent`, a plain
`UFUNCTION()`, static, inherited, or an interface function.

## Access Context

Every input field of the channel's access context is a row: the value editor Unreal builds for its
type, plus a bind button. Author the value, or bind a getter for it — the choice is per field, and
both sit on the same row so there is no second list to keep in step.

A bound getter is an ordinary one. `USceneComponent* GetWeaponComponent(FGameplayCueParameters)`
drives Owning Component; `FVector GetImpactPosition(FGameplayCueParameters)` drives Location. No node
and no context type is involved, which is why the same getter usually already exists for the payload.

Some fields show only a bind button and say *not set — bind to compute it per write*. Those are the
ones Niagara marks `Transient` — `Owning Component`, `Location` — which do not serialize, so there is
no constant to author.

**Required** (object-valued fields only) is how a write declines itself: a non-null answer is taken, a
null answer with *Required* skips the whole write, and a null answer without it leaves the authored
value standing. That last case is what makes a per-surface system getter work — an unmapped surface
returns null and the panel's own System To Spawn goes out.

**Gated fields keep their checkbox.** `System To Spawn` needs `bOverrideSystemToSpawn`, `Cell Size
Override` needs `bOverrideCellSize`; Unreal draws those as the checkbox left of the field's name, and
a write never ticks one for you. Bind a field whose checkbox is clear and the panel marks the row to
say the channel will ignore it, rather than quietly repairing it behind your back. The exception is a
flag Niagara marks `Transient` — `bOverrideLocation` — which cannot be authored at all, so the write
sets it.

Rows here are **sparse**: a field only has one once something is bound to it, and an unbound field
costs nothing at write time. With nothing bound, the write goes out on the access context exactly as
authored — so a channel that buckets spatially needs either a location in the panel or a row bound
to it.

A few fields are deliberately not listed: the channel's outputs, the `bOverride*` flags drawn inline,
and fields a write provably cannot act on. That last list is a setting — **Project Settings → Plugins
→ NDC Binder** — keyed by context type as well as field name.

## When a row goes stale

A row stores a bare name, so it outlives whatever it names. Rename a getter, drop a member from the
event data struct, remove a variable from the channel, and the row is still there naming something
that is gone.

Five things report it, and one of them stops work:

| Where | What it does |
| --- | --- |
| the bind menu | never offers a function that breaks a rule |
| the row | marked in the details panel with the specific reason |
| **the Blueprint compiler** | **an unresolvable row fails the compile of the asset holding it** |
| Validate Assets, the DataValidation commandlet | fails the asset, so a build stops on it |
| the write | skips the row, logging once per owner class and row |

**An error and a warning are different claims.** An error is drift — a row names something gone or no
longer fitting, which nobody asked for. A warning is an unfinished asset — no channel assigned yet, a
row set to read a function with none picked — and that is a state every asset passes through while it
is being authored.

Nothing is destroyed behind your back. A row whose variable the channel no longer has is kept, shown
greyed and skipped at write time — put the variable back and it works again. When you are sure it is
not coming back, **Remove** at the top of *Payload* takes out every row this channel has no place
for. It appears only when there is something to remove, and it is undoable.

Two limits worth knowing: a compile **on load** is skipped on purpose, because dependencies are still
arriving and a binding to a parent Blueprint's function can read as missing when it is merely not
compiled yet; and a data-only Blueprint is not fully compiled on load at all. So a C++ rename surfaces
when something actually compiles the asset, while Validate Assets is what catches it in a build where
nobody opened anything.

## What a graph may build

Owning a binder is supported; assembling one is not. A binder is configuration — rows authored in a
panel and read at write time — so the variable is the whole of what a Blueprint creates:

| | Variable / pin | Make & Break | Split pin |
| --- | --- | --- | --- |
| `FNDCBinder` | yes | no | no |
| `FNDCVariableBinding`, `FNDCContextBinding` | pins only | no | no |
| `ENDCVariableType`, `ENDCValueSource` | no | — | — |

All of it gates editor menus rather than the runtime, so a node someone placed before still compiles.

## Supported variable types

One entry per `UNiagaraDataChannelWriter::Write*` overload: bool, int32, float, vector2D, vector,
vector4, quat, linear color, position, enum, spawn info and id. `SpawnInfo` and `ID` have no constant
editor and are writable from a bound function only. A variable of any other type shows as unsupported
and is skipped.

That list is shorter than what a channel can hold, and **the ceiling is the engine's, not this
plugin's**: the only way into a game-data buffer is a template that asserts `sizeof(T)` against the
variable's recorded size, with no per-element byte-wise entry point anywhere. That is exactly why
Niagara's own Blueprint writer stops at the same twelve.

A **static array** member cannot be bound — neither as a row's source nor as a step in a path.
`FVector Corners[2]` is a single vector property to every type test, and both the offset a path
resolves to and the value a store reads would be element zero: an answer nobody asked for, on a row
that looks exactly like a correct one. Bind a getter returning the element you mean.

An **enum** row is bound by its own enum; a different enumeration is refused, since the two would
agree on an integer and disagree on everything else. A plain `uint8` is still accepted — an author
returning a raw index is saying which index they mean. The value travels as a byte the whole way
down, `WriteEnum` included, so an enumeration with entries past 255 is warned about rather than
failed: the entries that fit keep working.

## What it costs

Short answer: the binding layer is not where a write's time goes.

Measured against the same work written by hand, a row bound to an **event data field** costs a small
multiple of a direct member read — and a direct member read is something the compiler inlines to
nothing, so a large multiple of nearly zero is still nearly zero. A row bound to a **function** costs
noticeably more, because it is a reflected `ProcessEvent` call; that is the reason event data
bindings exist and are the shape to reach for.

Put beside a real write, both are small. **Over half the cost of a write on a simple channel is
Niagara's own per-write path** — resolving the world manager, finding the channel's data, growing the
buffers — and nothing in this plugin can make that cheaper. What the plugin *can* do about it is emit
several elements in one write instead of several writes, which is what the batched form is for.

A bound row does not look itself up any more: names are resolved once per owning class and per struct
and then read as pointers, the same move Epic makes in its own property-binding collection. Against a
Blueprint graph doing the equivalent work by hand — make a context, set its fields, make a writer,
write each value by name — this comes out several times cheaper on the native side alone, before
counting the VM dispatch a graph also pays and this does not.

The plugin ships its own benchmark, so none of this has to be taken on trust. It runs as
`NDCBinder.Performance.BindingOverhead` in the editor, and from the command line in any configuration
— including Shipping, where the automation framework does not exist — by passing
`-ndcbench=<path to a report file>`. Every case is timed several times over and the fastest kept,
because noise on a desktop only ever adds.

## Notes

- Payload rows are synced from the channel asset. Context rows are sparse and are not synced: there
  is no set to reconcile.
- A context row that cannot run — no such field, a missing or unusable function — is skipped with one
  log line and the authored value stands. A broken binding deliberately **cannot** turn a write off,
  even a required one: a typo in a function name would otherwise silently stop an effect from ever
  playing. Only a real null answer declines a write.
- The Blueprint node's Event Data pin is a wildcard and **nothing is copied into it**: the thunk reads
  the property and its address off the VM stack, so the write sees the graph's own struct. An
  `FInstancedStruct` connected there anyway is unwrapped, so a payload that arrived boxed costs
  nothing extra either.
- The access context is two-way: the channel writes back which handler systems a write spawned or
  joined. `WriteToChannel` does not surface them — drive `ResolveAccessContext` and `WriteWithContext`
  yourself and read the context afterwards.
- Writes are game-thread only and cannot be re-entered: if a bound function starts another write to
  the same channel, the nested one is skipped rather than corrupting the outer one, because Niagara
  hands out a single shared writer per channel.
- A write whose owner is already pending kill is skipped with a log line, rather than calling into it
  and quietly writing every bound row's default — which is what `ProcessEvent` would do on its own.
- Misconfiguration is reported once per owner class and row under `LogNDCBinder`, not every frame.

## Modules

| Module | Type | Contents |
| --- | --- | --- |
| `NDCBinder` | Runtime | `FNDCBinder`, `FNDCVariableBinding`, `UNDCBinderLibrary` |
| `NDCBinderEditor` | Editor | the details customization, the compiler extension, the asset validator |
| `NDCBinderUncooked` | UncookedOnly | `UK2Node_NDCWriteToDataChannel` — a K2Node has to load with the Blueprint compiler, which an Editor module does not |
| `NDCBinderTests` | DeveloperTool | the automation suite and the benchmark |

## Names

Niagara already owns the `FNDC*` prefix, and five of the types under it are writers of their own
(`FNDCWriterBase`, `FNDCVarWriter`, `FNDCScopedWriter`, `FNDCMapKeyWriter`, `FNDCExampleWriter`). So
`NDC` here is the subject, not a claim on the namespace, and everything this plugin declares carries
the plugin's own name after it.

**Binder** and **binding** are the plugin and its parts: an `FNDCBinder` holds `FNDCVariableBinding`
rows for the channel's variables and `FNDCContextBinding` rows for its access context. The agent noun
is the shape Epic uses for a framework named after what it does — Mover moves, Chooser chooses, and a
Chooser holds `FChooserEnumPropertyBinding`s the same way.

**Variable** is what the channel calls its fields, so a row that fills one carries an
`ENDCVariableType` — never a "param". **Value** is where a row's content comes from: `ENDCValueSource`
is a constant, a field of the event data, or a bound function. **Payload** keeps its narrow meaning —
the set of values one write pushes.

## License

MIT — see [LICENSE](LICENSE).
