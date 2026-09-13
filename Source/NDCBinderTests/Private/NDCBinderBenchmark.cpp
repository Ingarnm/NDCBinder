// NDCBinderBenchmark.cpp
//
// What the writer costs over writing the same thing by hand.
//
// Only the difference is measured. Everything from CreateDataChannelWriter_WithContext onwards is
// identical in both implementations — the same writer object, the same FindVariableBuffer scan per
// variable, the same Write* call — so timing it would just add a constant to both sides and make the
// interesting number harder to read. What differs is how a value reaches those calls, and that is
// what these cases pair off:
//
//   per bound-function row   reflected call (ProcessEvent) + reflected store   vs   direct call + store
//   per event data row       field lookup + reflected store                    vs   member read + store
//   per write, fixed         seeding the context from the authored default     vs   constructing one
//
// The per-row numbers are taken through ApplyContextBinding because it is the one public entry point
// that runs the whole per-row machinery — resolve, call, read, store — without needing a UWorld or a
// Niagara channel. A payload row runs the same machinery; it differs only in ending at Writer->Write*
// instead of a struct field, which is the shared part above.
//
// These are ns/op on one machine, not a threshold. They exist to answer "what order of magnitude is
// this", to be compared between build configurations, and to be re-run when the resolve path changes.

#include "NDCBinderBenchmark.h"

#include "NDCBinderTestTypes.h"
#include "NiagaraDataChannel.h"
#include "NiagaraDataChannelAccessor.h"
#include "NiagaraDataChannelAsset.h"
#include "NiagaraDataChannelFunctionLibrary.h"
#include "NiagaraDataChannelGameData.h"
#include "NiagaraDataChannelLayoutInfo.h"
#include "NiagaraDataChannelVariable.h"
#include "NiagaraWorldManager.h"

#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "NDCBinder.h"
#include "NiagaraDataChannelAccessContext.h"
#include "StructUtils/StructView.h"
#include "UObject/Package.h"

namespace NDCBinderBenchmarkPrivate
{
	/** Somewhere for every case to leave its result, so no branch of this can be optimized away. */
	static volatile double Sink = 0.0;

	/** How many times each case is timed; the fastest of them is the answer. See TimeNs. */
	static constexpr int32 NumTimedRuns = 4;

	/**
	 * Nanoseconds per iteration, after a warm-up pass that pays for the first-call costs the real
	 * thing also pays once — the bound-function cache filling, branch predictors settling, the
	 * context's allocation being made.
	 *
	 * Timed several times over, and the LOWEST kept. Noise on a desktop only ever adds — a scheduler
	 * slice handed to something else, a migration between cores — so the fastest run is the one least
	 * interrupted, and an average would report the interruptions as if they were the code. This used
	 * to be a discipline the person running the report followed by hand, four times, and a number in
	 * the README saying they had; doing it here is the same method and it cannot be forgotten.
	 */
	template <typename BodyType>
	static double TimeNs(int32 Iterations, BodyType&& Body)
	{
		for (int32 Pass = 0; Pass < Iterations / 8; ++Pass)
		{
			Body();
		}

		double BestNs = TNumericLimits<double>::Max();
		for (int32 Run = 0; Run < NumTimedRuns; ++Run)
		{
			const uint64 Start = FPlatformTime::Cycles64();
			for (int32 Pass = 0; Pass < Iterations; ++Pass)
			{
				Body();
			}
			const uint64 End = FPlatformTime::Cycles64();
			BestNs = FMath::Min(BestNs, FPlatformTime::ToSeconds64(End - Start) * 1e9 / Iterations);
		}
		return BestNs;
	}

	static FNDCAccessContextInst MakeContext()
	{
		FNDCAccessContextInst Context;
		Context.AccessContext.InitializeAs(FNDCAccessContext::StaticStruct());
		return Context;
	}

