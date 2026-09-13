// NDCBinderCustomization.cpp

#include "NDCBinderCustomization.h"
#include "NDCBinderCustomizationInternal.h"

#include "NDCBinderEditorSettings.h"
#include "NiagaraDataChannel_GameplayBurst.h"

#include "DetailLayoutBuilder.h"
#include "PropertyCustomizationHelpers.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "IDetailGroup.h"
#include "InstancedStructDetails.h"
#include "IPropertyUtilities.h"
#include "PropertyHandle.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Features/IModularFeatures.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/OutputDeviceNull.h"
#include "IPropertyAccessEditor.h"
#include "HAL/FileManager.h"
#include "K2Node_Event.h"
#include "Misc/Paths.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "SourceCodeNavigation.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#include "GameFramework/Actor.h"

#include "NiagaraDataChannelAsset.h"
#include "NiagaraDataChannel.h"
#include "NiagaraDataChannelVariable.h"
#include "NiagaraDataChannelAccessContext.h"
#include "NiagaraSystem.h"

#define LOCTEXT_NAMESPACE "NDCBinderCustomization"

namespace NDCBinderCustomizationPrivate
{
	/**
	 * Which colour a marker gets, and the rule behind it: the colour is the VALIDATOR'S verdict, not a
	 * judgement of the panel's own.
	 *
	 * FStyleColors::Error for what fails the asset's compile — a row whose channel variable is gone, a
	 * binding that no longer resolves, an Event Data Type that no bound function takes. Red is what
	 * the rest of the editor uses to mean "this will not build", and these do.
	 *
	 * FStyleColors::Warning for what ValidateBindings says nothing about and the panel is only
	 * pointing out: a channel variable of a type no Write* overload covers, or a bound context field
	 * whose checkbox is clear. Those ship.
	 *
	 * Engine tokens rather than the literal amber all of these used to share, which said "something is
	 * off here" in one voice for two quite different weights of claim — and left an author no way to
	 * tell, from the panel alone, which of them would stop a build.
	 */

	/** Raw read of the writer being edited (the first instance — the bind menu only needs its EventDataType). */
	static const FNDCBinder* GetWriterData(const TSharedRef<IPropertyHandle>& StructHandle)
	{
		void* RawData = nullptr;
		return StructHandle->GetValueData(RawData) == FPropertyAccess::Success
			? static_cast<const FNDCBinder*>(RawData)
			: nullptr;
	}

	/**
	 * The class to resolve bound function names against while editing.
	 *
	 * A function added to a Blueprint exists on its skeleton class immediately, but does not reach the
	 * generated class until the Blueprint is compiled. Resolving against the generated class alone
	 * reports a function created seconds ago as missing — so prefer the skeleton, which is what the
	 * editor itself uses to answer "what does this class have". The runtime side deliberately does not
	 * do this: there, only the compiled class exists.
	 */
	static const UClass* GetLookupClass(const UObject* Object)
	{
		if (!Object)
		{
			return nullptr;
		}
		const UClass* Class = Object->GetClass();
		if (const UBlueprint* Blueprint = Cast<UBlueprint>(Class->ClassGeneratedBy))
		{
			if (Blueprint->SkeletonGeneratedClass)
			{
				return Blueprint->SkeletonGeneratedClass;
			}
		}
		return Class;
	}

	/** Shape of the bound-function signature this writer accepts, for the bind tooltips. */
	static FText GetEventDataParamText(const TSharedRef<IPropertyHandle>& StructHandle)
	{
		const FNDCBinder* Writer = GetWriterData(StructHandle);
		if (!Writer || !Writer->GetEventDataType())
		{
			return LOCTEXT("NoEventDataParam", "T Func()");
		}
		return FText::Format(
			LOCTEXT("EventDataParamFmt", "T Func() or T Func(F{0} EventData)"),
			FText::FromString(Writer->GetEventDataType()->GetName()));
	}

	/**
	 * The struct a function takes as its event data, or null when it takes none and when it is not
	 * shaped like a binding at all — the three callers below treat those the same, because neither
	 * is fixed by declaring an Event Data Type.
	 *
	 * The walk itself belongs to the runtime, which has to agree with it: this used to be a second
	 * copy, it drifted from the signature it was mirroring, and the diagnostics built on it went
	 * quietly dead.
	 */
	static UScriptStruct* GetDeclaredEventDataStruct(const UFunction* Func)
	{
		UScriptStruct* Declared = nullptr;
		FNDCBinder::GetBoundFunctionEventDataStruct(Func, Declared);
		return Declared;
	}

	/**
	 * How many functions *this* binding could take if the writer declared an Event Data Type.
	 *
	 * The count has to go through the binding's own test, not just count two-parameter functions on
	 * the class: a bool row would otherwise be told about three functions returning vectors that
	 * could never bind to it, which is worse than saying nothing. Each candidate is offered to that
	 * test with the writer pretending to declare the struct the candidate itself takes — the single
	 * edit that would make it bindable.
	 *
	 * Zero when an EventDataType is already set: a rejection then has some other cause.
	 */
	//~ Defined with the other context-field predicates, far below; the banner has to ask the same
	//~ question the bind menu does, or it offers to fix a function the menu would still not list.
	static bool AcceptsForField(const FNDCBinder& Writer, const UFunction* Func, const FProperty* Field);

	int32 CountFunctionsNeedingEventDataType(const FNDCBinder& Writer, const UClass* LookupClass, const TFunction<bool(const FNDCBinder&, const UFunction*)>& Accepts)
	{
		if (!LookupClass || Writer.GetEventDataType())
		{
			return 0;
		}

		FNDCBinder Hypothetical = Writer;

		int32 Count = 0;
		for (TFieldIterator<UFunction> FuncIt(LookupClass, EFieldIteratorFlags::IncludeSuper, EFieldIteratorFlags::ExcludeDeprecated, EFieldIteratorFlags::IncludeInterfaces); FuncIt; ++FuncIt)
		{
			if (UScriptStruct* DeclaredStruct = GetDeclaredEventDataStruct(*FuncIt))
			{
				Hypothetical.SetEventDataTypeUnchecked(DeclaredStruct);
				Count += Accepts(Hypothetical, *FuncIt) ? 1 : 0;
			}
		}
		return Count;
	}

	/** The first class being edited through this handle, which is representative enough for a hint. */
	static const UClass* GetEditedLookupClass(const TSharedRef<IPropertyHandle>& StructHandle)
	{
		TArray<UObject*> OuterObjects;
		StructHandle->GetOuterObjects(OuterObjects);
		for (UObject* Obj : OuterObjects)
		{
			if (Obj)
			{
				return GetLookupClass(Obj);
			}
		}
		return nullptr;
	}

	//~ The same question, asked about whatever the panel is editing. The answer itself takes a class and
	//~ a writer and nothing else, which is what makes it testable without a details panel to hang it on.
	static int32 CountFunctionsNeedingEventDataType(const TSharedRef<IPropertyHandle>& StructHandle, const TFunction<bool(const FNDCBinder&, const UFunction*)>& Accepts)
	{
		const FNDCBinder* Writer = GetWriterData(StructHandle);
		return Writer ? CountFunctionsNeedingEventDataType(*Writer, GetEditedLookupClass(StructHandle), Accepts) : 0;
	}

	/**
	 * Scans every binding on the writer for functions that would work if Event Data Type were the
	 * struct they actually take. Returns that struct and how many bindings want it, so the one edit
	 * that revives them can be named once instead of being inferred from a row of orange markers.
	 */
	UScriptStruct* FindExpectedEventDataType(const FNDCBinder& InWriter, const UClass* OwnerClass, int32& OutNumAffected)
	{
		OutNumAffected = 0;

		if (!OwnerClass)
		{
			return nullptr;
		}
		const FNDCBinder* Writer = &InWriter;

		UScriptStruct* Expected = nullptr;
		FNDCBinder Hypothetical = *Writer;

		// Every bound name on the writer, each tested with the validator that actually governs it.
		auto Consider = [&](FName FuncName, const TFunction<bool(const FNDCBinder&, const UFunction*)>& Accepts)
		{
			if (FuncName.IsNone())
			{
				return;
			}
			const UFunction* Func = OwnerClass->FindFunctionByName(FuncName);
			if (!Func || Accepts(*Writer, Func))
			{
				return;
			}
			UScriptStruct* DeclaredStruct = GetDeclaredEventDataStruct(Func);
			if (!DeclaredStruct)
			{
				return;
			}
			Hypothetical.SetEventDataTypeUnchecked(DeclaredStruct);
			if (Accepts(Hypothetical, Func))
			{
				++OutNumAffected;
				Expected = Expected ? Expected : DeclaredStruct;
			}
		};

		const UScriptStruct* ContextType = Writer->GetContextType();
		for (const FNDCContextBinding& Binding : Writer->GetContextBindings())
		{
			if (Binding.Source != ENDCValueSource::Function)
			{
				continue;
			}
			if (const FProperty* Field = ContextType ? ContextType->FindPropertyByName(Binding.FieldName) : nullptr)
			{
				Consider(Binding.BoundFunction,
					[Field](const FNDCBinder& W, const UFunction* F) { return AcceptsForField(W, F, Field); });
			}
		}

		for (const FNDCVariableBinding& Binding : Writer->GetBindings())
		{
			if (Binding.Source == ENDCValueSource::Function)
			{
				const ENDCVariableType ValueType = Binding.Type;
				const UEnum* ValueEnum = Binding.EnumDef;
				Consider(Binding.BoundFunction,
					[ValueType, ValueEnum](const FNDCBinder& W, const UFunction* F) { return W.IsValidValueFunction(F, ValueType, ValueEnum); });
			}
		}

		return Expected;
	}

	//~ The same, asked about whatever the panel is editing.
	static UScriptStruct* FindExpectedEventDataType(const TSharedRef<IPropertyHandle>& StructHandle, int32& OutNumAffected)
	{
		OutNumAffected = 0;
		const FNDCBinder* Writer = GetWriterData(StructHandle);
		return Writer
			? FindExpectedEventDataType(*Writer, GetEditedLookupClass(StructHandle), OutNumAffected)
			: nullptr;
	}

	/** The Blueprint whose defaults are being edited, or null when this is a native class. */
	static UBlueprint* GetEditedBlueprint(const TSharedRef<IPropertyHandle>& StructHandle)
	{
		TArray<UObject*> OuterObjects;
		StructHandle->GetOuterObjects(OuterObjects);
		for (UObject* Obj : OuterObjects)
		{
			if (Obj)
			{
				return Cast<UBlueprint>(Obj->GetClass()->ClassGeneratedBy);
			}
		}
		return nullptr;
	}

