// NDCBinderEditorModule.cpp

#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "NDCBinder.h"
#include "NDCBinderCustomization.h"
#include "NDCBinderCompilerExtension.h"

class FNDCBinderEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
		PropertyModule.RegisterCustomPropertyTypeLayout(
			FNDCBinder::StaticStruct()->GetFName(),
			FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FNDCBinderCustomization::MakeInstance));

		// A row that cannot run fails the compile of the Blueprint carrying it. Registered here, once,
		// rather than asked of every class that owns a writer — see UNDCBinderCompilerExtension.
		UNDCBinderCompilerExtension::Register();
	}

	virtual void ShutdownModule() override
	{
		if (FModuleManager::Get().IsModuleLoaded(TEXT("PropertyEditor")))
		{
			FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
			PropertyModule.UnregisterCustomPropertyTypeLayout(FNDCBinder::StaticStruct()->GetFName());
		}
	}
};

IMPLEMENT_MODULE(FNDCBinderEditorModule, NDCBinderEditor);