	/**
	 * The same write, the way a Blueprint graph does it.
	 *
	 * The reference is /Game/NiagaraExamples/Blueprints/TriggerImpact, function ImpactNDC: a
	 * "Make (NDCAccess Context Gameplay Burst) Instance" node with five inputs set, then a
	 * "Write To Niagara Data Channel" node with eight payload pins. The channel below is that write
	 * node's pin list, variable for variable.
	 *
	 * Neither node is what it looks like. The make node is a CustomThunk that builds a fresh
	 * FNDCAccessContextInst and then takes one SetSinglePropertyInNDCAccessContextInstance per
	 * exposed pin — a pair of IsChildOf checks, a FindPropertyByName and a generic copy, uncached,
	 * per property per write. The write node "is just a placeholder" (NiagaraDataChannelFunctionLibrary)
	 * and expands into CreateDataChannelWriter_WithContext plus one UNiagaraDataChannelWriter::Write*
	 * per payload pin — BY NAME, so each one walks the channel's variables inside FindVariableBuffer.
	 *
	 * Both sides here are the NATIVE HALF ONLY. A Blueprint graph also pays the VM to dispatch each of
	 * those calls — fifteen of them for this write — and to assemble their arguments; none of that is
	 * counted. So this understates their side and never ours. What is equal: the same channel, the
	 * same eight values, the same world, one element per write.
	 *
	 * The payload values are literals on both sides, which is the fair shape of the question: the
	 * comparison is how each way GETS a value to the channel, not how it computes one.
	 */
	static TArray<FString> RunBlueprintComparison(UWorld* World)
	{
		//~ Fewer iterations than the world-free cases, for the same reason RunWorldCases uses fewer:
		//~ every write here appends to the channel's game data, which no tick is consuming.
		constexpr int32 Iterations = 20000;

		TArray<FString> Report;

		FNiagaraWorldManager* WorldMan = World ? FNiagaraWorldManager::Get(World) : nullptr;
		if (!WorldMan)
		{
			return Report;
		}

		TArray<TPair<FName, FNiagaraTypeDefinition>> Vars;
		Vars.Emplace(TEXT("Position"),        FNiagaraTypeDefinition::GetVec3Def());
		Vars.Emplace(TEXT("Normal"),          FNiagaraTypeDefinition::GetVec3Def());
		Vars.Emplace(TEXT("HitVelocity"),     FNiagaraTypeDefinition::GetVec3Def());
		Vars.Emplace(TEXT("HitDirection"),    FNiagaraTypeDefinition::GetVec3Def());
		Vars.Emplace(TEXT("SurfaceType"),     FNiagaraTypeDefinition::GetIntDef());
		Vars.Emplace(TEXT("Distance"),        FNiagaraTypeDefinition::GetFloatDef());
		Vars.Emplace(TEXT("IsLocal"),         FNiagaraTypeDefinition::GetBoolDef());
		Vars.Emplace(TEXT("SpawnCountScale"), FNiagaraTypeDefinition::GetFloatDef());

		UNiagaraDataChannelAsset* Asset = NDCBinderTestChannel::Make(Vars);
		UNiagaraDataChannel* Channel = Asset ? Asset->Get() : nullptr;
		if (!Channel)
		{
			return Report;
		}
		Asset->AddToRoot();
		WorldMan->InitDataChannel(Channel, /*bForce=*/ true);

		const TNDCAccessContextType ContextType = Channel->GetAccessContextType();
		const UScriptStruct* const ContextStruct = ContextType.Get();

		//~ The eight values, and their names hoisted: a name in the loop would time an FName
		//~ construction rather than the write, and a Blueprint's literal name is a constant in its
		//~ bytecode — so hoisting is what their side actually gets, not a favour to it.
		const FName NamePosition(TEXT("Position"));
		const FName NameNormal(TEXT("Normal"));
		const FName NameHitVelocity(TEXT("HitVelocity"));
		const FName NameHitDirection(TEXT("HitDirection"));
		const FName NameSurfaceType(TEXT("SurfaceType"));
		const FName NameDistance(TEXT("Distance"));
		const FName NameIsLocal(TEXT("IsLocal"));
		const FName NameSpawnCountScale(TEXT("SpawnCountScale"));

		const FVector Position(10.0, 20.0, 30.0);
		const FVector Normal(0.0, 0.0, 1.0);
		const FVector HitVelocity(4.0, 5.0, 6.0);
		const FVector HitDirection(0.0, 1.0, 0.0);
		constexpr int32 SurfaceType = 2;
		constexpr double Distance = 512.0;
		constexpr bool bIsLocal = true;
		constexpr double SpawnCountScale = 1.5;

		//~ THIS WRITER: eight constant rows and an authored context, which is the whole authored
		//~ state. Nothing is set per write — that is the difference being measured.
		FNDCBinder Writer;
		Writer.DataChannel = Asset;
		{
			TArray<FNDCVariableBinding>& Rows = Writer.GetMutableBindingsUnchecked();
			auto AddRow = [&Rows](FName Name, ENDCVariableType Type) -> FNDCVariableBinding&
			{
				FNDCVariableBinding& Row = Rows.AddDefaulted_GetRef();
				Row.VarName = Name;
				Row.Type = Type;
				Row.Source = ENDCValueSource::Constant;
				return Row;
			};
			AddRow(NamePosition, ENDCVariableType::Vector).VectorValue = Position;
			AddRow(NameNormal, ENDCVariableType::Vector).VectorValue = Normal;
			AddRow(NameHitVelocity, ENDCVariableType::Vector).VectorValue = HitVelocity;
			AddRow(NameHitDirection, ENDCVariableType::Vector).VectorValue = HitDirection;
			AddRow(NameSurfaceType, ENDCVariableType::Int32).IntValue = SurfaceType;
			AddRow(NameDistance, ENDCVariableType::Float).FloatValue = Distance;
			AddRow(NameIsLocal, ENDCVariableType::Bool).BoolValue = bIsLocal;
			AddRow(NameSpawnCountScale, ENDCVariableType::Float).FloatValue = SpawnCountScale;
		}
		Writer.DefaultAccessContext.Init(ContextType);

		//~ THE BLUEPRINT WAY: the context inputs its make node would expose, and a source value for
		//~ each, built once. The thunk copies from the VM stack; what it costs is the copy, not where
		//~ the bytes came from.
		TArray<const FProperty*> ContextInputs;
		FNDCBinder::ForEachContextInputField(ContextStruct, [&ContextInputs](FProperty& Field)
		{
			if (ContextInputs.Num() < 5)
			{
				ContextInputs.Add(&Field);
			}
		});
		TArray<TArray<uint8>> ContextSources;
		ContextSources.Reserve(ContextInputs.Num());
		for (const FProperty* Field : ContextInputs)
		{
			TArray<uint8>& Buffer = ContextSources.AddDefaulted_GetRef();
			Buffer.SetNumZeroed(Field->GetElementSize());
			Field->InitializeValue(Buffer.GetData());
		}

		const UScriptStruct* const ContextBase = FNDCAccessContextBase::StaticStruct();
		const FString DebugSource(TEXT("NDCBinderBenchmark"));

		// One of each first: a case that silently writes nothing would otherwise be timed as one that works.
		const bool bOursWorks = Writer.WriteToChannel(World, nullptr, FConstStructView());

		const double OursNs = TimeNs(Iterations, [&]()
		{
			Sink += Writer.WriteToChannel(World, nullptr, FConstStructView()) ? 1.0 : 0.0;
		});

		bool bTheirsWorks = false;
		const double TheirsNs = TimeNs(Iterations, [&]()
		{
			// Make (NDCAccess Context ...) Instance — a fresh instance every write, as the node returns.
			FNDCAccessContextInst Context;
			Context.Init(ContextType);

			// ...then one SetSinglePropertyInNDCAccessContextInstance per exposed pin.
			if (uint8* ContextMemory = Context.AccessContext.GetMutableMemory())
			{
				for (int32 Index = 0; Index < ContextInputs.Num(); ++Index)
				{
					if (ContextStruct->IsChildOf(ContextBase) && ContextStruct->IsChildOf(ContextStruct))
					{
						if (const FProperty* Dest = ContextStruct->FindPropertyByName(ContextInputs[Index]->GetFName()))
						{
							Dest->CopyCompleteValue(
								Dest->ContainerPtrToValuePtr<uint8>(ContextMemory), ContextSources[Index].GetData());
						}
					}
				}
			}

			// Write To Niagara Data Channel: a writer, then one call per payload pin, by name.
			if (UNiagaraDataChannelWriter* ChannelWriter =
				UNiagaraDataChannelLibrary::CreateDataChannelWriter_WithContext(
					World, Channel, Context, /*Count=*/ 1, true, true, true, DebugSource))
			{
				bTheirsWorks = true;
				ChannelWriter->WriteVector(NamePosition, 0, Position);
				ChannelWriter->WriteVector(NameNormal, 0, Normal);
				ChannelWriter->WriteVector(NameHitVelocity, 0, HitVelocity);
				ChannelWriter->WriteVector(NameHitDirection, 0, HitDirection);
				ChannelWriter->WriteInt(NameSurfaceType, 0, SurfaceType);
				ChannelWriter->WriteFloat(NameDistance, 0, Distance);
				ChannelWriter->WriteBool(NameIsLocal, 0, bIsLocal);
				ChannelWriter->WriteFloat(NameSpawnCountScale, 0, SpawnCountScale);
			}
			Sink += 1.0;
		});

		if (!bOursWorks || !bTheirsWorks)
		{
			Report.Add(FString::Printf(
				TEXT("vs BP    | skipped: a side wrote nothing (ours=%d theirs=%d), so the pair means nothing"),
				(int32)bOursWorks, (int32)bTheirsWorks));
		}
		else
		{
			Report.Add(FString::Printf(TEXT("vs BP    | Blueprint nodes, native half : %7.1f ns  (make context + %d property sets + writer + 8 writes by name)"),
				TheirsNs, ContextInputs.Num()));
			Report.Add(FString::Printf(TEXT("vs BP    | this writer, same 8 values   : %7.1f ns  (x%.2f)"),
				OursNs, TheirsNs / FMath::Max(OursNs, KINDA_SMALL_NUMBER)));
			Report.Add(TEXT("vs BP    | and their VM dispatch — 15 calls for this write — is on top of theirs, uncounted"));
		}

		WorldMan->RemoveDataChannel(Channel);
		Asset->RemoveFromRoot();
		return Report;
	}