	/** Pin type a bound function must return for one channel variable type. Invalid for Unsupported. */
	FEdGraphPinType GetValueReturnPinType(ENDCVariableType Type, UEnum* EnumDef)
	{
		FEdGraphPinType PinType;
		switch (Type)
		{
		case ENDCVariableType::Bool:
			PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			break;
		case ENDCVariableType::Int32:
			PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
			break;
		case ENDCVariableType::Float:
			PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
			break;
		case ENDCVariableType::Enum:
			// A byte pin carrying the channel's enum, so the graph shows named values rather than a number.
			PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
			PinType.PinSubCategoryObject = EnumDef;
			break;
		default:
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				switch (Type)
				{
				case ENDCVariableType::Vector2D:    PinType.PinSubCategoryObject = TBaseStructure<FVector2D>::Get(); break;
				case ENDCVariableType::Vector:
				case ENDCVariableType::Position:    PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get(); break;
				case ENDCVariableType::Vector4:     PinType.PinSubCategoryObject = TBaseStructure<FVector4>::Get(); break;
				case ENDCVariableType::Quat:        PinType.PinSubCategoryObject = TBaseStructure<FQuat>::Get(); break;
				case ENDCVariableType::LinearColor: PinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get(); break;
				case ENDCVariableType::SpawnInfo:   PinType.PinSubCategoryObject = FNiagaraSpawnInfo::StaticStruct(); break;
				case ENDCVariableType::ID:          PinType.PinSubCategoryObject = FNiagaraID::StaticStruct(); break;
				default:                         PinType.PinCategory = NAME_None; break;
				}
				break;
			}
		}
		return PinType;
	}

	/**
	 * The event data parameter of a generated getter: a CONST REFERENCE, never a bare struct pin.
	 *
	 * This is a performance decision made where it is invisible, so it is written down here rather
	 * than left to the two flags. A Blueprint parameter is passed by value unless its pin says
	 * otherwise (KismetCompiler only adds the reference flags for arrays and for pins marked
	 * bIsReference), and a by-value event data struct is DEEP COPIED into the parameter block on every
	 * single call — for FGameplayCueParameters that is two tag containers and a shared pointer, per
	 * bound row, per write. With both flags the compiler emits `const FEventData&`
	 * (CPF_ReferenceParm | CPF_OutParm | CPF_ConstParm), which FNDCBinder's call path aliases
	 * instead of copying.
	 *
	 * Both flags or neither. bIsReference alone is a mutable reference, which
	 * FNDCBinder::GetBoundFunctionEventDataStruct refuses outright — the generated function
	 * would not be bindable at all. That is also why the panel cannot leave this to the author: the
	 * Blueprint UI offers a "Pass-by-Reference" checkbox and no way to say const, so the only shape
	 * the write path wants is the one shape a designer cannot author.
	 *
	 * NDCBinder.Editor.GeneratedBindingSignature compiles a function from this and asserts the
	 * parameter comes out ByConstReference.
	 */
	FEdGraphPinType MakeEventDataPinType(UScriptStruct* EventDataType)
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = EventDataType;
		PinType.bIsReference = true;
		PinType.bIsConst = true;
		return PinType;
	}

	/**
	 * Adds the function graph a binding needs — the signature and nothing else — and returns its name.
	 *
	 * Split out from the panel action below so the test can build one without a details panel to hang
	 * it on. What that buys is a test of the real generator rather than of a second copy of it: the
	 * pin flags this lands on the event data parameter are a performance decision (see
	 * MakeEventDataPinType) that nothing downstream would complain about if it were quietly reverted.
	 *
	 * EventDataType may be null, which is the `T Func()` shape. Returns the graph, whose name is the
	 * function's — null when the entry node the whole signature hangs on did not come out.
	 */
	UEdGraph* AddBindingFunctionGraph(UBlueprint* Blueprint, const FString& SuggestedName, const FEdGraphPinType& ReturnPinType, UScriptStruct* EventDataType, const FEdGraphPinType* EventDataPinTypeOverride)
	{
		const FName FunctionName = FBlueprintEditorUtils::FindUniqueKismetName(Blueprint, SuggestedName);
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FunctionName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, /*bIsUserCreated=*/ true, /*SignatureFromClass=*/ nullptr);

		UK2Node_FunctionEntry* EntryNode = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			EntryNode = Cast<UK2Node_FunctionEntry>(Node);
			if (EntryNode)
			{
				break;
			}
		}
		if (!EntryNode)
		{
			return nullptr;
		}

		// A binding is a read, and FNDCBinder::GetBindingRejection enforces that at write time,
		// so the function this creates has to be born passing it. Pure is what a Blueprint author would
		// tick by hand; const is its C++ half, and both together are what the engine stamps on its own
		// generated binding functions (MVVMConversionFunctionHelper). Reconstructing rebuilds the pins
		// for the pure shape — a pure entry has no exec output — before any user pin is added.
		EntryNode->Modify();
		EntryNode->AddExtraFlags(FUNC_BlueprintPure | FUNC_Const);
		EntryNode->ReconstructNode();

		// Inputs. On the entry node a function input is an output pin: the value flows out of the entry
		// into the body. The override is the test's, to build the shapes this deliberately does not.
		if (EventDataType)
		{
			EntryNode->CreateUserDefinedPin(TEXT("EventData"),
				EventDataPinTypeOverride ? *EventDataPinTypeOverride : MakeEventDataPinType(EventDataType),
				EGPD_Output);
		}

		// A result node, wired to the entry so the body has somewhere to go.
		FGraphNodeCreator<UK2Node_FunctionResult> ResultCreator(*Graph);
		UK2Node_FunctionResult* ResultNode = ResultCreator.CreateNode();
		ResultNode->FunctionReference = EntryNode->FunctionReference;
		ResultNode->NodePosX = EntryNode->NodePosX + EntryNode->NodeWidth + 256;
		ResultNode->NodePosY = EntryNode->NodePosY;
		ResultCreator.Finalize();
		ResultNode->CreateUserDefinedPin(UEdGraphSchema_K2::PN_ReturnValue, ReturnPinType, EGPD_Input);

		if (UEdGraphPin* EntryExec = EntryNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output))
		{
			if (UEdGraphPin* ResultExec = ResultNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input))
			{
				EntryExec->MakeLinkTo(ResultExec);
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		return Graph;
	}

	/**
	 * Writes a Blueprint function with exactly the signature this binding needs, binds it and opens it.
	 *
	 * The signature is the part that is tedious and easy to get subtly wrong — a `double` instead of a
	 * `float` pin, the event data struct forgotten, the parameters in the other order — and getting it
	 * wrong produces a function that simply never appears in the bind menu, with nothing to explain
	 * why. Generating it removes that whole class of dead end, the same way UMG's Create Binding does.
	 */
	static void CreateAndBindFunction(TSharedRef<IPropertyHandle> StructHandle, const FString& SuggestedName, const FEdGraphPinType& ReturnPinType, TFunction<void(FName)> OnPick)
	{
		UBlueprint* Blueprint = GetEditedBlueprint(StructHandle);
		const FNDCBinder* Writer = GetWriterData(StructHandle);
		if (!Blueprint || !Writer || ReturnPinType.PinCategory.IsNone())
		{
			return;
		}

		const FScopedTransaction Transaction(LOCTEXT("CreateNDCBinding", "Create NDC Binding"));
		Blueprint->Modify();

		UEdGraph* const Graph =
			AddBindingFunctionGraph(Blueprint, SuggestedName, ReturnPinType, Writer->GetEventDataType());
		if (!Graph)
		{
			return;
		}

		// Bind before opening, so the row already shows the new name when the author comes back.
		OnPick(Graph->GetFName());

		if (GEditor)
		{
			GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Blueprint);
			FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(Graph);
		}
	}

	/** Raw read of the binding on the first edited instance (rows mirror the channel, so instances agree). */
	static const FNDCVariableBinding* GetBindingData(const TSharedRef<IPropertyHandle>& BindingHandle)
	{
		void* RawData = nullptr;
		return BindingHandle->GetValueData(RawData) == FPropertyAccess::Success
			? static_cast<const FNDCVariableBinding*>(RawData)
			: nullptr;
	}

	/**
	 * True when this row is one of the stale rows the panel draws and the compiler fails the asset for.
	 *
	 * Both halves are the validator's own, asked through the same two functions rather than restated
	 * here: a stale row holding nothing does not survive the sync above, so marking one would be
	 * promising a compile error that never comes. What this marks is exactly what ValidateBindings
	 * reports.
	 */
	static bool IsStaleAndDrawn(const FNDCVariableBinding& Row, const TSet<FName>& ChannelVarNames)
	{
		return FNDCBinder::IsBindingStale(Row, ChannelVarNames) && Row.HasAuthoredContent();
	}

	/**
	 * Gives a payload row the Copy and Paste its context menu offers but cannot perform.
	 *
	 * Those entries belong to the property row this deliberately is not — a row per channel variable,
	 * drawn from the array by hand — so without this the menu shows them greyed out with nothing
	 * behind them. FDetailWidgetRow::CopyAction/PasteAction is the supported hook for that, the one
	 * FMatrixStructCustomization binds its composed rows with.
	 *
	 * What travels is the WHOLE row — its source, what it is bound to, and every constant on it — so a
	 * getter moves between rows with the value it falls back to, rather than a bare number moving on
	 * its own.
	 *
	 * BOTH actions are bound even where paste is refused, because the menu appears only when both are
	 * (FDetailWidgetRow::IsCopyPasteBound). Paste declines through its CanExecute instead, which
	 * leaves Copy working where it is most wanted: on a stale row, which is one somebody may want to
	 * read the value out of before pressing the bin.
	 */
	static void BindRowCopyPaste(FDetailWidgetRow& Row, const TSharedRef<IPropertyHandle>& BindingHandle, FName VarName, ENDCVariableType Type, UEnum* EnumDef, bool bPasteAllowed)
	{
		Row.CopyAction(FUIAction(FExecuteAction::CreateLambda([BindingHandle]()
		{
			FString Value;
			if (BindingHandle->GetValueAsFormattedString(Value) == FPropertyAccess::Success)
			{
				FPlatformApplicationMisc::ClipboardCopy(*Value);
			}
		})));

		Row.PasteAction(FUIAction(
			FExecuteAction::CreateLambda([BindingHandle, VarName, Type, EnumDef]()
			{
				FString Clipboard;
				FPlatformApplicationMisc::ClipboardPaste(Clipboard);

				FString Repaired;
				if (MakePastedRowText(Clipboard, VarName, Type, EnumDef, Repaired))
				{
					BindingHandle->SetValueFromFormattedString(Repaired);
				}
			}),
			FCanExecuteAction::CreateLambda([bPasteAllowed]() { return bPasteAllowed; })));
	}

	/**
	 * The row that a paste should write, or false when the clipboard is not one.
	 *
	 * Parsed into a row of our own first, because the clipboard holds whatever was last copied
	 * anywhere on the machine: text that is not a binding has to leave the target alone rather than
	 * half-written, and ImportText answering null is how that is known.
	 *
	 * The repair is the point. VarName, Type and EnumDef are the CHANNEL's, synced onto the row and
	 * never authored, so a paste that carried them would rename one row to another — leaving the array
	 * with two of one name and none of the other for the next sync to argue with. Handing the incoming
	 * row the target's identity back, before anything is written, keeps the paste to one edit with no
	 * moment in between where the row is called something else.
	 *
	 * Pasting across types is allowed and lands on its feet: the constant read at write time is
	 * whichever field the TARGET's type names, and a binding that no longer returns the right thing is
	 * exactly what the validator reports and the panel marks red. Refusing it here would be a second,
	 * quieter opinion about what a usable row is.
	 */
	bool MakePastedRowText(const FString& Clipboard, FName VarName, ENDCVariableType Type, UEnum* EnumDef, FString& OutText)
	{
		if (Clipboard.IsEmpty())
		{
			return false;
		}

		// Errors swallowed rather than logged: text that is not a binding is the ordinary case for a
		// clipboard, not a fault, and the paste doing nothing is the whole of what needs saying. GLog
		// here would also put an Error in the output of every automation run that covers this.
		FOutputDeviceNull Discard;

		FNDCVariableBinding Incoming;
		if (!FNDCVariableBinding::StaticStruct()->ImportText(*Clipboard, &Incoming, nullptr, PPF_None, &Discard, FNDCVariableBinding::StaticStruct()->GetName()))
		{
			return false;
		}

		Incoming.VarName = VarName;
		Incoming.Type = Type;
		Incoming.EnumDef = EnumDef;

		FNDCVariableBinding::StaticStruct()->ExportText(OutText, &Incoming, nullptr, nullptr, PPF_None, nullptr);
		return true;
	}

	/** True when something is bound, so the row's constant is not what goes out. */
	static bool IsSourceBound(const TSharedRef<IPropertyHandle>& SourceHandle)
	{
		void* RawData = nullptr;
		return SourceHandle->GetValueData(RawData) == FPropertyAccess::Success
			&& *static_cast<ENDCValueSource*>(RawData) != ENDCValueSource::Constant;
	}

	/** Reads an FName property value (NAME_None on failure). */
	static FName GetNameValue(const TSharedRef<IPropertyHandle>& NameHandle)
	{
		FName Value;
		NameHandle->GetValue(Value);
		return Value;
	}


	/**
	 * Reads and writes that pair across the three handles a row stores it in.
	 *
	 * The two names are kept in separate properties so switching a row from a function to a field and
	 * back does not lose what it was bound to, which means the source enum is the only thing that
	 * decides which of them is live. These two functions are the only place that knows that, so the
	 * three handles cannot be left disagreeing.
	 */
	static FBoundTo ReadBoundTo(const TSharedRef<IPropertyHandle>& SourceHandle, const TSharedRef<IPropertyHandle>& FunctionHandle, const TSharedRef<IPropertyHandle>& FieldHandle)
	{
		void* RawData = nullptr;
		if (SourceHandle->GetValueData(RawData) != FPropertyAccess::Success)
		{
			return FBoundTo{};
		}
		const ENDCValueSource Source = *static_cast<ENDCValueSource*>(RawData);
		switch (Source)
		{
		case ENDCValueSource::Function:  return FBoundTo{ Source, GetNameValue(FunctionHandle) };
		case ENDCValueSource::EventData: return FBoundTo{ Source, GetNameValue(FieldHandle) };
		default:                         return FBoundTo{};
		}
	}

	static void WriteBoundTo(const TSharedRef<IPropertyHandle>& SourceHandle, const TSharedRef<IPropertyHandle>& FunctionHandle, const TSharedRef<IPropertyHandle>& FieldHandle, FBoundTo Bound)
	{
		if (Bound.Source == ENDCValueSource::Function)
		{
			FunctionHandle->SetValueFromFormattedString(*Bound.Name.ToString());
		}
		else if (Bound.Source == ENDCValueSource::EventData)
		{
			FieldHandle->SetValueFromFormattedString(*Bound.Name.ToString());
		}
		SourceHandle->SetValueFromFormattedString(
			Bound.Source == ENDCValueSource::Function  ? TEXT("Function") :
			Bound.Source == ENDCValueSource::EventData ? TEXT("EventData") :
			TEXT("Constant"));
	}

	/**
	 * Menu-entry widget for one bindable event data field: the same layout as a function entry, with
	 * the property's own pin icon and no ƒ glyph — which is the whole visual difference between
	 * "read this member" and "call this".
	 */
	static TSharedRef<SWidget> MakeEventDataEntryWidget(const FProperty* Field)
	{
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

		FEdGraphPinType PinType;
		const bool bHasPinType = Schema->ConvertPropertyToPinType(Field, PinType);

		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SSpacer)
				.Size(FVector2D(18.f, 0.f))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(1.f, 0.f)
			[
				SNew(SImage)
				.Image(bHasPinType ? FBlueprintEditorUtils::GetIconFromPin(PinType, true) : FAppStyle::GetBrush("Icons.Error"))
				.ColorAndOpacity(bHasPinType ? Schema->GetPinTypeColor(PinType) : FSlateColor::UseForeground())
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.f, 0.f)
			[
				SNew(STextBlock)
				.Text(Field->GetDisplayNameText())
			];
	}

	/**
	 * Menu-entry widget for one bindable function, mirroring the engine's SPropertyBinding menu:
	 * pin-type icon colored by the K2 schema (the "property type"), the function name and the ƒ glyph.
	 */
	static TSharedRef<SWidget> MakeFunctionEntryWidget(const UFunction* Func)
	{
		static FName FunctionIcon(TEXT("GraphEditor.Function_16x"));
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

		FEdGraphPinType PinType;
		const bool bHasReturnType = Func->GetReturnProperty() != nullptr;
		if (bHasReturnType)
		{
			Schema->ConvertPropertyToPinType(Func->GetReturnProperty(), PinType);
		}

		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SSpacer)
				.Size(FVector2D(18.f, 0.f))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(1.f, 0.f)
			[
				SNew(SImage)
				.Image(bHasReturnType ? FBlueprintEditorUtils::GetIconFromPin(PinType, true) : FAppStyle::GetBrush("Icons.Error"))
				.ColorAndOpacity(bHasReturnType ? Schema->GetPinTypeColor(PinType) : FSlateColor::UseForeground())
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.f, 0.f)
			[
				SNew(STextBlock)
				.Text(Func->GetDisplayNameText())
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			.HAlign(HAlign_Right)
			.VAlign(VAlign_Center)
			.Padding(2.f, 0.f)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush(FunctionIcon))
			];
	}

	/** The dotted path a binding chain names — exactly what the row stores in BoundEventDataField. */
	static FName MakeEventDataPath(TConstArrayView<FBindingChainElement> Chain)
	{
		FString Path;
		for (const FBindingChainElement& Element : Chain)
		{
			if (!Path.IsEmpty())
			{
				Path.AppendChar(TEXT('.'));
			}
			Path += Element.Field.GetName();
		}
		return Path.IsEmpty() ? NAME_None : FName(*Path);
	}

	/**
	 * How many segments a path ending at this property would have.
	 *
	 * Counted rather than assumed: the menu asks about a candidate with it already on the chain in one
	 * branch and without it in another, and guessing wrong shifts the depth limit by one.
	 */
	static int32 EventDataPathDepth(const FProperty* Property, TConstArrayView<FBindingChainElement> Chain)
	{
		const bool bChainEndsWithIt = Chain.Num() > 0
			&& Chain.Last().Field.ToField() == static_cast<const FField*>(Property);
		return Chain.Num() + (bChainEndsWithIt ? 0 : 1);
	}

	/**
	 * The event data's members as a menu, nested paths and all.
	 *
	 * The submenu walk is the engine's: IPropertyAccessEditor::FillPropertyMenu is the same call that
	 * draws StateTree's binding menu (its FStateTreeBindingExtension inherits FPropertyBindingExtension,
	 * which goes through here). The interface lives in UnrealEd, which this module already depends on,
	 * and its implementation is the PropertyAccessEditor plugin — on by default, not beta, and
	 * UncookedOnly, so none of it can reach a cooked build. A project that switches that plugin off gets
	 * the flat list of direct members below, which is what this menu offered before paths existed.
	 *
	 * What the engine's walk adds beyond nesting: it prunes a submenu with no bindable leaf under it, so
	 * a struct never opens onto nothing. What it does differently: it also skips AdvancedDisplay members.
	 * Nothing used as event data here has one, and a member hidden from the details panel is a strange
	 * thing to bind, so that difference is left as the engine has it.
	 */
	void AddEventDataEntries(
		FMenuBuilder& MenuBuilder,
		const FNDCBinder& Writer,
		const TFunction<bool(const FNDCBinder&, const FProperty*)>& AcceptsField,
		const TFunction<void(FBoundTo)>& OnPick)
	{
		UScriptStruct* const EventDataType = Writer.GetEventDataType();
		if (!EventDataType)
		{
			return;
		}

		if (!IModularFeatures::Get().IsModularFeatureAvailable("PropertyAccessEditor"))
		{
			TArray<FProperty*> Fields;
			Writer.ForEachEventDataField([&Fields, &Writer, &AcceptsField](FProperty& Field)
			{
				if (AcceptsField(Writer, &Field))
				{
					Fields.Add(&Field);
				}
			});
			if (Fields.Num() == 0)
			{
				return;
			}
			MenuBuilder.BeginSection("EventDataFields", EventDataType->GetDisplayNameText());
			for (const FProperty* Field : Fields)
			{
				const FName FieldName = Field->GetFName();
				MenuBuilder.AddMenuEntry(
					FUIAction(FExecuteAction::CreateLambda([OnPick, FieldName]()
					{
						OnPick(FBoundTo{ ENDCValueSource::EventData, FieldName });
					})),
					MakeEventDataEntryWidget(Field));
			}
			MenuBuilder.EndSection();
			return;
		}

		IPropertyAccessEditor& PropertyAccessEditor =
			IModularFeatures::Get().GetModularFeature<IPropertyAccessEditor>("PropertyAccessEditor");

		// By value, and that matters: FillPropertyMenu copies these args into the delegate it builds for
		// each submenu, and those run when the user expands one — after this call has returned. A
		// captured reference would point into property data the panel is free to rebuild by then.
		const FNDCBinder WriterCopy = Writer;

		FPropertyBindingWidgetArgs Args;
		// This menu builds its own Functions section, from the owning class rather than from the event
		// data, so the engine's is switched off rather than merged into it.
		Args.bAllowFunctionBindings = false;
		Args.bAllowFunctionLibraryBindings = false;
		Args.bAllowStructFunctions = false;
		Args.bAllowUObjectFunctions = false;
		Args.bAllowNewBindings = false;
		// A bound path is a fixed sum of member offsets: struct members yes, array elements no — the
		// path carries no index to pick one with.
		Args.bAllowPropertyBindings = true;
		Args.bAllowStructMemberBindings = true;
		Args.bAllowArrayElementBindings = false;

		// Not optional, and not a filter to leave out when you want no filtering: SPropertyBinding's
		// IsClassDenied answers "denied" for EVERYTHING while this is unbound, and the walk it gates
		// breaks out of the property loop rather than skipping one — so an unset delegate here empties
		// the menu instead of widening it. It is also asked with the owning CLASS of each property,
		// which is null for a member of a struct, so null has to be allowed too.
		Args.OnCanBindToClass = FOnCanBindToClass::CreateLambda([](UClass*) { return true; });

		Args.OnCanBindPropertyWithBindingChain = FOnCanBindPropertyWithBindingChain::CreateLambda(
			[WriterCopy, AcceptsField](FProperty* Property, TConstArrayView<FBindingChainElement> Chain)
			{
				// A null property is the menu asking whether it has a destination worth opening for at
				// all, not asking about a member. Answering no there hides the whole section.
				if (!Property)
				{
					return true;
				}
				return EventDataPathDepth(Property, Chain) <= FNDCBinder::MaxFieldPathDepth
					&& AcceptsField(WriterCopy, Property);
			});

		Args.OnCanAcceptPropertyOrChildrenWithBindingChain = FOnCanAcceptPropertyOrChildrenWithBindingChain::CreateLambda(
			[WriterCopy, AcceptsField](FProperty* Property, TConstArrayView<FBindingChainElement> Chain)
			{
				if (!Property)
				{
					return true;
				}
				// False here discards the property AND everything under it.
				const int32 Depth = EventDataPathDepth(Property, Chain);
				if (Depth > FNDCBinder::MaxFieldPathDepth)
				{
					return false;
				}
				if (AcceptsField(WriterCopy, Property))
				{
					return true;
				}
				// Not bindable itself, so worth showing only if something inside it might be — and only
				// a struct can be gone into, for the reason ResolveFieldPath gives.
				return CastField<FStructProperty>(Property) != nullptr
					&& Depth < FNDCBinder::MaxFieldPathDepth;
			});

		// Inverted, despite the name: SPropertyBinding SKIPS a subobject when this answers true. An
		// object member may well be bindable as a leaf — a context field takes one — but the path stops
		// there rather than dereferencing it on every write.
		Args.OnCanBindToSubObjectClass = FOnCanBindToSubObjectClass::CreateLambda([](UClass*) { return true; });

		Args.OnAddBinding = FOnAddBinding::CreateLambda(
			[OnPick](FName, const TArray<FBindingChainElement>& Chain)
			{
				const FName Path = MakeEventDataPath(Chain);
				if (!Path.IsNone())
				{
					OnPick(FBoundTo{ ENDCValueSource::EventData, Path });
				}
			});

		// Nothing of ours around this call. FillPropertyMenu opens its own "Properties" section, and
		// FMenuBuilder::BeginSection asserts on a section inside a section — which is what wrapping it
		// in one headed with the struct's name did.
		//
		// A heading widget instead of a section would have its own bug: when the struct has nothing
		// bindable in it FillPropertyMenu adds no entries at all, and the heading would stand over an
		// empty stretch of menu. (Its own "None" placeholder does not help here — that only appears when
		// the whole builder is otherwise empty, and ours already holds the Bindings section.) So the
		// struct's name is given up and the engine's own header stands, the same one its binding menus
		// show everywhere else.
		PropertyAccessEditor.FillPropertyMenu(MenuBuilder, Args, NAME_None, EventDataType, {});
	}

	/** Shared bind-menu: function entries of the owning class the binding accepts, plus "Clear Binding" when bound. */
	static TSharedRef<SWidget> BuildFunctionMenuWidget(TSharedRef<IPropertyHandle> StructHandle, TFunction<bool(const FNDCBinder&, const UFunction*)> Accepts, TFunction<bool(const FNDCBinder&, const FProperty*)> AcceptsField, TFunction<void(FBoundTo)> OnPick, TFunction<void()> OnClear, bool bShowClear, const FString& SuggestedName, const FEdGraphPinType& ReturnPinType)
	{
		FMenuBuilder MenuBuilder(true, nullptr);

		const FNDCBinder* Writer = GetWriterData(StructHandle);

		// Actions first, matching the layout the engine's own binding widgets use: remove above create,
		// both under a Bindings heading, then the list of things you can bind to.
		MenuBuilder.BeginSection("BindingActions", LOCTEXT("BindingsSection", "Bindings"));
		{
			if (bShowClear)
			{
				MenuBuilder.AddMenuEntry(
					LOCTEXT("RemoveBinding", "Remove Binding"),
					LOCTEXT("RemoveBindingTooltip", "Drop the function binding and go back to the constant."),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "Cross"),
					FUIAction(FExecuteAction::CreateLambda([OnClear]() { OnClear(); })));
			}

			// Only a Blueprint has a graph to write into; a native class has to declare its own.
			if (GetEditedBlueprint(StructHandle) && !ReturnPinType.PinCategory.IsNone())
			{
				MenuBuilder.AddMenuEntry(
					LOCTEXT("CreateBinding", "Create Binding"),
					LOCTEXT("CreateBindingTooltip", "Add a Blueprint function with the signature this binding needs, bind it and open it for editing."),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "Plus"),
					FUIAction(FExecuteAction::CreateLambda([StructHandle, SuggestedName, ReturnPinType, OnPick]()
					{
						CreateAndBindFunction(StructHandle, SuggestedName, ReturnPinType,
							[OnPick](FName FuncName) { OnPick(FBoundTo{ ENDCValueSource::Function, FuncName }); });
					})));
			}
		}
		MenuBuilder.EndSection();

		// Compatible functions of the edited class. A multi-edit can span unrelated classes, and the
		// The event data's own fields, first: they are the cheap answer and usually the right one, and
		// a function that just returns one of them is boilerplate worth not writing. Unlike functions
		// this list does not depend on the edited class at all — the struct is the writer's, so a
		// multi-selection cannot disagree about it.
		if (AcceptsField && Writer && Writer->GetEventDataType())
		{
			AddEventDataEntries(MenuBuilder, *Writer, AcceptsField, OnPick);
		}

		// binding is stored as a bare name resolved per object, so offer the intersection by name:
		// anything every selected class implements compatibly.
		TArray<UObject*> OuterObjects;
		StructHandle->GetOuterObjects(OuterObjects);

		TArray<UFunction*> Functions;
		bool bFirstClass = true;
		for (UObject* Obj : OuterObjects)
		{
			if (!Obj)
			{
				continue;
			}

			TSet<FName> CompatibleHere;
			for (TFieldIterator<UFunction> It(GetLookupClass(Obj), EFieldIteratorFlags::IncludeSuper, EFieldIteratorFlags::ExcludeDeprecated, EFieldIteratorFlags::IncludeInterfaces); It; ++It)
			{
				if (Writer && Accepts(*Writer, *It))
				{
					CompatibleHere.Add(It->GetFName());
					if (bFirstClass)
					{
						Functions.Add(*It);
					}
				}
			}

			if (bFirstClass)
			{
				bFirstClass = false;
			}
			else
			{
				Functions.RemoveAll([&CompatibleHere](const UFunction* Func) { return !CompatibleHere.Contains(Func->GetFName()); });
			}
		}
		Functions.Sort([](const UFunction& A, const UFunction& B) { return A.GetName() < B.GetName(); });

		MenuBuilder.BeginSection("Functions", LOCTEXT("FunctionsSection", "Functions"));
		if (Functions.IsEmpty())
		{
			// The commonest reason for an empty menu is a writer with no Event Data Type: every
			// two-parameter function on the class is then rejected, silently and for one fixable
			// reason. Say so instead of leaving the author to guess.
			const int32 NumBlockedByMissingType = CountFunctionsNeedingEventDataType(StructHandle, Accepts);
			if (NumBlockedByMissingType > 0)
			{
				MenuBuilder.AddMenuEntry(
					LOCTEXT("NoEventDataTypeDeclared", "No Event Data Type declared on this writer"),
					FText::Format(
						LOCTEXT("NoEventDataTypeTooltipFmt", "{0} function(s) on this class take an event data struct, but this writer declares no Event Data Type, so none of them can be bound. Set it under Advanced > Event Data Type (a C++ owner normally sets it in its constructor)."),
						FText::AsNumber(NumBlockedByMissingType)),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Warning"),
					FUIAction());
			}
			else
			{
				MenuBuilder.AddMenuEntry(
					LOCTEXT("NoCompatibleFunctions", "No compatible functions on this class"),
					FText::Format(
						LOCTEXT("NoCompatibleFunctionsTooltipFmt", "Add a function with a matching return type: {0}."),
						GetEventDataParamText(StructHandle)),
					FSlateIcon(),
					FUIAction());
			}
		}

		for (const UFunction* Func : Functions)
		{
			MenuBuilder.AddMenuEntry(
				FUIAction(FExecuteAction::CreateLambda([OnPick, Func]() { OnPick(FBoundTo{ ENDCValueSource::Function, Func->GetFName() }); })),
				MakeFunctionEntryWidget(Func));
		}
		MenuBuilder.EndSection();

		return MenuBuilder.MakeWidget();
	}

	/** Why a stored binding is not usable, which decides where the problem is worth showing. */
	enum class EBindingProblem : uint8
	{
		/** Usable, or nothing bound. */
		None,
		/** The function is fine; the writer's Event Data Type is what does not match it. */
		EventDataType,
		/** The function itself: gone, wrong return type, wrong shape. */
		Binding,
	};

	/**
	 * Diagnoses the name a binding currently stores. A binding survives as a bare name through
	 * anything that changes what "accepted" means, so it keeps looking bound while the write silently
	 * skips it — the button has to re-test rather than trust that it was valid when it was picked.
	 *
	 * The distinction matters for where to complain. Change Event Data Type and every row that took
	 * the old struct goes bad at once; blaming each of them buries the one edit that caused it, so
	 * that case is reported against Event Data Type instead and the rows stay quiet.
	 */
	static EBindingProblem DiagnoseBinding(const TSharedRef<IPropertyHandle>& StructHandle, FBoundTo Bound, const TFunction<bool(const FNDCBinder&, const UFunction*)>& Accepts, const TFunction<bool(const FNDCBinder&, const FProperty*)>& AcceptsField)
	{
		const FNDCBinder* Writer = GetWriterData(StructHandle);
		if (!Bound.IsBound() || !Writer)
		{
			return EBindingProblem::None;
		}

		// An event data field is judged against the struct alone — no class, no signature, nothing that
		// a multi-selection could answer two ways. Either the field is still there and still fits, or it
		// is not, and Event Data Type is what changed if so.
		if (Bound.IsEventData())
		{
			const FProperty* Field = Writer->FindEventDataField(Bound.Name);
			if (!Field)
			{
				return EBindingProblem::EventDataType;
			}
			return (AcceptsField && AcceptsField(*Writer, Field)) ? EBindingProblem::None : EBindingProblem::Binding;
		}

		const FName FuncName = Bound.Name;

		TArray<UObject*> OuterObjects;
		StructHandle->GetOuterObjects(OuterObjects);

		for (UObject* Obj : OuterObjects)
		{
			if (!Obj)
			{
				continue;
			}

			const UFunction* Func = GetLookupClass(Obj)->FindFunctionByName(FuncName);
			if (!Func)
			{
				return EBindingProblem::Binding;
			}
			if (Accepts(*Writer, Func))
			{
				continue;
			}

			// Would declaring the struct the function itself takes be the whole fix?
			if (UScriptStruct* DeclaredStruct = GetDeclaredEventDataStruct(Func))
			{
				FNDCBinder Hypothetical = *Writer;
				Hypothetical.SetEventDataTypeUnchecked(DeclaredStruct);
				if (Accepts(Hypothetical, Func))
				{
					return EBindingProblem::EventDataType;
				}
			}
			return EBindingProblem::Binding;
		}
		return EBindingProblem::None;
	}

	/**
	 * Why the bound name is not usable, in one clause. The flag rules reject a function that is
	 * otherwise shaped correctly — a replicated one, an editor-only one, one that is not const — and
	 * "expected: (FGameplayCueParameters)" would be actively misleading for those, since the
	 * signature is exactly right and the problem is somewhere else entirely.
	 */
	static FText DescribeBoundFunctionRejection(const TSharedRef<IPropertyHandle>& StructHandle, FName FuncName)
	{
		TArray<UObject*> OuterObjects;
		StructHandle->GetOuterObjects(OuterObjects);
		for (UObject* Obj : OuterObjects)
		{
			if (!Obj)
			{
				continue;
			}
			const UFunction* Func = GetLookupClass(Obj)->FindFunctionByName(FuncName);
			const ENDCBindingRejection Rejection = FNDCBinder::GetBindingRejection(Func);
			if (Rejection != ENDCBindingRejection::None)
			{
				return FText::FromString(FNDCBinder::DescribeRejection(Rejection));
			}
			break; // classes agree in multi-edit — the first object decides
		}
		return FText::GetEmpty();
	}

	/**
	 * Where "go to this binding" should land inside the edited Blueprint: the function's own graph
	 * for a user function or a BlueprintNativeEvent override, or the event node for a
	 * BlueprintImplementableEvent, which lives in the event graph and has no graph of its own.
	 * Null when the implementation is not in this Blueprint at all — a plain C++ function.
	 */
	static UObject* FindBlueprintNavigationTarget(UBlueprint* Blueprint, const UFunction* Func, FName FuncName)
	{
		if (!Blueprint || FuncName.IsNone())
		{
			return nullptr;
		}

		// A user function, and a BlueprintNativeEvent override, each get a graph of their own named
		// after the function.
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetFName() == FuncName)
			{
				return Graph;
			}
		}

		// A BlueprintImplementableEvent has no graph — its implementation is a node in the event graph.
		if (Func)
		{
			if (UK2Node_Event* EventNode = FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, Func->GetOwnerClass(), FuncName))
			{
				return EventNode;
			}
		}
		return nullptr;
	}

	/** The bound function as the class resolves it now, or null. */
	static UFunction* FindBoundFunction(const TSharedRef<IPropertyHandle>& StructHandle, FName FuncName)
	{
		if (FuncName.IsNone())
		{
			return nullptr;
		}
		TArray<UObject*> OuterObjects;
		StructHandle->GetOuterObjects(OuterObjects);
		for (UObject* Obj : OuterObjects)
		{
			if (Obj)
			{
				const UClass* LookupClass = GetLookupClass(Obj);
				return LookupClass ? const_cast<UClass*>(LookupClass)->FindFunctionByName(FuncName) : nullptr;
			}
		}
		return nullptr;
	}

	/**
	 * Whether the magnifier has anywhere to go. UMG only offers this for a Blueprint graph, but the
	 * usual shape here is a BlueprintNativeEvent declared in C++ and left unoverridden, so a native
	 * function is worth offering too — the IDE is exactly where its author wants to end up.
	 */
	static bool CanGotoBoundFunction(const TSharedRef<IPropertyHandle>& StructHandle, FName FuncName)
	{
		const UFunction* Func = FindBoundFunction(StructHandle, FuncName);
		return FindBlueprintNavigationTarget(GetEditedBlueprint(StructHandle), Func, FuncName) != nullptr
			|| (Func && FSourceCodeNavigation::CanNavigateToFunction(Func));
	}

	/**
	 * Opens the C++ an author actually wrote, which is not the symbol the UFunction names.
	 *
	 * For a BlueprintNativeEvent — the shape almost every binding here has — UHT generates
	 * `UClass::FuncName` as a dispatcher inside the .gen.cpp and leaves `UClass::FuncName_Implementation`
	 * for the author. FSourceCodeNavigation::NavigateToFunction builds the symbol from the UFunction's
	 * own name, so it lands in generated code every time. Asking for the _Implementation symbol
	 * instead lands on the line the author wrote.
	 */
	static bool NavigateToBoundFunctionSource(UFunction* Func)
	{
		if (!Func || !FSourceCodeNavigation::CanNavigateToFunction(Func))
		{
			return false;
		}

		UClass* OwnerClass = Func->GetOwnerClass();
		FString ModuleName;
		if (Func->HasAllFunctionFlags(FUNC_Native | FUNC_Event)
			&& FSourceCodeNavigation::FindClassModuleName(OwnerClass, ModuleName))
		{
			const FString SymbolName = FString::Printf(TEXT("%s%s::%s_Implementation"),
				OwnerClass->GetPrefixCPP(), *OwnerClass->GetName(), *Func->GetName());
			FSourceCodeNavigation::NavigateToFunctionSourceAsync(SymbolName, ModuleName, /*bIgnoreLineNumber=*/ false);
			return true;
		}

		// A plain native function is defined under its own name, so the engine's own lookup is right.
		if (Func->IsNative() && !Func->HasAnyFunctionFlags(FUNC_Event) && FSourceCodeNavigation::NavigateToFunction(Func))
		{
			return true;
		}

		// Left: a BlueprintImplementableEvent, which has no C++ body at all. The header it is declared
		// in is as close as source gets — the same fallback UK2Node_CallFunction::JumpToDefinition uses.
		FString HeaderPath;
		if (FSourceCodeNavigation::FindClassHeaderPath(Func, HeaderPath)
			&& IFileManager::Get().FileSize(*HeaderPath) != INDEX_NONE)
		{
			FSourceCodeNavigation::OpenSourceFileAsync(FPaths::ConvertRelativePathToFull(HeaderPath));
			return true;
		}
		return false;
	}

	/**
	 * Whether the magnifier has anywhere to go, answered once per bound name.
	 *
	 * A details row repaints every frame and visibility is asked every repaint, but neither the
	 * Blueprint's graphs nor whether a C++ function is reachable changes between two frames — and
	 * anything that does change either rebinds the row or recompiles the Blueprint, and both rebuild
	 * this widget. So the search stays off the paint path entirely.
	 */
	struct FGotoTargetCache
	{
		FName ResolvedFor = NAME_None;
		bool bResolved = false;
		bool bCanGoto = false;

		bool CanGoto(const TSharedRef<IPropertyHandle>& StructHandle, FName FuncName)
		{
			if (!bResolved || ResolvedFor != FuncName)
			{
				ResolvedFor = FuncName;
				bResolved = true;
				bCanGoto = !FuncName.IsNone() && CanGotoBoundFunction(StructHandle, FuncName);
			}
			return bCanGoto;
		}
	};

	/** Opens the Blueprint graph the binding lives in, or the C++ declaration when it has none. */
	static FReply GotoBoundFunction(TSharedRef<IPropertyHandle> StructHandle, FName FuncName)
	{
		UBlueprint* Blueprint = GetEditedBlueprint(StructHandle);
		UFunction* Func = FindBoundFunction(StructHandle, FuncName);
		if (UObject* Target = FindBlueprintNavigationTarget(Blueprint, Func, FuncName))
		{
			if (GEditor)
			{
				GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Blueprint);
				FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(Target);
			}
			return FReply::Handled();
		}

		return NavigateToBoundFunctionSource(Func) ? FReply::Handled() : FReply::Unhandled();
	}

	/** Pin type of the bound function's return value (drives the colored type icon on the button). */
	static bool GetBoundPinType(const TSharedRef<IPropertyHandle>& StructHandle, FBoundTo Bound, FEdGraphPinType& OutPinType)
	{
		if (!Bound.IsBound())
		{
			return false;
		}
		if (Bound.IsEventData())
		{
			const FNDCBinder* Writer = GetWriterData(StructHandle);
			const FProperty* Field = Writer ? Writer->FindEventDataField(Bound.Name) : nullptr;
			return Field && GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Field, OutPinType);
		}
		const FName FuncName = Bound.Name;
		TArray<UObject*> OuterObjects;
		StructHandle->GetOuterObjects(OuterObjects);
		for (UObject* Obj : OuterObjects)
		{
			if (!Obj)
			{
				continue;
			}
			if (const UFunction* Func = GetLookupClass(Obj)->FindFunctionByName(FuncName))
			{
				if (FProperty* ReturnProp = Func->GetReturnProperty())
				{
					GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(ReturnProp, OutPinType);
					return true;
				}
			}
			break; // classes agree in multi-edit — the first object decides
		}
		return false;
	}

	/**
	 * UMG-style bind button (mirrors the engine's SPropertyBinding): a gray button with a chain
	 * icon — Unlink when unbound, Link when bound —, the bound function's return-type icon colored
	 * by the K2 schema, and the function's name; clicking opens the function menu.
	 */
	/**
	 * True when what is bound fits the field by type and is refused only by its AllowedClasses.
	 *
	 * Worth telling apart from a plain type mismatch: the two look identical in the panel and have
	 * nothing in common as fixes. One is the wrong value; the other is the right kind of value from a
	 * getter that does not promise it — SystemToSpawn takes a NiagaraSystem, and a getter declared to
	 * return UObject may well hand one back, but the row cannot know that and will not guess.
	 */
	static bool IsAssignableButDisallowed(const TSharedRef<IPropertyHandle>& StructHandle, FBoundTo Bound, const FProperty* ContextField)
	{
		const FNDCBinder* Writer = GetWriterData(StructHandle);
		if (!Writer || !ContextField || !Bound.IsBound())
		{
			return false;
		}

		const FProperty* Source = nullptr;
		if (Bound.IsEventData())
		{
			Source = Writer->FindEventDataField(Bound.Name);
		}
		else if (const UFunction* Func = FindBoundFunction(StructHandle, Bound.Name))
		{
			Source = Func->GetReturnProperty();
		}
		return FNDCBinder::CanAssignPropertyToField(Source, ContextField)
			&& !FNDCBinder::IsSourceAllowedByFieldClasses(Source, ContextField);
	}

	static TSharedRef<SWidget> MakeBindButton(
		TSharedRef<IPropertyHandle> StructHandle,
		TFunction<bool(const FNDCBinder&, const UFunction*)> Accepts,
		TFunction<bool(const FNDCBinder&, const FProperty*)> AcceptsField,
		TFunction<FBoundTo()> GetCurrent,
		TFunction<void(FBoundTo)> OnPick,
		TFunction<void()> OnClear,
		const FText& UnboundTooltip,
		const FString& SuggestedName,
		const FEdGraphPinType& ReturnPinType,
		//~ Context rows only. A payload row writes a channel variable, which has no class restriction
		//~ to explain, so it leaves this null and never reaches the branch that uses it.
		const FProperty* ContextField = nullptr)
	{
		// Re-tested on every paint rather than at pick time, so a change marks an existing binding
		// without needing a panel refresh. Both kinds of problem are marked — a row that will not run
		// should look like it — but they read differently in the tooltip: one is this binding's fault,
		// the other is the writer's Event Data Type, which the banner above already names.
		TFunction<bool()> IsBound = [GetCurrent]() { return GetCurrent().IsBound(); };
		TFunction<EBindingProblem()> GetBindingProblem = [StructHandle, Accepts, AcceptsField, GetCurrent]()
		{
			return DiagnoseBinding(StructHandle, GetCurrent(), Accepts, AcceptsField);
		};

		return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.FillWidth(1.f)
		[
			// Flat, the way the editor draws every other binding button: StateTree, UMG and Chooser
			// all reach SPropertyBinding, which uses HoverHintOnly. A framed button on every row made
			// the list a column of grey slabs, and made the binding read as an action you perform
			// rather than as the value the row currently has.
			SNew(SComboButton)
			.ButtonStyle(FAppStyle::Get(), "HoverHintOnly")
			.HasDownArrow(false)
			.ContentPadding(FMargin(2.f, 2.f))
			.ToolTipText_Lambda([StructHandle, IsBound, GetBindingProblem, GetCurrent, UnboundTooltip, ContextField]()
			{
				const FBoundTo Bound = GetCurrent();
				if (!Bound.IsBound())
				{
					return UnboundTooltip;
				}
				switch (GetBindingProblem())
				{
				case EBindingProblem::EventDataType:
					return Bound.IsEventData()
						? FText::Format(
							LOCTEXT("EventDataFieldGoneTooltipFmt", "The event data struct this writer declares has no field called {0} any more, so this row is skipped at write time. Rebind it, or fix Advanced > Event Data Type."),
							FText::FromName(Bound.Name))
						: FText::Format(
							LOCTEXT("EventDataTypeBoundTooltipFmt", "{0} takes a different event data struct than this writer declares, so this row is skipped at write time. The binding itself is fine — fix Advanced > Event Data Type, as the warning above this writer describes."),
							FText::FromName(Bound.Name));
				case EBindingProblem::Binding:
					{
						// Refused by the field's own AllowedClasses rather than by anything about the
						// binding's shape. Said first, because both messages below name something that
						// is not what went wrong — the row's type, or the writer's Event Data Type.
						TArray<const UClass*> Allowed;
						FNDCBinder::GetFieldAllowedClasses(ContextField, Allowed);
						if (!Allowed.IsEmpty() && IsAssignableButDisallowed(StructHandle, Bound, ContextField))
						{
							TArray<FString> Names;
							Names.Reserve(Allowed.Num());
							for (const UClass* Class : Allowed)
							{
								Names.Add(Class->GetName());
							}
							return FText::Format(
								LOCTEXT("DisallowedClassTooltipFmt", "{0} does not hand back one of the classes this field takes ({1}), so the row is skipped at write time. Bind it to something that returns one of those."),
								FText::FromName(Bound.Name),
								FText::FromString(FString::Join(Names, TEXT(" or "))));
						}
						if (Bound.IsEventData())
						{
							return FText::Format(
								LOCTEXT("EventDataFieldTypeTooltipFmt", "{0} is not of a type this row can take, so it is skipped at write time. Rebind it to a field of the right type."),
								FText::FromName(Bound.Name));
						}
						const FText Rejection = DescribeBoundFunctionRejection(StructHandle, Bound.Name);
						return Rejection.IsEmpty()
							? FText::Format(
								LOCTEXT("BrokenBoundTooltipFmt", "{0} no longer matches what this writer accepts, so this row is skipped at write time. Expected: {1}. Rebind it or fix the function."),
								FText::FromName(Bound.Name),
								GetEventDataParamText(StructHandle))
							: FText::Format(
								LOCTEXT("RejectedBoundTooltipFmt", "{0} cannot be used as a binding, so this row is skipped at write time: {1}. Rebind it or fix the function."),
								FText::FromName(Bound.Name),
								Rejection);
					}
				default:
					return Bound.IsEventData()
						? FText::Format(LOCTEXT("BoundFieldTooltipFmt", "Reads {0} from the event data — click to rebind or clear"), FText::FromName(Bound.Name))
						: FText::Format(LOCTEXT("BoundTooltipFmt", "Bound to {0} — click to rebind or clear"), FText::FromName(Bound.Name));
				}
			})
			.OnGetMenuContent_Lambda([StructHandle, Accepts, AcceptsField, IsBound, OnPick, OnClear, SuggestedName, ReturnPinType]() -> TSharedRef<SWidget>
			{
				return BuildFunctionMenuWidget(StructHandle, Accepts, AcceptsField, OnPick, OnClear, IsBound(), SuggestedName, ReturnPinType);
			})
			.ButtonContent()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SBox)
					.WidthOverride(16.f)
					.HeightOverride(16.f)
					[
						SNew(SImage)
						.Image_Lambda([IsBound]()
						{
							return IsBound() ? FAppStyle::GetBrush("Icons.Link") : FAppStyle::GetBrush("Icons.Unlink");
						})
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.f, 0.f, 0.f, 0.f)
				[
					SNew(SImage)
					.Visibility_Lambda([IsBound, GetCurrent]()
					{
						return IsBound() ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.Image_Lambda([StructHandle, GetCurrent, GetBindingProblem]() -> const FSlateBrush*
					{
						if (GetBindingProblem() != EBindingProblem::None)
						{
							return FAppStyle::GetBrush("Icons.Warning");
						}
						FEdGraphPinType PinType;
						return GetBoundPinType(StructHandle, GetCurrent(), PinType)
							? FBlueprintEditorUtils::GetIconFromPin(PinType, true)
							: FAppStyle::GetBrush("Icons.Error");
					})
					.ColorAndOpacity_Lambda([StructHandle, GetCurrent, GetBindingProblem]() -> FSlateColor
					{
						if (GetBindingProblem() != EBindingProblem::None)
						{
							return FStyleColors::Error;
						}
						FEdGraphPinType PinType;
						return GetBoundPinType(StructHandle, GetCurrent(), PinType)
							? GetDefault<UEdGraphSchema_K2>()->GetPinTypeColor(PinType)
							: FSlateColor::UseForeground();
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.f, 0.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Font(IDetailLayoutBuilder::GetDetailFont())
					.Text_Lambda([IsBound, GetCurrent]()
					{
						return IsBound() ? FText::FromName(GetCurrent().Name) : FText::GetEmpty();
					})
					.ColorAndOpacity_Lambda([GetBindingProblem]() -> FSlateColor
					{
						return GetBindingProblem() != EBindingProblem::None ? FStyleColors::Error : FSlateColor::UseForeground();
					})
				]
			]
		]

		// The magnifier, exactly as UMG's SPropertyBinding places it: a borderless sibling of the bind
		// button that appears only when there is somewhere to jump to.
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "HoverHintOnly")
			.VAlign(VAlign_Center)
			.ToolTipText(LOCTEXT("GotoFunction", "Goto Function"))
			.Visibility_Lambda([StructHandle, IsBound, GetCurrent, GotoCache = MakeShared<FGotoTargetCache>()]()
			{
				const FBoundTo Bound = GetCurrent();
				const FName Current = Bound.IsFunction() ? Bound.Name : NAME_None;
				return GotoCache->CanGoto(StructHandle, Current) ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.OnClicked_Lambda([StructHandle, GetCurrent]()
			{
				return GotoBoundFunction(StructHandle, GetCurrent().Name);
			})
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.Search"))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		];
	}

	//~ ---- Access context field rows -------------------------------------------------------------
	//~
	//~ The context is edited as a value, natively, and each of its input fields also carries a bind
	//~ button. Value and binding for a field are therefore the same row: there is no second list to
	//~ keep in step with the first, and no way to bind a field you cannot see.

	/** Index of the row driving FieldName, or INDEX_NONE. Rows are sparse and few — a scan is right. */
	static int32 FindContextRowIndex(const TSharedRef<IPropertyHandle>& RowsHandle, FName FieldName)
	{
		TSharedPtr<IPropertyHandleArray> Array = RowsHandle->AsArray();
		uint32 NumRows = 0;
		if (!Array.IsValid() || Array->GetNumElements(NumRows) != FPropertyAccess::Success)
		{
			return INDEX_NONE;
		}
		for (uint32 Index = 0; Index < NumRows; ++Index)
		{
			TSharedPtr<IPropertyHandle> Field = Array->GetElement(Index)->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCContextBinding, FieldName));
			FName Stored;
			if (Field.IsValid() && Field->GetValue(Stored) == FPropertyAccess::Success && Stored == FieldName)
			{
				return (int32)Index;
			}
		}
		return INDEX_NONE;
	}

	/** The row driving FieldName, or null. */
	static TSharedPtr<IPropertyHandle> FindContextRow(const TSharedRef<IPropertyHandle>& RowsHandle, FName FieldName)
	{
		const int32 Index = FindContextRowIndex(RowsHandle, FieldName);
		TSharedPtr<IPropertyHandleArray> Array = RowsHandle->AsArray();
		if (Index == INDEX_NONE || !Array.IsValid())
		{
			return nullptr;
		}
		return Array->GetElement(Index);
	}

	/** What FieldName is bound to, or nothing when it has no row. */
	static FBoundTo GetContextRowBinding(const TSharedRef<IPropertyHandle>& RowsHandle, FName FieldName)
	{
		TSharedPtr<IPropertyHandle> Row = FindContextRow(RowsHandle, FieldName);
		if (!Row.IsValid())
		{
			return FBoundTo{};
		}
		TSharedPtr<IPropertyHandle> SourceHandle = Row->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCContextBinding, Source));
		TSharedPtr<IPropertyHandle> FuncHandle = Row->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCContextBinding, BoundFunction));
		TSharedPtr<IPropertyHandle> FieldHandle = Row->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCContextBinding, BoundEventDataField));
		if (!SourceHandle.IsValid() || !FuncHandle.IsValid() || !FieldHandle.IsValid())
		{
			return FBoundTo{};
		}
		return ReadBoundTo(SourceHandle.ToSharedRef(), FuncHandle.ToSharedRef(), FieldHandle.ToSharedRef());
	}

	/** Binds FieldName, adding its row if this is the first time. */
	static void SetContextRowBinding(const TSharedRef<IPropertyHandle>& RowsHandle, const UScriptStruct* ContextType, const FProperty* Field, FBoundTo Bound)
	{
		TSharedPtr<IPropertyHandleArray> Array = RowsHandle->AsArray();
		if (!Array.IsValid() || !Field)
		{
			return;
		}
		const FName FieldName = Field->GetFName();

		int32 Index = FindContextRowIndex(RowsHandle, FieldName);
		if (Index == INDEX_NONE)
		{
			uint32 NumBefore = 0;
			Array->GetNumElements(NumBefore);
			if (Array->AddItem() != FPropertyAccess::Success)
			{
				return;
			}
			uint32 NumAfter = 0;
			Array->GetNumElements(NumAfter);
			if (NumAfter != NumBefore + 1)
			{
				return;
			}
			Index = (int32)NumBefore;

			TSharedRef<IPropertyHandle> NewRow = Array->GetElement(Index);
			if (TSharedPtr<IPropertyHandle> FieldHandle = NewRow->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCContextBinding, FieldName)))
			{
				FieldHandle->SetValue(FieldName);
			}
			// The field's EditCondition, resolved now and stored: metadata does not survive a cooked
			// build, and the write needs to know which flag to switch on.
			if (TSharedPtr<IPropertyHandle> FlagHandle = NewRow->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCContextBinding, EnableFlagField)))
			{
				FlagHandle->SetValue(FNDCBinder::ResolveContextFieldEnableFlag(ContextType, Field));
			}
		}

		TSharedRef<IPropertyHandle> Row = Array->GetElement(Index);
		TSharedPtr<IPropertyHandle> SourceHandle = Row->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCContextBinding, Source));
		TSharedPtr<IPropertyHandle> FuncHandle = Row->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCContextBinding, BoundFunction));
		TSharedPtr<IPropertyHandle> FieldHandle = Row->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCContextBinding, BoundEventDataField));
		if (SourceHandle.IsValid() && FuncHandle.IsValid() && FieldHandle.IsValid())
		{
			WriteBoundTo(SourceHandle.ToSharedRef(), FuncHandle.ToSharedRef(), FieldHandle.ToSharedRef(), Bound);
		}
	}

	/** Unbinds FieldName by removing its row entirely — an unbound field has no row at all. */
	static void ClearContextRow(const TSharedRef<IPropertyHandle>& RowsHandle, FName FieldName)
	{
		const int32 Index = FindContextRowIndex(RowsHandle, FieldName);
		TSharedPtr<IPropertyHandleArray> Array = RowsHandle->AsArray();
		if (Index != INDEX_NONE && Array.IsValid())
		{
			Array->DeleteItem(Index);
		}
	}

	/**
	 * Whether a function may drive this field, as the picker sees it.
	 *
	 * Stricter than the runtime check by exactly one thing: AllowedClasses. SystemToSpawn is declared
	 * as a bare UObject* and narrowed to Niagara systems by metadata, which is editor-only — so the
	 * write cannot enforce it and the menu can. Offering a component for it and letting the write
	 * quietly do nothing is precisely the failure this panel exists to prevent.
	 */
	static bool AcceptsForField(const FNDCBinder& Writer, const UFunction* Func, const FProperty* Field)
	{
		return Writer.IsValidContextFieldFunction(Func, Field)
			&& FNDCBinder::IsSourceAllowedByFieldClasses(Func ? Func->GetReturnProperty() : nullptr, Field);
	}

	/**
	 * Context input fields the panel does not offer.
	 *
	 * The list is a project setting rather than a list of names in this file, because a bare name is
	 * wrong in both directions: it under-hides a field of the same nature under a different name on
	 * someone else's context, and it over-hides a field that merely shares the name. Keying the rule to
	 * the context type says what was actually meant. What ships is one entry, seeded in the settings
	 * object's constructor — see UNDCBinderEditorSettings for it, and for why it is there at all.
	 *
	 * Still an escape hatch and not a classification: Niagara's metadata knows Input, Output and
	 * Transient and nothing else, so there is no rule to read. Marking such inputs at the source is what
	 * would retire the setting.
	 */
	bool IsHiddenContextField(const UScriptStruct* ContextType, const FProperty& Field)
	{
		return GetDefault<UNDCBinderEditorSettings>()->IsContextFieldHidden(ContextType, Field);
	}

	/**
	 * Whether an event data field can drive a context field. Exactly the runtime's own question, asked
	 * with no addresses so nothing is written — the same call the write makes before it stores.
	 */
	static bool AcceptsEventDataForField(const FProperty* Source, const FProperty* Field)
	{
		return FNDCBinder::CanAssignPropertyToField(Source, Field)
			&& FNDCBinder::IsSourceAllowedByFieldClasses(Source, Field);
	}

	/** Pin type a bound function must return to drive this field (drives Create Binding's signature). */
	static FEdGraphPinType GetFieldReturnPinType(const FProperty* Field)
	{
		FEdGraphPinType PinType;
		if (Field)
		{
			GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Field, PinType);
			// The generated function returns the value; the field's own by-reference-ness, if any, is
			// not part of that.
			PinType.bIsReference = false;

			// The declared type is not the wanted one where the field restricts its classes, and
			// generating a getter that returns the declared type would produce a function the bind
			// menu then refuses to list. The first allowed class is the one generated: where a field
			// names several they are unrelated types (SystemToSpawn takes a NiagaraSystem or a
			// NiagaraSystemCollection), so there is no single type that covers them and the common
			// case is the one worth writing. Retyping the generated pin to another allowed class is a
			// two-click edit, and the menu accepts the result.
			TArray<const UClass*> Allowed;
			FNDCBinder::GetFieldAllowedClasses(Field, Allowed);
			if (!Allowed.IsEmpty())
			{
				PinType.PinSubCategoryObject = const_cast<UClass*>(Allowed[0]);
			}
		}
		return PinType;
	}

}

