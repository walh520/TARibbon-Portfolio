#include "Modules/ModuleManager.h"
#include "TARibbonComponent.h"

#include "Application/ThrottleManager.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "TickableEditorObject.h"
#include "UObject/ObjectSaveContext.h"

/** Only manages editor lifecycle/UI. Simulation remains in the one-per-world runtime scheduler. */
class FTARibbonEditorModule final : public IModuleInterface, public FTickableEditorObject
{
	struct FPreviewEntry
	{
		TWeakObjectPtr<AActor> Owner;
		FName ComponentName;
		TWeakObjectPtr<UTARibbonComponent> LastComponent;
		bool bPaused = false;
		bool bResetRequired = false;
		bool bHasSourceTransform = false;
		FTransform SourceTransform = FTransform::Identity;
	};

public:
	virtual void StartupModule() override
	{
		bStarted = true;
		UTARibbonComponent::OnEditorPreviewRequestChanged().AddRaw(this, &FTARibbonEditorModule::OnRequest);
		FEditorDelegates::PreSaveWorldWithContext.AddRaw(this, &FTARibbonEditorModule::OnPreSave);
		FEditorDelegates::PostSaveWorldWithContext.AddRaw(this, &FTARibbonEditorModule::OnPostSave);
		FEditorDelegates::PreBeginPIE.AddRaw(this, &FTARibbonEditorModule::OnPreBeginPIE);
		FEditorDelegates::EndPIE.AddRaw(this, &FTARibbonEditorModule::OnEndPIE);
		FWorldDelegates::OnWorldCleanup.AddRaw(this, &FTARibbonEditorModule::OnWorldCleanup);
	}

	virtual void ShutdownModule() override
	{
		bStarted = false;
		UTARibbonComponent::OnEditorPreviewRequestChanged().RemoveAll(this);
		FEditorDelegates::PreSaveWorldWithContext.RemoveAll(this);
		FEditorDelegates::PostSaveWorldWithContext.RemoveAll(this);
		FEditorDelegates::PreBeginPIE.RemoveAll(this);
		FEditorDelegates::EndPIE.RemoveAll(this);
		FWorldDelegates::OnWorldCleanup.RemoveAll(this);
		for (const FPreviewEntry& Entry : Entries)
		{
			if (UTARibbonComponent* Component = Resolve(Entry)) { Component->StopEditorPreview(); }
		}
		Entries.Reset();
		RefreshViewportOverrides();
	}

	virtual bool IsTickable() const override { return bStarted && !IsRunningCommandlet(); }
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(FTARibbonEditorModule, STATGROUP_Tickables); }
	virtual void Tick(float DeltaTime) override
	{
		if (!GEditor) { return; }
		if (!bInPIE && SaveDepth == 0)
		{
			// Start may broadcast OnRequest; iterate a copy rather than a reallocatable array.
			const TArray<FPreviewEntry> Pending = Entries;
			for (const FPreviewEntry& Entry : Pending)
			{
				UTARibbonComponent* Component = Resolve(Entry);
				if (!Component || !Component->IsRegistered()) { continue; }
				FTransform NewSourceTransform;
				const bool bMoved = Entry.bHasSourceTransform && Component->GetEditorPreviewSourceTransform(NewSourceTransform) &&
					!NewSourceTransform.Equals(Entry.SourceTransform, 1.0e-4);
				if (bMoved && !Component->IsEditorPreviewResetRequired()) { Component->RestoreEditorPreviewRequest(true); }
				if (!Component->IsEditorPreviewRequested())
				{
					// RerunConstructionScripts replaced the controller. Restore intent, not GPU history.
					Component->bPaused = Entry.bPaused;
					Component->RestoreEditorPreviewRequest(Entry.bResetRequired || bMoved);
				}
				Component->SetEditorPreviewSuspended(false);
				if (FPreviewEntry* Stored = FindEntry(Component))
				{
					Stored->LastComponent = Component;
					Stored->bPaused = Component->bPaused;
					Stored->bResetRequired = Component->IsEditorPreviewResetRequired();
				}
			}
		}
		// A removed component must not silently restart if a later component reuses its name.
		Entries.RemoveAll([](const FPreviewEntry& Entry)
		{
			return !Entry.Owner.IsValid() || !Resolve(Entry);
		});
		RefreshViewportOverrides();
	}