	/**
	 * A WHOLE WRITE, with Niagara's own per-write path inside it — the one quantity every case above
	 * deliberately leaves out.
	 *
	 * Everything else here measures what the writer costs OVER writing the same thing by hand, which
	 * is the right question for the abstraction and the wrong one for a frame budget: it says nothing
	 * about what a write costs, only about what this way of asking for one adds. The rest is
	 * FNDCWriterBase::BeginWrite — a world lookup, the Niagara world manager, the channel's handler,
	 * FindData, GetGameDataForWriteGT and a SetNum across every variable buffer — and none of it was
	 * ever measured, so nobody could say whether the nanoseconds above matter at all.
	 *
	 * Needs a world, which is why it is separate and skipped without one. The channel is a Global one:
	 * the cheapest kind, and therefore a FLOOR for Niagara's share. A GameplayBurst channel resolves
	 * an attachment and a spatial bucket inside FindData on top of this.
	 *
	 * Fewer iterations than the rest: each write appends to the channel's game data, and Niagara
	 * flushes and reallocates that every 128 elements, so a 200k-iteration loop would spend the run
	 * accumulating publish requests no tick ever consumes.
	 */
	static TArray<FString> RunWorldCases(UWorld* World, UNDCBinderTestFunctionHost* Host, FConstStructView EventView)
	{
		TArray<FString> Report;
		if (!World)
		{
			Report.Add(TEXT("real write| skipped: no world was given, so Niagara's own per-write path cannot run"));
			return Report;
		}

		FNiagaraWorldManager* WorldMan = FNiagaraWorldManager::Get(World);
		if (!WorldMan)
		{
			Report.Add(TEXT("real write| skipped: this world has no Niagara world manager"));
			return Report;
		}

		constexpr int32 NumRows = 5;
		TArray<TPair<FName, FNiagaraTypeDefinition>> Vars;
		for (int32 Index = 0; Index < NumRows; ++Index)
		{
			Vars.Emplace(FName(*FString::Printf(TEXT("Var%d"), Index)), FNiagaraTypeDefinition::GetVec3Def());
		}

		UNiagaraDataChannelAsset* Asset = NDCBinderTestChannel::Make(Vars);
		UNiagaraDataChannel* Channel = Asset ? Asset->Get() : nullptr;
		if (!Channel)
		{
			Report.Add(TEXT("real write| skipped: could not build a channel to write to"));
			return Report;
		}
		Asset->AddToRoot();

		// Without this the world has no handler for the channel and every write below would return
		// false on its first branch — a fast, meaningless number.
		WorldMan->InitDataChannel(Channel, /*bForce=*/ true);

		// Five rows of the same type, all reading the same thing, so the only difference between the
		// two writers is how a row gets its value.
		auto MakeWriter = [Asset](ENDCValueSource Source)
		{
			FNDCBinder Writer;
			Writer.SetEventDataTypeUnchecked(FNDCBinderTestContext::StaticStruct());
			Writer.DataChannel = Asset;

			TArray<FNDCVariableBinding>& Rows = Writer.GetMutableBindingsUnchecked();
			for (int32 Index = 0; Index < NumRows; ++Index)
			{
				const FName VarName(*FString::Printf(TEXT("Var%d"), Index));
				Rows.Add(Source == ENDCValueSource::EventData
					? FNDCVariableBinding::MakeEventDataBinding(VarName, ENDCVariableType::Vector, TEXT("Location"))
					: FNDCVariableBinding::MakeFunctionBinding(VarName, ENDCVariableType::Vector, TEXT("ConstEventDataRef")));
			}
			return Writer;
		};

		const FNDCBinder EventDataWriter = MakeWriter(ENDCValueSource::EventData);
		const FNDCBinder FunctionWriter = MakeWriter(ENDCValueSource::Function);

		// One real write first: a case that silently fails would otherwise be timed as a success.
		if (!EventDataWriter.WriteToChannel(World, Host, EventView))
		{
			Report.Add(TEXT("real write| skipped: the write did not go through, so there is nothing to time"));
			WorldMan->RemoveDataChannel(Channel);
			Asset->RemoveFromRoot();
			return Report;
		}

		constexpr int32 WorldIterations = 20000;

		const double FullEventDataNs = TimeNs(WorldIterations, [&]()
		{
			Sink += EventDataWriter.WriteToChannel(World, Host, EventView) ? 1.0 : 0.0;
		});

		const double FullFunctionNs = TimeNs(WorldIterations, [&]()
		{
			Sink += FunctionWriter.WriteToChannel(World, Host, EventView) ? 1.0 : 0.0;
		});

		// The same write with its rows taken out: everything a write pays before a single value moves.
		FNDCAccessContextInst OpenContext;
		EventDataWriter.ResolveAccessContext(OpenContext, Host, EventView);
		const double OpenCloseNs = TimeNs(WorldIterations, [&]()
		{
			FNDCWriteScope Scope;
			Sink += EventDataWriter.BeginWrite(Scope, World, OpenContext, /*Count=*/ 1, Host) ? 1.0 : 0.0;
			// The scope's destructor ends the write and hands the buffer back.
		});

		// And the context half on its own, against the same writer.
		const double ContextNs = TimeNs(WorldIterations, [&]()
		{
			Sink += EventDataWriter.ResolveAccessContext(OpenContext, Host, EventView) ? 1.0 : 0.0;
		});

		//~ Eight elements, two ways. This is the shape a shotgun or a multi-hit cue has, and the
		//~ question the mothballed multi-element API exists to answer: does opening one write for
		//~ eight elements beat opening eight writes of one.
		constexpr int32 Burst = 8;
		const int32 BurstIterations = WorldIterations / Burst;

		const double EightSinglesNs = TimeNs(BurstIterations, [&]()
		{
			for (int32 Index = 0; Index < Burst; ++Index)
			{
				Sink += EventDataWriter.WriteToChannel(World, Host, EventView) ? 1.0 : 0.0;
			}
		});

		FNDCAccessContextInst BurstContext;
		const double OneBurstNs = TimeNs(BurstIterations, [&]()
		{
			EventDataWriter.ResolveAccessContext(BurstContext, Host, EventView);
			FNDCWriteScope Scope;
			if (EventDataWriter.BeginWrite(Scope, World, BurstContext, Burst, Host))
			{
				for (int32 Index = 0; Index < Burst; ++Index)
				{
					EventDataWriter.WriteBindings(Scope, Index, Host, EventView);
				}
			}
			Sink += 1.0;
		});

		Report.Add(FString::Printf(TEXT("real write| 5 rows on event data     : %7.1f ns  (a whole WriteToChannel, Global channel)"), FullEventDataNs));
		Report.Add(FString::Printf(TEXT("real write| 5 rows on functions      : %7.1f ns"), FullFunctionNs));
		Report.Add(FString::Printf(TEXT("  of which| open + close, no rows    : %7.1f ns  (%.0f%% of the write; Niagara's own path plus the guard)"),
			OpenCloseNs, 100.0 * OpenCloseNs / FMath::Max(FullEventDataNs, KINDA_SMALL_NUMBER)));
		Report.Add(FString::Printf(TEXT("  of which| access context          : %7.1f ns  (%.0f%%)"),
			ContextNs, 100.0 * ContextNs / FMath::Max(FullEventDataNs, KINDA_SMALL_NUMBER)));
		Report.Add(FString::Printf(TEXT("  of which| the five rows           : %7.1f ns  (%.0f%%)"),
			FMath::Max(FullEventDataNs - OpenCloseNs - ContextNs, 0.0),
			100.0 * FMath::Max(FullEventDataNs - OpenCloseNs - ContextNs, 0.0) / FMath::Max(FullEventDataNs, KINDA_SMALL_NUMBER)));
		// And the form a caller actually writes, which is the one the README points at: a single
		// WriteToChannel taking the whole batch. The manual case above answers whether opening one
		// write for eight beats opening eight — this one answers what that is worth through the API a
		// caller has, its per-call checks and its guard included, so the number and the call in the
		// documentation are the same thing.
		TArray<FConstStructView, TInlineAllocator<Burst>> BurstViews;
		BurstViews.Init(EventView, Burst);
		const double OneCallBurstNs = TimeNs(BurstIterations, [&]()
		{
			Sink += EventDataWriter.WriteToChannel(World, Host, BurstViews) ? 1.0 : 0.0;
		});

		Report.Add(FString::Printf(TEXT("8 elements| eight writes of one      : %7.1f ns"), EightSinglesNs));
		Report.Add(FString::Printf(TEXT("8 elements| one write of eight       : %7.1f ns  (-%.1f ns, x%.2f, scope by hand)"),
			OneBurstNs, EightSinglesNs - OneBurstNs, EightSinglesNs / FMath::Max(OneBurstNs, KINDA_SMALL_NUMBER)));
		Report.Add(FString::Printf(TEXT("8 elements| one WriteToChannel(array): %7.1f ns  (-%.1f ns, x%.2f, what a caller writes)"),
			OneCallBurstNs, EightSinglesNs - OneCallBurstNs, EightSinglesNs / FMath::Max(OneCallBurstNs, KINDA_SMALL_NUMBER)));

		WorldMan->RemoveDataChannel(Channel);
		Asset->RemoveFromRoot();

		Report.Append(RunBlueprintComparison(World));
		return Report;
	}
}