void FNDCBinderCustomization::CustomizeHeader(TSharedRef<IPropertyHandle> StructPropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	PropertyUtilities = CustomizationUtils.GetPropertyUtilities();
	WeakStructHandle = StructPropertyHandle;

	TSharedPtr<IPropertyHandle> ChannelHandle = StructPropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCBinder, DataChannel));

	HeaderRow
	.NameContent()
	[
		StructPropertyHandle->CreatePropertyNameWidget()
	]
	.ValueContent()
	.MinDesiredWidth(250.f)
	.MaxDesiredWidth(400.f)
	[
		ChannelHandle->CreatePropertyValueWidget()
	];

	ChannelHandle->SetOnPropertyValueChanged(FSimpleDelegate::CreateSP(this, &FNDCBinderCustomization::OnChannelPropertyChanged));
}

void FNDCBinderCustomization::CustomizeChildren(TSharedRef<IPropertyHandle> StructPropertyHandle, IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	using namespace NDCBinderCustomizationPrivate;

	PropertyUtilities = CustomizationUtils.GetPropertyUtilities();
	WeakStructHandle = StructPropertyHandle;

	TSet<FName> ChannelVarNames;
	SyncBindings(StructPropertyHandle, ChannelVarNames);

	// A wrong Event Data Type invalidates every binding that took the old struct at once. Say that
	// here, where the eye lands first, rather than leaving it to be inferred from a column of
	// markers — and rather than burying it in the collapsed Advanced group where the property lives.
	BuildEventDataTypeWarningRow(ChildBuilder, StructPropertyHandle);

	// The context every write starts from: the channel's own input fields, each as a value editor
	// plus a bind button for the case where that value has to be computed per write.
	BuildContextRows(ChildBuilder, StructPropertyHandle);

	// One row per binding.
	TSharedPtr<IPropertyHandle> BindingsHandle = StructPropertyHandle->GetChildHandle(FNDCBinder::GetBindingsPropertyName());
	TSharedPtr<IPropertyHandleArray> ArrayHandle = BindingsHandle->AsArray();
	if (!ArrayHandle.IsValid())
	{
		return;
	}

	uint32 NumElements = 0;
	ArrayHandle->GetNumElements(NumElements);

	// The payload, kept visibly apart from the access context above: one says what is written, the
	// other where it goes, and reading them as one list makes neither question easy to answer.
	IDetailGroup& PayloadGroup = ChildBuilder.AddGroup("NDCPayload", LOCTEXT("PayloadGroup", "Payload"), /*bStartExpanded=*/ true);

	// A stale row — one the channel has no variable for, with something bound to it — fails the
	// compile of the asset carrying it, so this line is here to say why, in the same place the author
	// is looking when it happens.
	//
	// Counted by IsStaleAndDrawn, which is the compiler's own condition rather than an approximation
	// of it: this line claims a compile failure, so the two have to be the same claim. Two kinds of
	// stale row are deliberately absent — an unbound one and a context row whose field is gone are
	// both dropped by the sync, neither being anything an author asked for.
	int32 NumStale = 0;
	StructPropertyHandle->EnumerateConstRawData([&NumStale, &ChannelVarNames](const void* RawData, const int32, const int32) -> bool
	{
		if (const FNDCBinder* Writer = static_cast<const FNDCBinder*>(RawData))
		{
			int32 Count = 0;
			for (const FNDCVariableBinding& Row : Writer->GetBindings())
			{
				Count += IsStaleAndDrawn(Row, ChannelVarNames) ? 1 : 0;
			}
			NumStale = FMath::Max(NumStale, Count);
		}
		return true;
	});
	if (NumStale > 0)
	{
		PayloadGroup.AddWidgetRow()
		.WholeRowContent()
		[
			SNew(STextBlock)
			.Text(FText::Format(
				LOCTEXT("StaleRowsPresentFmt",
					"{0} row(s) have no variable in this channel, which fails this asset's compile. Restore the variable, or delete the row."),
				FText::AsNumber(NumStale)))
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.ColorAndOpacity(FStyleColors::Error)
			.AutoWrapText(true)
		];
	}

	for (uint32 Index = 0; Index < NumElements; ++Index)
	{
		TSharedRef<IPropertyHandle> BindingHandle = ArrayHandle->GetElement(Index);

		const FNDCVariableBinding* Binding = GetBindingData(BindingHandle);
		if (!Binding)
		{
			continue;
		}
		const FName VarName = Binding->VarName;
		const ENDCVariableType Type = Binding->Type;

		const bool bStale = IsStaleAndDrawn(*Binding, ChannelVarNames);
		const FText TypeText = StaticEnum<ENDCVariableType>()->GetDisplayNameTextByValue((int64)Type);

		FText NameToolTip = FText::Format(LOCTEXT("BindingRowTooltip", "{0} ({1})"), FText::FromName(VarName), TypeText);
		FSlateColor NameColor = FSlateColor::UseForeground();
		if (bStale)
		{
			NameColor = FStyleColors::Error;
			NameToolTip = LOCTEXT("StaleRowTooltip", "This variable no longer exists in the channel, so the row writes nothing and fails this asset's compile. It is kept because something was authored on it. Put the variable back and it works again with what it holds, or delete it with the bin at the end of the row.");
		}
		else if (Type == ENDCVariableType::Unsupported)
		{
			//~ Warning, not error: the validator lets this ship. The channel declares a type no Write*
			//~ overload covers, which is the channel's business and nothing the author of this asset
			//~ can act on beyond knowing the row goes out empty.
			NameColor = FStyleColors::Warning;
			NameToolTip = LOCTEXT("UnsupportedRowTooltip", "This channel variable type has no writer support — the row is skipped at write time.");
		}

		TSharedRef<IPropertyHandle> SourceHandle = BindingHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCVariableBinding, Source)).ToSharedRef();

		FDetailWidgetRow& Row = PayloadGroup.AddWidgetRow();
		BindRowCopyPaste(Row, BindingHandle, VarName, Type, Binding->EnumDef, /*bPasteAllowed=*/ !bStale && Type != ENDCVariableType::Unsupported);
		Row
		.FilterString(FText::FromName(VarName))
		.NameContent()
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("BindingRowFormat", "{0}  ({1})"), FText::FromName(VarName), TypeText))
			.ToolTipText(NameToolTip)
			.ColorAndOpacity(NameColor)
			.Font(IDetailLayoutBuilder::GetDetailFont())
		]
		.ValueContent()
		.MinDesiredWidth(450.f)
		[
			SNew(SHorizontalBox)
			// Value editor, gone while bound. A payload row's constant is not a fallback: a row whose
			// binding cannot be resolved is skipped outright and writes nothing, and a row whose
			// binding resolves always overwrites it. So once bound it is dead data, and showing it
			// greyed out only invited the reader to work that out for themselves.
			//
			// Collapsed, not Hidden: the bind button then slides into the space the editor gave up
			// and can spell out what the row is bound to instead of clipping it to a stub. That is
			// the trade — a bound row loses its column alignment, and in exchange the binding reads
			// as this row's value rather than as a control parked beside an empty box.
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.Visibility_Lambda([SourceHandle]()
				{
					return IsSourceBound(SourceHandle) ? EVisibility::Collapsed : EVisibility::Visible;
				})
				//~ Greyed on a stale row rather than hidden: such a row can be one whose only content is
				//~ the value shown here, and that value is why it was kept and what the author is being
				//~ asked about. Readable, not editable — editing it would change nothing that is written.
				.IsEnabled_Lambda([SourceHandle, bStale]()
				{
					return !bStale && !IsSourceBound(SourceHandle);
				})
				[
					BuildValueEditor(BindingHandle, Type)
				]
			]
			// Bind button — hidden for unsupported types (nothing can write them anyway).
			// NOTE: no CreateDefaultPropertyButtonWidgets here — on array elements it brings the
			// Insert/Delete/Duplicate menu (meaningless for channel-synced rows) and an
			// index-based reset (wrong variable after any sync reorder).
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.f, 0.f, 0.f, 0.f)
			[
				SNew(SBox)
				.Visibility(Type == ENDCVariableType::Unsupported ? EVisibility::Collapsed : EVisibility::Visible)
				.IsEnabled(!bStale)
				[
					BuildBindButton(StructPropertyHandle, BindingHandle, Type)
				]
			]
			// The way out of a stale row, on the row itself. Nothing else here is live: a row the
			// channel has no variable for cannot be given a value or a binding that would do anything,
			// so the only control it keeps is the one that removes it. Present only on such a row, so
			// an ordinary row is not invited to be deleted — the set is the channel's to decide.
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.f, 0.f, 0.f, 0.f)
			[
				SNew(SBox)
				.Visibility(bStale ? EVisibility::Visible : EVisibility::Collapsed)
				[
					PropertyCustomizationHelpers::MakeDeleteButton(
						FSimpleDelegate::CreateLambda([StructPropertyHandle, Utilities = PropertyUtilities, VarName]()
						{
							// Its own edit, so its own transaction — unlike the sync, which runs inside
							// the property change that provoked it. Removed by NAME rather than by the
							// index this widget was built at: a sync between the two reorders the array.
							const FScopedTransaction Transaction(LOCTEXT("RemoveStaleRowTransaction", "Remove Stale NDC Row"));
							StructPropertyHandle->NotifyPreChange();
							StructPropertyHandle->EnumerateRawData([VarName](void* RawData, const int32, const int32) -> bool
							{
								if (FNDCBinder* Writer = static_cast<FNDCBinder*>(RawData))
								{
									Writer->GetMutableBindingsUnchecked().RemoveAll(
										[VarName](const FNDCVariableBinding& Row) { return Row.VarName == VarName; });
								}
								return true;
							});
							StructPropertyHandle->NotifyPostChange(EPropertyChangeType::ArrayRemove);
							if (Utilities.IsValid())
							{
								Utilities->RequestForceRefresh();
							}
						}),
						LOCTEXT("RemoveStaleRowTooltip", "Delete this row. It has no variable in this channel, so it writes nothing and fails the asset's compile."))
				]
			]
		];
	}

	// An empty Payload group, said out loud. The Access Context group next to it either does not
	// appear or explains itself, and a panel that narrates every other dead end should not leave this
	// one as a heading over nothing. Which of the two it is matters: no channel is a step not taken
	// yet and names where to take it, while a channel with no variables is the channel's own doing and
	// nothing here will change it.
	if (NumElements == 0)
	{
		const FNDCBinder* const EmptyWriter = GetWriterData(StructPropertyHandle);
		const bool bHasChannel = EmptyWriter && EmptyWriter->GetChannel() != nullptr;
		PayloadGroup.AddWidgetRow()
		.WholeRowContent()
		[
			SNew(STextBlock)
			.Text(bHasChannel
				? LOCTEXT("PayloadNoVariables", "This Data Channel declares no variables, so there is nothing to write.")
				: LOCTEXT("PayloadNoChannel", "Assign a Data Channel above and its variables are listed here, one row each."))
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
	}

	// Visibility flags feed UNiagaraDataChannelLibrary::CreateDataChannelWriter directly, but the
	// defaults (all true) cover every current use case — tuck them away in a collapsed Advanced
	// group at the bottom (analogous to the AdvancedDisplay meta, which manual child layout ignores).
	// NOTE: the third AddGroup argument is bStartExpanded, so false = collapsed.
	IDetailGroup& AdvancedGroup = ChildBuilder.AddGroup("NDCAdvanced", LOCTEXT("AdvancedGroup", "Advanced"), false);
	// A C++ owner sets EventDataType in its constructor and never touches this row; a Blueprint-only
	// owner has nowhere else to declare it. The bind menu points here when it is missing.
	AdvancedGroup.AddPropertyRow(StructPropertyHandle->GetChildHandle(FNDCBinder::GetEventDataTypePropertyName()).ToSharedRef());
	AdvancedGroup.AddPropertyRow(StructPropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCBinder, bVisibleToGame)).ToSharedRef());
	AdvancedGroup.AddPropertyRow(StructPropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCBinder, bVisibleToCPU)).ToSharedRef());
	AdvancedGroup.AddPropertyRow(StructPropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCBinder, bVisibleToGPU)).ToSharedRef());
}