private:
	static UTARibbonComponent* Resolve(const FPreviewEntry& Entry)
	{
		AActor* Owner = Entry.Owner.Get();
		if (!IsValid(Owner) || !Owner->GetWorld() || Owner->GetWorld()->WorldType != EWorldType::Editor) { return nullptr; }
		TInlineComponentArray<UTARibbonComponent*> Components;
		Owner->GetComponents(Components);
		for (UTARibbonComponent* Component : Components)
		{
			if (IsValid(Component) && Component->GetFName() == Entry.ComponentName) { return Component; }
		}
		return nullptr;
	}

	FPreviewEntry* FindEntry(UTARibbonComponent* Component)
	{
		return Entries.FindByPredicate([Component](const FPreviewEntry& Entry)
		{
			return Entry.Owner.Get() == Component->GetOwner() && Entry.ComponentName == Component->GetFName();
		});
	}

	void OnRequest(UTARibbonComponent* Component, bool bRequested)
	{
		if (!IsValid(Component) || !Component->GetOwner()) { return; }
		if (bRequested)
		{
			if (!FindEntry(Component)) { Entries.Add({ Component->GetOwner(), Component->GetFName(), Component, Component->bPaused }); }
			FPreviewEntry* Entry = FindEntry(Component);
			Entry->bPaused = Component->bPaused;
			Entry->bResetRequired = Component->IsEditorPreviewResetRequired();
			if (!Entry->bHasSourceTransform || (Component->IsEditorPreviewActive() && !Entry->bResetRequired))
			{
				Entry->bHasSourceTransform = Component->GetEditorPreviewSourceTransform(Entry->SourceTransform);
			}
			if (bInPIE || SaveDepth > 0) { Component->SetEditorPreviewSuspended(true); }
		}
		else
		{
			Entries.RemoveAll([Component](const FPreviewEntry& Entry)
			{
				return Entry.Owner.Get() == Component->GetOwner() && Entry.ComponentName == Component->GetFName();
			});
		}
		RefreshViewportOverrides();
	}

	void SuspendAll()
	{
		for (FPreviewEntry& Entry : Entries)
		{
			if (UTARibbonComponent* Component = Resolve(Entry))
			{
				Entry.bPaused = Component->bPaused;
				Entry.bResetRequired = Component->IsEditorPreviewResetRequired();
				Component->SetEditorPreviewSuspended(true);
			}
		}
		RefreshViewportOverrides();
	}

	void OnPreSave(UWorld*, FObjectPreSaveContext) { ++SaveDepth; SuspendAll(); }
	void OnPostSave(UWorld*, FObjectPostSaveContext) { SaveDepth = FMath::Max(0, SaveDepth - 1); }
	void OnPreBeginPIE(bool) { bInPIE = true; SuspendAll(); }
	void OnEndPIE(bool) { bInPIE = false; } // Resume next editor tick, after PIE teardown.
	void OnWorldCleanup(UWorld* World, bool, bool)
	{
		const auto Previous = Entries;
		for (const FPreviewEntry& Entry : Previous)
		{
			if (AActor* Owner = Entry.Owner.Get(); Owner && Owner->GetWorld() == World)
			{
				if (UTARibbonComponent* Component = Resolve(Entry)) { Component->StopEditorPreview(); }
			}
		}
		Entries.RemoveAll([World](const FPreviewEntry& Entry) { return !Entry.Owner.IsValid() || Entry.Owner->GetWorld() == World; });
		RefreshViewportOverrides();
	}

	void RefreshViewportOverrides()
	{
		TSet<UWorld*> PreviewWorlds;
		if (bStarted && !bInPIE && SaveDepth == 0)
		{
			for (const FPreviewEntry& Entry : Entries)
			{
				if (UTARibbonComponent* Component = Resolve(Entry); Component && Component->IsEditorPreviewActive())
				{
					PreviewWorlds.Add(Component->GetWorld());
				}
			}
		}
		TSet<FEditorViewportClient*> LiveClients;
		if (GEditor)
		{
			for (FEditorViewportClient* Client : GEditor->GetAllViewportClients())
			{
				if (!Client) { continue; }
				LiveClients.Add(Client);
				const bool bWanted = Client->IsLevelEditorClient() && PreviewWorlds.Contains(Client->GetWorld());
				if (bWanted && !OverriddenClients.Contains(Client))
				{
					Client->AddRealtimeOverride(true, OverrideName());
					OverriddenClients.Add(Client);
				}
				else if (!bWanted && OverriddenClients.Contains(Client))
				{
					Client->RemoveRealtimeOverride(OverrideName(), false);
					OverriddenClients.Remove(Client);
				}
			}
		}
		for (auto It = OverriddenClients.CreateIterator(); It; ++It)
		{
			if (!LiveClients.Contains(*It)) { It.RemoveCurrent(); }
		}
		const bool bWantThrottleExemption = !PreviewWorlds.IsEmpty();
		if (bWantThrottleExemption != bThrottleDisabled)
		{
			FSlateThrottleManager::Get().DisableThrottle(bWantThrottleExemption);
			bThrottleDisabled = bWantThrottleExemption;
		}
	}

	static FText OverrideName() { return NSLOCTEXT("TARibbon", "EditorPreviewRealtime", "TARibbon 布料预览"); }
	TArray<FPreviewEntry> Entries;
	TSet<FEditorViewportClient*> OverriddenClients;
	int32 SaveDepth = 0;
	bool bInPIE = false;
	bool bThrottleDisabled = false;
	bool bStarted = false;
};

IMPLEMENT_MODULE(FTARibbonEditorModule, TARibbonEditor)
