// NDCBinderCustomization.h
#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"
#include "NDCBinder.h"

class IPropertyHandle;
class IPropertyUtilities;
class SWidget;

/**
 * Details customization for FNDCBinder: the DataChannel lives in the header row; the children
 * are the access context's own input fields, one row per channel variable (synced from the NDC asset)
 * and the visibility flags.
 *
 * Every bindable row — context field or payload variable — mirrors the engine's property binding
 * widget (UMG-style): the typed value editor stays visible, disabled while bound, and a chain-icon
 * button next to it offers the owning class's compatible functions. Unlink icon when unbound, Link
 * icon plus the function name when bound, and a magnifier to jump to the implementation.
 *
 * Rows for variables that no longer exist in the channel are kept and marked; an explicit channel
 * switch prunes them.
 */
class FNDCBinderCustomization : public IPropertyTypeCustomization
{
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance()
	{
		return MakeShared<FNDCBinderCustomization>();
	}

	//~ IPropertyTypeCustomization
	virtual void CustomizeHeader(TSharedRef<IPropertyHandle> StructPropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& CustomizationUtils) override;
	virtual void CustomizeChildren(TSharedRef<IPropertyHandle> StructPropertyHandle, IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& CustomizationUtils) override;
	//~ End IPropertyTypeCustomization

private:
	/** Syncs bindings with the channel on every edited instance and collects the channel variable names (for stale-row marking). */
	void SyncBindings(const TSharedRef<IPropertyHandle>& StructPropertyHandle, TSet<FName>& OutChannelVarNames);

	/** Banner naming the Event Data Type as the cause when it is what invalidated the bindings; hidden otherwise. */
	void BuildEventDataTypeWarningRow(IDetailChildrenBuilder& ChildBuilder, const TSharedRef<IPropertyHandle>& StructPropertyHandle) const;

	/** The access context's own input fields: a native value editor and a Bind button per field. */
	void BuildContextRows(IDetailChildrenBuilder& ChildBuilder, const TSharedRef<IPropertyHandle>& StructPropertyHandle) const;

	/** Constant-mode editor: the value property matching the binding type (or an enum dropdown). */
	TSharedRef<SWidget> BuildValueEditor(TSharedRef<IPropertyHandle> BindingHandle, ENDCVariableType Type) const;

	/** Bind dropdown listing the owning class's functions valid for the binding type (UMG-style). */
	TSharedRef<SWidget> BuildBindButton(TSharedRef<IPropertyHandle> StructHandle, TSharedRef<IPropertyHandle> BindingHandle, ENDCVariableType Type) const;

	void OnChannelPropertyChanged();

	TWeakPtr<IPropertyHandle> WeakStructHandle;
	TSharedPtr<IPropertyUtilities> PropertyUtilities;
	bool bRefreshing = false;
};