void FNDCBinderCustomization::SyncBindings(const TSharedRef<IPropertyHandle>& StructPropertyHandle, TSet<FName>& OutChannelVarNames)
{
	// Pass 1 (read-only): collect the channel variable names and find out whether there is anything to
	// sync at all. CustomizeChildren runs on every details refresh, and the notify pair below must not
	// fire when nothing changes — an unmatched NotifyPreChange would leave the edited object mid-edit.
	bool bNeedsSync = false;
	StructPropertyHandle->EnumerateConstRawData([&](const void* RawData, const int32 /*DataIndex*/, const int32 /*NumDatas*/) -> bool
	{
		const FNDCBinder* Writer = static_cast<const FNDCBinder*>(RawData);
		if (!Writer)
		{
			return true;
		}

		if (const UNiagaraDataChannel* Channel = Writer->GetChannel())
		{
			for (const FNiagaraDataChannelVariable& Var : Channel->GetVariables())
			{
				if (Var.IsValid())
				{
					OutChannelVarNames.Add(Var.GetName());
				}
			}
		}

		bNeedsSync |= Writer->NeedsBindingSync();
		return true;
	});

	if (!bNeedsSync)
	{
		return;
	}

	// Pass 2: the rows are rebuilt from the channel in place, so announce it like any other edit —
	// NotifyPreChange takes the transaction/Modify() snapshot the matching NotifyPostChange needs.
	StructPropertyHandle->NotifyPreChange();
	StructPropertyHandle->EnumerateRawData([](void* RawData, const int32 /*DataIndex*/, const int32 /*NumDatas*/) -> bool
	{
		if (FNDCBinder* Writer = static_cast<FNDCBinder*>(RawData))
		{
			Writer->SyncBindingsWithChannel();
		}
		return true;
	});
	StructPropertyHandle->NotifyPostChange(EPropertyChangeType::ArrayAdd);
}