TArray<FString> NDCBinderBenchmark::Run(UWorld* World)
{
	using namespace NDCBinderBenchmarkPrivate;

	constexpr int32 Iterations = 200000;
	TArray<FString> Report;

	UNDCBinderTestFunctionHost* Host = NewObject<UNDCBinderTestFunctionHost>(GetTransientPackage());
	Host->AddToRoot();

	FNDCBinder Writer;
	Writer.SetEventDataTypeUnchecked(FNDCBinderTestContext::StaticStruct());

	FNDCBinderTestContext EventData;
	EventData.Location = FVector(1.0, 2.0, 3.0);
	EventData.Tags = { TEXT("Alpha"), TEXT("Beta") };
	const FConstStructView EventView = FConstStructView::Make(EventData);

	FNDCAccessContextInst Context = MakeContext();
	FNDCAccessContext& Ctx = Context.GetChecked<FNDCAccessContext>();

	const FNDCContextBinding FunctionRow = FNDCContextBinding::Make(
		TEXT("Location"), TEXT("PerfGetVector"), /*bRequired=*/ false, TEXT("bOverrideLocation"));
	const FNDCContextBinding EventDataRow = FNDCContextBinding::MakeFromEventData(
		TEXT("Location"), TEXT("Location"), /*bRequired=*/ false, TEXT("bOverrideLocation"));

	//~ One row, three ways.

	const double DirectNs = TimeNs(Iterations, [&]()
	{
		Ctx.Location = Host->PerfGetVectorDirect(EventData);
		Ctx.bOverrideLocation = true;
		Sink += Ctx.Location.X;
	});

	const double EventDataNs = TimeNs(Iterations, [&]()
	{
		Writer.ApplyContextBinding(EventDataRow, Context, Host, EventView);
		Sink += Ctx.Location.X;
	});

	const double FunctionNs = TimeNs(Iterations, [&]()
	{
		Writer.ApplyContextBinding(FunctionRow, Context, Host, EventView);
		Sink += Ctx.Location.X;
	});

	//~ What the event data parameter's SHAPE costs, isolated.
	//~
	//~ These two host functions differ in exactly one thing: `const FNDCBinderTestContext&` against
	//~ `FNDCBinderTestContext`. Same return type, same const-ness, same plain UFUNCTION dispatch — so
	//~ the difference between the two numbers is the deep copy of the event data struct and nothing
	//~ else. The call path aliases a const reference and copies everything else
	//~ (FNDCBinder::GetEventDataPassing), which is why the details panel generates the
	//~ const-reference shape rather than the by-value one a Blueprint pin gives by default.
	//~
	//~ It is a FLOOR for what that decision is worth, not the figure: this struct owns one array, and
	//~ a real event data struct owns more of them — FGameplayCueParameters carries two tag containers
	//~ and a shared pointer, each with its own copy to make and destroy.
	const FNDCContextBinding ConstRefRow = FNDCContextBinding::Make(
		TEXT("Location"), TEXT("ConstEventDataRef"), /*bRequired=*/ false, TEXT("bOverrideLocation"));
	const FNDCContextBinding ByValueRow = FNDCContextBinding::Make(
		TEXT("Location"), TEXT("EventDataOnly"), /*bRequired=*/ false, TEXT("bOverrideLocation"));

	const double ConstRefNs = TimeNs(Iterations, [&]()
	{
		Writer.ApplyContextBinding(ConstRefRow, Context, Host, EventView);
		Sink += Ctx.Location.X;
	});

	const double ByValueNs = TimeNs(Iterations, [&]()
	{
		Writer.ApplyContextBinding(ByValueRow, Context, Host, EventView);
		Sink += Ctx.Location.X;
	});

	//~ The name is hoisted deliberately. Passing TEXT("Location") here instead would build an FName
	//~ from a string every iteration — a hash and a name-table probe — and time that rather than the
	//~ lookup. A row stores its name as an FName already, so this is what it actually pays.
	const FName BoundFieldName(TEXT("Location"));
	const double FindFieldNs = TimeNs(Iterations, [&]()
	{
		Sink += (double)(UPTRINT)Writer.FindEventDataField(BoundFieldName);
	});

	//~ What an editor build pays that a Shipping one does not.
	//~
	//~ FNDCBoundFunctionCache::TryGet re-resolves the function by name inside an ensureMsgf, on every
	//~ cache hit, under WITH_EDITOR — a deliberate development-only net over the caching. It is on the
	//~ function-row path measured above, so a Shipping build's row is cheaper by roughly this much.
	//~ ProcessEvent also drops its script call-stack tracking there (DO_BLUEPRINT_GUARD), which this
	//~ cannot isolate. Settling that means a Shipping run, which needs the module retyped to Runtime
	//~ first; see NDCBinderBenchmark.h.
	const UClass* HostClass = Host->GetClass();
	const FName BoundFuncName(TEXT("PerfGetVector"));
	const double FindFunctionNs = TimeNs(Iterations, [&]()
	{
		Sink += (double)(UPTRINT)HostClass->FindFunctionByName(BoundFuncName);
	});

	//~ The other way a project would do this: Niagara's own Blueprint nodes.
	//~
	//~ Not a hand-written C++ baseline this time but the actual competitor. UK2Node_DataChannelAccessContext_Make
	//~ expands to one SetSinglePropertyInNDCAccessContextInstance call per exposed context pin, and the
	//~ write node "is just a placeholder and calls into CreateDataChannelWriter_WithContext and its
	//~ individual write functions from the BP node" (NiagaraDataChannelFunctionLibrary.cpp) — one
	//~ Write* call per payload pin, exactly as this writer makes.
	//~
	//~ The payload side used to be the same call on both sides and cancel out. It is not any more:
	//~ the BP node writes by name and this writer writes by buffer index, which the "full store" pair
	//~ below measures on both sides at once. The context side is what
	//~ differs, and this is the native half of Epic's version of it, lifted from
	//~ execSetSinglePropertyInNDCAccessContextInstance: two IsChildOf walks, a FindPropertyByName over
	//~ the context struct, and a generic copy. Uncached, per property, per write.
	//~
	//~ What it does NOT include is the Blueprint VM call that dispatches to it — one per property —
	//~ nor the VM cost of the nodes that computed the value. Both are on Epic's side of the ledger
	//~ only, so this number understates their path and never this one.
	const UScriptStruct* CtxStruct = FNDCAccessContext::StaticStruct();
	const UScriptStruct* CtxBase = FNDCAccessContextBase::StaticStruct();
	uint8* CtxMemory = Context.AccessContext.GetMutableMemory();
	const FName ContextPropName(TEXT("Location"));
	const FVector SourceValue(4.0, 5.0, 6.0);

	const double EpicContextNs = TimeNs(Iterations, [&]()
	{
		if (CtxStruct->IsChildOf(CtxBase) && CtxStruct->IsChildOf(CtxStruct))
		{
			if (FProperty* DestProperty = CtxStruct->FindPropertyByName(ContextPropName))
			{
				DestProperty->CopyCompleteValue(DestProperty->ContainerPtrToValuePtr<uint8>(CtxMemory), &SourceValue);
			}
		}
		Sink += Ctx.Location.X;
	});

	//~ The row write itself, which nothing above measures.
	//~
	//~ Writing a row by name — every Write* on UNiagaraDataChannelWriter, and so every write this
	//~ plugin used to make — ends in FNiagaraDataChannelGameData::FindVariableBuffer, which walks the
	//~ channel's variables comparing an FName and a type. Per row, per write. Writing by the buffer
	//~ index instead is the same store with the walk hoisted out, so the pair below is the whole of
	//~ what that change is worth — and it is measured at two channel widths because the walk is the
	//~ part that scales with how wide the channel is.
	//~
	//~ No world and no handler here: this is the buffer store, not the write pipeline around it.
	auto TimeRowWrite = [&](int32 NumVars, double& OutByName, double& OutByOffset, double& OutAllByName, double& OutAllByOffset)
	{
		OutByName = 0.0;
		OutByOffset = 0.0;
		OutAllByName = 0.0;
		OutAllByOffset = 0.0;

		TArray<TPair<FName, FNiagaraTypeDefinition>> Vars;
		Vars.Reserve(NumVars);
		for (int32 VarIndex = 0; VarIndex < NumVars; ++VarIndex)
		{
			Vars.Emplace(FName(*FString::Printf(TEXT("Var%d"), VarIndex)), FNiagaraTypeDefinition::GetVec3Def());
		}

		UNiagaraDataChannelAsset* Asset = NDCBinderTestChannel::Make(Vars);
		UNiagaraDataChannel* Channel = Asset ? Asset->Get() : nullptr;
		if (!Channel)
		{
			return;
		}
		const FNiagaraDataChannelGameDataPtr Data = Channel->CreateGameData();
		if (!Data.IsValid())
		{
			return;
		}
		Data->SetNum(1);

		// The last variable, so the walk pays its full length — which is what a row in the middle of a
		// wide channel costs on average anyway, doubled.
		const FNiagaraVariableBase LastVar(
			FNiagaraDataChannelVariable::ToDataChannelType(FNiagaraTypeDefinition::GetVec3Def()),
			Vars.Last().Key);
		const FVector Value(1.0, 2.0, 3.0);

		OutByName = TimeNs(Iterations, [&]()
		{
			if (FNiagaraDataChannelVariableBuffer* Buffer = Data->FindVariableBuffer(LastVar))
			{
				Buffer->Write<FVector>(0, Value);
			}
			Sink += 1.0;
		});

		int32 Offset = INDEX_NONE;
		if (const FNiagaraDataChannelLayoutInfoPtr Layout = Channel->GetLayoutInfo())
		{
			if (const int32* Found = Layout->GetGameDataLayout().VariableIndices.Find(LastVar))
			{
				Offset = *Found;
			}
		}
		OutByOffset = TimeNs(Iterations, [&]()
		{
			Data->Write<FVector>(Offset, 0, Value);
			Sink += 1.0;
		});

		// A whole write, not one row of it. This is the quantity the two implementations actually
		// differ by: the by-name walk is shorter for the first variable than for the last, so five rows
		// do not cost five times the worst row. Measured rather than derived from the pair above.
		TArray<FNiagaraVariableBase> AllVars;
		TArray<int32> AllOffsets;
		AllVars.Reserve(NumVars);
		AllOffsets.Reserve(NumVars);
		const FNiagaraDataChannelLayoutInfoPtr AllLayout = Channel->GetLayoutInfo();
		for (const TPair<FName, FNiagaraTypeDefinition>& Var : Vars)
		{
			const FNiagaraVariableBase Full(
				FNiagaraDataChannelVariable::ToDataChannelType(FNiagaraTypeDefinition::GetVec3Def()), Var.Key);
			AllVars.Add(Full);
			const int32* Found = AllLayout.IsValid() ? AllLayout->GetGameDataLayout().VariableIndices.Find(Full) : nullptr;
			AllOffsets.Add(Found ? *Found : INDEX_NONE);
		}

		OutAllByName = TimeNs(Iterations, [&]()
		{
			for (const FNiagaraVariableBase& Var : AllVars)
			{
				if (FNiagaraDataChannelVariableBuffer* Buffer = Data->FindVariableBuffer(Var))
				{
					Buffer->Write<FVector>(0, Value);
				}
			}
			Sink += 1.0;
		});

		OutAllByOffset = TimeNs(Iterations, [&]()
		{
			for (const int32 RowOffset : AllOffsets)
			{
				Data->Write<FVector>(RowOffset, 0, Value);
			}
			Sink += 1.0;
		});
	};

	double NarrowByName = 0.0, NarrowByOffset = 0.0, NarrowAllByName = 0.0, NarrowAllByOffset = 0.0;
	double WideByName = 0.0, WideByOffset = 0.0, WideAllByName = 0.0, WideAllByOffset = 0.0;
	TimeRowWrite(5, NarrowByName, NarrowByOffset, NarrowAllByName, NarrowAllByOffset);
	TimeRowWrite(20, WideByName, WideByOffset, WideAllByName, WideAllByOffset);

	//~ The fixed per-write part: the writer copies the authored default into the channel's scratch
	//~ context, where hand-written code constructs a fresh one and fills what it needs.

	FNDCBinder Seeded;
	Seeded.DefaultAccessContext.AccessContext.InitializeAs(FNDCAccessContext::StaticStruct());
	const uint8* DefaultMemory = Seeded.DefaultAccessContext.AccessContext.GetMemory();
	FNDCAccessContextInst Scratch = MakeContext();

	const double SeedNs = TimeNs(Iterations, [&]()
	{
		Scratch.AccessContext.InitializeAs(FNDCAccessContext::StaticStruct(), DefaultMemory);
		Sink += Scratch.GetChecked<FNDCAccessContext>().Location.X;
	});

	const TNDCAccessContextType ContextType(FNDCAccessContext::StaticStruct());
	const double ConstructNs = TimeNs(Iterations, [&]()
	{
		FNDCAccessContextInst Fresh(ContextType);
		Fresh.GetChecked<FNDCAccessContext>().bOverrideLocation = true;
		Sink += Fresh.GetChecked<FNDCAccessContext>().Location.X;
	});

	//~ The configuration comes first because these numbers mean nothing without it: an editor build
	//~ measures a path a shipped game does not run.
	Report.Add(FString::Printf(TEXT("build    | WITH_EDITOR=%d SHIPPING=%d TEST=%d DO_CHECK=%d BLUEPRINT_GUARD=%d"),
		(int32)WITH_EDITOR, (int32)UE_BUILD_SHIPPING, (int32)UE_BUILD_TEST, (int32)DO_CHECK, (int32)DO_BLUEPRINT_GUARD));
	Report.Add(FString::Printf(TEXT("per row  | hand-written direct call : %7.1f ns"), DirectNs));
	Report.Add(FString::Printf(TEXT("per row  | bound to event data field: %7.1f ns  (+%.1f ns)"), EventDataNs, EventDataNs - DirectNs));
	Report.Add(FString::Printf(TEXT("per row  | bound to a function      : %7.1f ns  (+%.1f ns)"), FunctionNs, FunctionNs - DirectNs));
	Report.Add(FString::Printf(TEXT("  detail | event data by const ref  : %7.1f ns  (the shape the panel generates)"), ConstRefNs));
	Report.Add(FString::Printf(TEXT("  detail | event data by value      : %7.1f ns  (+%.1f ns, x%.2f — one copied array; a real cue struct owns more)"),
		ByValueNs, ByValueNs - ConstRefNs, ByValueNs / FMath::Max(ConstRefNs, KINDA_SMALL_NUMBER)));
	Report.Add(FString::Printf(TEXT("  detail | event data name lookup   : %7.1f ns  (%.0f%% of an event data row)"),
		FindFieldNs, 100.0 * FindFieldNs / FMath::Max(EventDataNs, KINDA_SMALL_NUMBER)));
	Report.Add(FString::Printf(TEXT("  detail | editor-only cache recheck: %7.1f ns  (%.0f%% of a function row; absent in Shipping)"),
		WITH_EDITOR ? FindFunctionNs : 0.0,
		WITH_EDITOR ? 100.0 * FindFunctionNs / FMath::Max(FunctionNs, KINDA_SMALL_NUMBER) : 0.0));
	Report.Add(FString::Printf(TEXT("per row  | Niagara BP node, native   : %7.1f ns  (its BP VM dispatch is on top, and not counted)"), EpicContextNs));
	Report.Add(FString::Printf(TEXT("row store| by name, 5-var channel   : %7.1f ns"), NarrowByName));
	Report.Add(FString::Printf(TEXT("row store| by offset, 5-var channel : %7.1f ns  (-%.1f ns)"), NarrowByOffset, NarrowByName - NarrowByOffset));
	Report.Add(FString::Printf(TEXT("row store| by name, 20-var channel  : %7.1f ns"), WideByName));
	Report.Add(FString::Printf(TEXT("row store| by offset, 20-var channel: %7.1f ns  (-%.1f ns)"), WideByOffset, WideByName - WideByOffset));
	Report.Add(FString::Printf(TEXT("full store| 5 rows by name          : %7.1f ns"), NarrowAllByName));
	Report.Add(FString::Printf(TEXT("full store| 5 rows by offset        : %7.1f ns  (-%.1f ns)"), NarrowAllByOffset, NarrowAllByName - NarrowAllByOffset));
	Report.Add(FString::Printf(TEXT("full store| 20 rows by name         : %7.1f ns"), WideAllByName));
	Report.Add(FString::Printf(TEXT("full store| 20 rows by offset       : %7.1f ns  (-%.1f ns)"), WideAllByOffset, WideAllByName - WideAllByOffset));
	Report.Add(FString::Printf(TEXT("per write| seed context from default: %7.1f ns"), SeedNs));
	Report.Add(FString::Printf(TEXT("per write| construct one by hand    : %7.1f ns"), ConstructNs));

	// A realistic cue: three payload rows and two context rows. The muzzle cue's shape.
	const double AllFunctions = 5.0 * FunctionNs + SeedNs;
	const double AllEventData = 5.0 * EventDataNs + SeedNs;
	const double HandWritten  = 5.0 * DirectNs + ConstructNs;
	Report.Add(FString::Printf(TEXT("5-row cue| hand-written            : %7.1f ns"), HandWritten));
	Report.Add(FString::Printf(TEXT("5-row cue| all rows on event data  : %7.1f ns  (x%.2f)"), AllEventData, AllEventData / HandWritten));
	Report.Add(FString::Printf(TEXT("5-row cue| all rows on functions   : %7.1f ns  (x%.2f)"), AllFunctions, AllFunctions / HandWritten));

	// The ratio alone is alarming and the absolute number is not, so both are reported. Neither side
	// includes the getter bodies, nor the channel store — that one is reported separately above,
	// because it is no longer the same on both sides and it is larger than any of this.
	constexpr double FrameNs = 16666666.0;
	Report.Add(FString::Printf(TEXT("budget   | one write, functions    : %.5f%% of a 60Hz frame (%.0f writes to reach 1%%)"),
		100.0 * AllFunctions / FrameNs, FrameNs * 0.01 / AllFunctions));
	Report.Add(FString::Printf(TEXT("budget   | one write, event data   : %.5f%% of a 60Hz frame (%.0f writes to reach 1%%)"),
		100.0 * AllEventData / FrameNs, FrameNs * 0.01 / AllEventData));

	Report.Append(RunWorldCases(World, Host, EventView));

	Host->RemoveFromRoot();
	return Report;
}

void NDCBinderBenchmark::RunIfRequestedOnCommandLine()
{
	FString OutPath;
	if (!FParse::Value(FCommandLine::Get(), TEXT("ndcbench="), OutPath) || OutPath.IsEmpty())
	{
		return;
	}

	// Deferred to the first ticking game world, because that is the earliest moment the real-write
	// cases can run at all: they need a world, its Niagara manager and a channel handler, and module
	// startup has none of the three. Every other case is indifferent to when it runs, so waiting
	// costs nothing and is what lets a packaged game produce the whole report rather than the subset
	// that needs no world — which is the only reason this hook exists rather than an automation test.
	static FDelegateHandle TickHandle;
	static bool bRan = false;
	TickHandle = FWorldDelegates::OnWorldTickStart.AddLambda(
		[OutPath](UWorld* World, ELevelTick, float)
		{
			if (bRan || !World || !World->IsGameWorld())
			{
				return;
			}
			bRan = true;
			FWorldDelegates::OnWorldTickStart.Remove(TickHandle);
			TickHandle.Reset();

			FFileHelper::SaveStringArrayToFile(Run(World), *OutPath);
		});
}