void FNDCBinderCustomization::BuildEventDataTypeWarningRow(IDetailChildrenBuilder& ChildBuilder, const TSharedRef<IPropertyHandle>& StructPropertyHandle) const
{
	using namespace NDCBinderCustomizationPrivate;

	// Everything is a lambda so the row appears and disappears as Event Data Type is edited, with no
	// panel refresh: the whole point is that it tracks the very property that causes the problem.
	auto GetWarningText = [StructPropertyHandle]() -> FText
	{
		int32 NumAffected = 0;
		UScriptStruct* Expected = FindExpectedEventDataType(StructPropertyHandle, NumAffected);
		if (!Expected || NumAffected == 0)
		{
			return FText::GetEmpty();
		}

		const FNDCBinder* Writer = GetWriterData(StructPropertyHandle);
		const FText Declared = (Writer && Writer->GetEventDataType())
			? FText::FromString(Writer->GetEventDataType()->GetName())
			: LOCTEXT("EventDataTypeNotSet", "not set");

		return FText::Format(
			LOCTEXT("EventDataTypeMismatchFmt", "{0} binding(s) take F{1}, but Event Data Type is {2} — those bindings are skipped at write time. Set Advanced > Event Data Type to {1}."),
			FText::AsNumber(NumAffected),
			FText::FromString(Expected->GetName()),
			Declared);
	};

	ChildBuilder.AddCustomRow(LOCTEXT("EventDataTypeWarningRowFilter", "Event Data Type"))
	.Visibility(TAttribute<EVisibility>::CreateLambda([GetWarningText]()
	{
		return GetWarningText().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
	}))
	.WholeRowContent()
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.f, 0.f, 4.f, 0.f)
		[
			SNew(SImage)
			.Image(FAppStyle::GetBrush("Icons.Warning"))
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			//~ Error: every binding this line counts is one the validator reports, each for taking a
			//~ struct the writer does not declare.
			.ColorAndOpacity(FStyleColors::Error)
			.AutoWrapText(true)
			.Text_Lambda(GetWarningText)
		]
	];
}

void FNDCBinderCustomization::BuildContextRows(IDetailChildrenBuilder& ChildBuilder, const TSharedRef<IPropertyHandle>& StructPropertyHandle) const
{
	using namespace NDCBinderCustomizationPrivate;

	TSharedPtr<IPropertyHandle> DefaultContextHandle = StructPropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCBinder, DefaultAccessContext));
	TSharedPtr<IPropertyHandle> InnerHandle = DefaultContextHandle.IsValid()
		? DefaultContextHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCAccessContextInst, AccessContext))
		: nullptr;
	TSharedPtr<IPropertyHandle> RowsHandle = StructPropertyHandle->GetChildHandle(FNDCBinder::GetContextBindingsPropertyName());
	const FNDCBinder* Writer = GetWriterData(StructPropertyHandle);
	const UScriptStruct* ContextType = Writer ? Writer->GetContextType() : nullptr;
	if (!InnerHandle.IsValid() || !RowsHandle.IsValid() || !ContextType)
	{
		return;
	}

	// One row per context field: the value as authored, and the binding that computes it per write.
	// Two lists would mean naming every field twice and reading two places to answer one question.
	//
	// The fields are enumerated from reflection rather than taken from whatever the property system
	// chooses to draw, because neither of the obvious routes reaches all of them. Niagara registers a
	// property type customization for the context structs, so FInstancedStructDataDetails routes
	// through AddChildStructure and never calls its OnChildRowAdded hook; and that customization
	// deliberately hides transient inputs, which is Owning Component, Location and Override Location —
	// the three fields most worth binding.
	//
	// The handles come from one AddChildStructure over the whole struct, which is the same call
	// FInstancedStructDataDetails makes. Asking for each field by name instead — one
	// AddChildStructureProperty per field, sharing a provider — crashes inside the property editor.
	// A field with no handle is one the property system will not expose; its row still binds.
	TSharedRef<IPropertyHandle> Rows = RowsHandle.ToSharedRef();

	TMap<FName, TSharedPtr<IPropertyHandle>> HandleByField;
	if (Writer->DefaultAccessContext.IsValid())
	{
		InnerHandle->RemoveChildren();
		for (const TSharedPtr<IPropertyHandle>& ChildHandle : InnerHandle->AddChildStructure(MakeShared<FInstancedStructProvider>(InnerHandle)))
		{
			if (ChildHandle.IsValid())
			{
				if (const FProperty* ChildProperty = ChildHandle->GetProperty())
				{
					HandleByField.Add(ChildProperty->GetFName(), ChildHandle);
				}
			}
		}
	}

	// Every bool that is some other field's enable flag — bOverrideLocation, bOverrideSystemToSpawn,
	// bOverrideCellSize. None of them gets a row: the row that binds the field it gates switches it
	// on, so a row of its own would be a second way to say the same thing and a way to get it wrong.
	TSet<FName> EnableFlags;
	FNDCBinder::ForEachContextInputField(ContextType, [&EnableFlags, ContextType](FProperty& Field)
	{
		const FName Flag = FNDCBinder::ResolveContextFieldEnableFlag(ContextType, &Field);
		if (!Flag.IsNone())
		{
			EnableFlags.Add(Flag);
		}
	});

	IDetailGroup& ContextGroup = ChildBuilder.AddGroup("NDCAccessContext", LOCTEXT("AccessContextGroup", "Access Context"), /*bStartExpanded=*/ true);

	int32 NumFields = 0;
	FNDCBinder::ForEachContextInputField(ContextType, [&](FProperty& Field)
	{
		if (EnableFlags.Contains(Field.GetFName()) || IsHiddenContextField(ContextType, Field))
		{
			return;
		}
		++NumFields;

		FProperty* FieldPtr = &Field;
		const FName FieldName = Field.GetFName();
		const bool bCanBeRequired = FNDCBinder::CanContextFieldBeRequired(FieldPtr);

		TFunction<FBoundTo()> GetCurrent = [Rows, FieldName]() { return GetContextRowBinding(Rows, FieldName); };
		TFunction<bool()> IsBound = [GetCurrent]() { return GetCurrent().IsBound(); };

		// Whether the authored value still means something while the field is bound.
		//
		// It does in exactly one case: an object field whose row is not Required. There a null answer
		// is "no opinion" and the authored value is what goes out — that is how a per-surface system
		// getter falls back to the panel. Everywhere else a bound row always overwrites, so the value
		// is dead the moment something is bound to it.
		TFunction<bool()> IsFallbackForNullAnswer = [Rows, FieldName, bCanBeRequired]()
		{
			if (!bCanBeRequired)
			{
				return false;
			}
			TSharedPtr<IPropertyHandle> Row = FindContextRow(Rows, FieldName);
			TSharedPtr<IPropertyHandle> Required = Row.IsValid() ? Row->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCContextBinding, bRequired)) : nullptr;
			bool bValue = false;
			return Required.IsValid() && Required->GetValue(bValue) == FPropertyAccess::Success && !bValue;
		};

		// Required, offered only where a value can mean "there wasn't one".
		TSharedRef<SWidget> RequiredToggle =
			SNew(SCheckBox)
			.Visibility_Lambda([IsBound, bCanBeRequired]()
			{
				return (bCanBeRequired && IsBound()) ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.ToolTipText(LOCTEXT("ContextRequiredTooltip", "Required: a null answer declines the whole write, rather than leaving the value on the left standing. This is how a cue says \"there is nothing to attach to here, skip it\"."))
			.IsChecked_Lambda([Rows, FieldName]()
			{
				TSharedPtr<IPropertyHandle> Row = FindContextRow(Rows, FieldName);
				TSharedPtr<IPropertyHandle> Required = Row.IsValid() ? Row->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCContextBinding, bRequired)) : nullptr;
				bool bValue = false;
				return (Required.IsValid() && Required->GetValue(bValue) == FPropertyAccess::Success && bValue)
					? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([Rows, FieldName](ECheckBoxState NewState)
			{
				TSharedPtr<IPropertyHandle> Row = FindContextRow(Rows, FieldName);
				if (TSharedPtr<IPropertyHandle> Required = Row.IsValid() ? Row->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCContextBinding, bRequired)) : nullptr)
				{
					Required->SetValue(NewState == ECheckBoxState::Checked);
				}
			})
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ContextRequiredLabel", "Required"))
				.Font(IDetailLayoutBuilder::GetDetailFont())
			];

		// A gated field carries a checkbox on its own row — Unreal draws its flag inline — and that
		// checkbox is the author's answer, binding or no binding. The write does not touch it, so
		// binding a field whose gate is clear writes the field and changes nothing.
		//
		// Which is a trap unless it is visible, so it is marked: FlagHandle is the gate the panel can
		// hold, and the row shows a warning while it is clear and the field is bound.
		//
		// Nothing is marked for a gate the panel cannot hold — a Transient one has no handle and no
		// checkbox, so there was never a choice to disagree with, and the write sets it itself.
		const FName EnableFlag = FNDCBinder::ResolveContextFieldEnableFlag(ContextType, FieldPtr);
		TSharedPtr<IPropertyHandle> FlagHandle = EnableFlag.IsNone() ? nullptr : HandleByField.FindRef(EnableFlag);

		TFunction<bool()> IsGateOpen = [FlagHandle]()
		{
			bool bValue = false;
			return !FlagHandle.IsValid() || (FlagHandle->GetValue(bValue) == FPropertyAccess::Success && bValue);
		};

		const FText UnboundTooltip = !FlagHandle.IsValid()
			? FText::Format(
				LOCTEXT("ContextFieldUnboundTooltipFmt", "Bind {0} to a function that computes it per write. Unbound, the value on the left is used as authored."),
				Field.GetDisplayNameText())
			: FText::Format(
				LOCTEXT("ContextFieldUnboundGatedTooltipFmt", "Bind {0} to a function that computes it per write. Unbound, the value on the left is used as authored.\n\nThis field is only used when the checkbox next to its name is ticked, whether it is bound or not — the write does not tick it for you."),
				Field.GetDisplayNameText());

		TSharedRef<SWidget> GateWarning =
			SNew(SImage)
			.Visibility_Lambda([IsBound, IsGateOpen]()
			{
				return (IsBound() && !IsGateOpen()) ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.Image(FAppStyle::GetBrush("Icons.Warning"))
			//~ Warning, and the icon already says so: a clear checkbox is a legal asset that ships.
			.ColorAndOpacity(FStyleColors::Warning)
			.ToolTipText(FText::Format(
				LOCTEXT("ContextGateClearTooltipFmt", "{0} is bound, but the checkbox next to its name is clear, so the context ignores this field and the binding has no effect. Tick it to use the bound value."),
				Field.GetDisplayNameText()));

		TSharedRef<SWidget> BindButton = MakeBindButton(
			StructPropertyHandle,
			[FieldPtr](const FNDCBinder& W, const UFunction* Func) { return AcceptsForField(W, Func, FieldPtr); },
			[FieldPtr](const FNDCBinder&, const FProperty* Source) { return AcceptsEventDataForField(Source, FieldPtr); },
			GetCurrent,
			[Rows, ContextType, FieldPtr](FBoundTo Bound) { SetContextRowBinding(Rows, ContextType, FieldPtr, Bound); },
			[Rows, FieldName]() { ClearContextRow(Rows, FieldName); },
			UnboundTooltip,
			FString::Printf(TEXT("Get%s"), *Field.GetName()),
			GetFieldReturnPinType(FieldPtr),
			FieldPtr);

		TSharedPtr<IPropertyHandle> FieldHandle = HandleByField.FindRef(FieldName);
		IDetailPropertyRow* PropRow = FieldHandle.IsValid()
			? &ContextGroup.AddPropertyRow(FieldHandle.ToSharedRef())
			: nullptr;

		if (!PropRow)
		{
			// No property row for this field. Nothing is lost that could have been authored — the row
			// still binds — so say where the value comes from instead of leaving an empty cell.
			ContextGroup.AddWidgetRow()
			.FilterString(Field.GetDisplayNameText())
			.NameContent()
			[
				SNew(STextBlock)
				.Text(Field.GetDisplayNameText())
				.ToolTipText(Field.GetToolTipText())
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
			.ValueContent()
			.MinDesiredWidth(450.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Visibility_Lambda([IsBound]() { return IsBound() ? EVisibility::Collapsed : EVisibility::Visible; })
					.Text(LOCTEXT("ContextFieldNoEditor", "not set — bind to compute it per write"))
					.Font(IDetailLayoutBuilder::GetDetailFont())
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 0.f, 0.f, 0.f) [ GateWarning ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 0.f, 0.f, 0.f) [ RequiredToggle ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 0.f, 0.f, 0.f) [ BindButton ]
			];
			return;
		}

		// The native widgets, kept rather than reimplemented: the value editor for a context field is
		// whatever the property system already builds for its type, inline edit-condition checkbox and
		// struct expansion included.
		TSharedPtr<SWidget> NameWidget;
		TSharedPtr<SWidget> ValueWidget;
		FDetailWidgetRow DefaultRow;
		PropRow->GetDefaultWidgets(NameWidget, ValueWidget, DefaultRow, /*bAddWidgetDecoration=*/ true);

		FDetailWidgetRow& Row = PropRow->CustomWidget(/*bShowChildren=*/ true);
		Row.CopyAction(DefaultRow.CopyMenuAction);
		Row.PasteAction(DefaultRow.PasteMenuAction);
		// The name half keeps its inline gate checkbox live at all times: it is the author's switch,
		// and a binding does not take it away from them.
		Row.NameContent()
		[
			NameWidget.ToSharedRef()
		];
		Row.ValueContent()
		.MinDesiredWidth(450.f)
		[
			SNew(SHorizontalBox)

			// Gone while a function drives it, exactly as a payload row — with the one exception
			// below, where the authored value is still what a null answer falls back to.
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.Visibility_Lambda([IsBound, IsFallbackForNullAnswer]()
				{
					// Gone once bound, unless the authored value is still what a null answer falls
					// back to — then it is live data and hiding it would hide the fallback.
					return (IsBound() && !IsFallbackForNullAnswer()) ? EVisibility::Collapsed : EVisibility::Visible;
				})
				.IsEnabled_Lambda([IsBound]() { return !IsBound(); })
				[
					ValueWidget.ToSharedRef()
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 0.f, 0.f, 0.f) [ GateWarning ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 0.f, 0.f, 0.f) [ RequiredToggle ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 0.f, 0.f, 0.f) [ BindButton ]
		];
	});

	if (NumFields == 0)
	{
		ContextGroup.AddWidgetRow()
		.WholeRowContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("ContextNoInputFields", "This channel's access context declares no bindable input fields."))
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
	}
}

TSharedRef<SWidget> FNDCBinderCustomization::BuildValueEditor(TSharedRef<IPropertyHandle> BindingHandle, ENDCVariableType Type) const
{
	using namespace NDCBinderCustomizationPrivate;

	const FNDCVariableBinding* Binding = GetBindingData(BindingHandle);

	if (Type == ENDCVariableType::Enum)
	{
		TSharedRef<IPropertyHandle> EnumValueHandle = BindingHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCVariableBinding, EnumValue)).ToSharedRef();
		UEnum* EnumDef = Binding ? Binding->EnumDef.Get() : nullptr;
		if (!EnumDef)
		{
			return SNew(STextBlock)
				.Text(LOCTEXT("NoEnumDef", "Enum type unknown"))
				.Font(IDetailLayoutBuilder::GetDetailFont());
		}

		// SComboButton builds its menu on open — no options array lifetime to manage.
		return SNew(SComboButton)
			.ButtonContent()
			[
				SNew(STextBlock)
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.Text_Lambda([EnumValueHandle, EnumDef]() -> FText
				{
					void* RawData = nullptr;
					const int64 Value = EnumValueHandle->GetValueData(RawData) == FPropertyAccess::Success
						? (int64)*static_cast<uint8*>(RawData)
						: 0;
					return EnumDef->GetDisplayNameTextByValue(Value);
				})
			]
			.OnGetMenuContent_Lambda([EnumValueHandle, EnumDef]() -> TSharedRef<SWidget>
			{
				FMenuBuilder MenuBuilder(true, nullptr);
				for (int32 NameIndex = 0; NameIndex < EnumDef->NumEnums(); ++NameIndex)
				{
					if (EnumDef->GetNameStringByIndex(NameIndex).EndsWith(TEXT("_MAX")))
					{
						continue;
					}
					const int64 Value = EnumDef->GetValueByIndex(NameIndex);
					MenuBuilder.AddMenuEntry(
						EnumDef->GetDisplayNameTextByIndex(NameIndex),
						FText::GetEmpty(),
						FSlateIcon(),
						FUIAction(FExecuteAction::CreateLambda([EnumValueHandle, Value]()
						{
							EnumValueHandle->SetValueFromFormattedString(FString::FromInt((int32)Value));
						})));
				}
				return MenuBuilder.MakeWidget();
			});
	}

	FName ValuePropName = NAME_None;
	switch (Type)
	{
	case ENDCVariableType::Bool:        ValuePropName = GET_MEMBER_NAME_CHECKED(FNDCVariableBinding, BoolValue); break;
	case ENDCVariableType::Int32:       ValuePropName = GET_MEMBER_NAME_CHECKED(FNDCVariableBinding, IntValue); break;
	case ENDCVariableType::Float:       ValuePropName = GET_MEMBER_NAME_CHECKED(FNDCVariableBinding, FloatValue); break;
	case ENDCVariableType::Vector2D:    ValuePropName = GET_MEMBER_NAME_CHECKED(FNDCVariableBinding, Vector2DValue); break;
	case ENDCVariableType::Vector:
	case ENDCVariableType::Position:    ValuePropName = GET_MEMBER_NAME_CHECKED(FNDCVariableBinding, VectorValue); break;
	case ENDCVariableType::Vector4:     ValuePropName = GET_MEMBER_NAME_CHECKED(FNDCVariableBinding, Vector4Value); break;
	case ENDCVariableType::Quat:        ValuePropName = GET_MEMBER_NAME_CHECKED(FNDCVariableBinding, QuatValue); break;
	case ENDCVariableType::LinearColor: ValuePropName = GET_MEMBER_NAME_CHECKED(FNDCVariableBinding, ColorValue); break;
	default: break;
	}

	if (ValuePropName.IsNone())
	{
		// SpawnInfo/ID have no constant — the bind button next to this is the only way to fill them.
		const bool bFunctionOnly = Type == ENDCVariableType::SpawnInfo || Type == ENDCVariableType::ID;
		return SNew(STextBlock)
			.Text(bFunctionOnly ? LOCTEXT("FunctionOnlyValue", "Bind a function") : LOCTEXT("UnsupportedTypeValue", "—"))
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.ColorAndOpacity(FSlateColor::UseSubduedForeground());
	}

	TSharedRef<IPropertyHandle> ValueHandle = BindingHandle->GetChildHandle(ValuePropName).ToSharedRef();

	// Structs the property editor has no value widget for render empty in a details value column
	// (their editors are layout-level customizations, not value widgets), so compose the member
	// widgets side by side instead — the layout standard rows give a vector, UMG "Image Size"-style.
	// Enumerating the handle's own children keeps that working for any struct added later.
	const FStructProperty* StructProp = CastField<FStructProperty>(ValueHandle->GetProperty());
	const bool bHasNativeValueWidget = !StructProp
		|| StructProp->Struct == TBaseStructure<FLinearColor>::Get()   // swatch + color picker
		|| StructProp->Struct == TBaseStructure<FColor>::Get();

	uint32 NumMembers = 0;
	ValueHandle->GetNumChildren(NumMembers);
	if (bHasNativeValueWidget || NumMembers == 0)
	{
		return ValueHandle->CreatePropertyValueWidget();
	}

	TSharedRef<SHorizontalBox> MembersBox = SNew(SHorizontalBox);
	for (uint32 MemberIndex = 0; MemberIndex < NumMembers; ++MemberIndex)
	{
		TSharedPtr<IPropertyHandle> MemberHandle = ValueHandle->GetChildHandle(MemberIndex);
		if (!MemberHandle.IsValid())
		{
			continue;
		}
		MembersBox->AddSlot()
		.FillWidth(1.f)
		.Padding(MemberIndex == 0 ? 0.f : 2.f, 0.f, 0.f, 0.f)
		[
			MemberHandle->CreatePropertyValueWidget()
		];
	}
	return MembersBox;
}

TSharedRef<SWidget> FNDCBinderCustomization::BuildBindButton(TSharedRef<IPropertyHandle> StructHandle, TSharedRef<IPropertyHandle> BindingHandle, ENDCVariableType Type) const
{
	using namespace NDCBinderCustomizationPrivate;

	TSharedRef<IPropertyHandle> BoundFunctionHandle = BindingHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCVariableBinding, BoundFunction)).ToSharedRef();
	TSharedRef<IPropertyHandle> SourceHandle = BindingHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCVariableBinding, Source)).ToSharedRef();
	TSharedRef<IPropertyHandle> BoundFieldHandle = BindingHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FNDCVariableBinding, BoundEventDataField)).ToSharedRef();

	// Name and return type for a function created from this row: both come from the channel variable.
	const FNDCVariableBinding* Binding = GetBindingData(BindingHandle);
	const FName VarName = Binding ? Binding->VarName : NAME_None;
	UEnum* EnumDef = Binding ? Binding->EnumDef.Get() : nullptr;

	return MakeBindButton(
		StructHandle,
		[Type, EnumDef](const FNDCBinder& Writer, const UFunction* Func) { return Writer.IsValidValueFunction(Func, Type, EnumDef); },
		[Type, EnumDef](const FNDCBinder&, const FProperty* Field) { return FNDCBinder::IsPropertyCompatibleWithVariableType(Field, Type, EnumDef); },
		[SourceHandle, BoundFunctionHandle, BoundFieldHandle]() { return ReadBoundTo(SourceHandle, BoundFunctionHandle, BoundFieldHandle); },
		[SourceHandle, BoundFunctionHandle, BoundFieldHandle](FBoundTo Bound) { WriteBoundTo(SourceHandle, BoundFunctionHandle, BoundFieldHandle, Bound); },
		[SourceHandle, BoundFunctionHandle, BoundFieldHandle]() { WriteBoundTo(SourceHandle, BoundFunctionHandle, BoundFieldHandle, FBoundTo{}); },
		FText::Format(LOCTEXT("BindButtonTooltipFmt", "Bind to a field of the event data, or to a function on this class: {0}."), GetEventDataParamText(StructHandle)),
		FString::Printf(TEXT("Get%s"), *VarName.ToString()),
		GetValueReturnPinType(Type, EnumDef));
}

void FNDCBinderCustomization::OnChannelPropertyChanged()
{
	if (bRefreshing)
	{
		return;
	}
	bRefreshing = true;
	if (TSharedPtr<IPropertyHandle> StructHandle = WeakStructHandle.Pin())
	{
		TSet<FName> Unused;
		SyncBindings(StructHandle.ToSharedRef(), Unused);

		// An explicit channel switch drops the rows that have no variable in the new channel —
		// their stored values are meaningless for it. (Removing a variable from the SAME channel
		// keeps the stale row; only a switch prunes.) Same as in SyncBindings: ask first, so the
		// notify pair only wraps a real edit.
		bool bHasStale = false;
		StructHandle->EnumerateConstRawData([&bHasStale](const void* RawData, int32 /*DataIndex*/, int32 /*NumDatas*/) -> bool
		{
			if (const FNDCBinder* Writer = static_cast<const FNDCBinder*>(RawData))
			{
				bHasStale |= Writer->HasStaleBindings();
			}
			return true;
		});

		if (bHasStale)
		{
			StructHandle->NotifyPreChange();
			StructHandle->EnumerateRawData([](void* RawData, int32 /*DataIndex*/, int32 /*NumDatas*/) -> bool
			{
				if (FNDCBinder* Writer = static_cast<FNDCBinder*>(RawData))
				{
					Writer->PruneStaleBindings();
				}
				return true;
			});
			StructHandle->NotifyPostChange(EPropertyChangeType::ArrayRemove);
		}
	}
	if (PropertyUtilities.IsValid())
	{
		// Deferred, not immediate: this runs from inside a property-changed callback, and IPropertyUtilities
		// says so itself ("This may run immediately; consider RequestForceRefresh"). Rebuilding the panel
		// under the delegate that is still executing also drops the focus the author was typing in.
		PropertyUtilities->RequestForceRefresh();
	}
	bRefreshing = false;
}

#undef LOCTEXT_NAMESPACE
