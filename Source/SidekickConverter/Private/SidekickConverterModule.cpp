#include "Modules/ModuleManager.h"

#if WITH_EDITOR
#include "SSidekickConverterPanel.h"
#include "ToolMenus.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Brushes/SlateImageBrush.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"

static const FName SidekickConverterTabName(TEXT("SidekickConverter"));
static const FName SidekickConverterStyleName(TEXT("SidekickConverterStyle"));
static const FName SidekickConverterIconName(TEXT("SidekickConverter.Icon"));

// Editor entry point: registers the dockable panel and the Tools menu entry that opens it.
// The conversion runs in a headless editor process the panel launches, so the live editor
// never renders ~165 part thumbnails (which exhausts video memory) or shows a per-part
// import dialog.
class FSidekickConverterModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// The toolkit's database subsystem holds Synty_Sidekick.db open for the whole session
		// and can't be made to release it, so a conversion can't write its color schemes in.
		// It leaves them in a manifest instead, and this applies them here at module load,
		// before that subsystem initializes and opens the file.
		ApplyPendingColorSchemes();

		RegisterStyle();

		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
			SidekickConverterTabName,
			FOnSpawnTab::CreateRaw(this, &FSidekickConverterModule::SpawnTab))
			.SetDisplayName(FText::FromString(TEXT("Sidekick Converter")))
			.SetTooltipText(FText::FromString(TEXT("Convert a Synty Sidekick Unity pack into Unreal parts.")))
			.SetIcon(FSlateIcon(SidekickConverterStyleName, SidekickConverterIconName))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

		UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FSidekickConverterModule::RegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(SidekickConverterTabName);
		}
		if (StyleSet.IsValid())
		{
			FSlateStyleRegistry::UnRegisterSlateStyle(*StyleSet);
			StyleSet.Reset();
		}
	}

private:
	TSharedPtr<FSlateStyleSet> StyleSet;

	void ApplyPendingColorSchemes()
	{
		const FString Manifest = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SidekickConverter"), TEXT("schemes.txt"));
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SidekickConverter"));
		if (!Plugin.IsValid() || !FPaths::FileExists(Manifest))
		{
			return;
		}
		const FString PythonExe = FPaths::Combine(FPaths::EngineDir(), TEXT("Binaries"), TEXT("ThirdParty"), TEXT("Python3"), TEXT("Win64"), TEXT("python.exe"));
		const FString Registrar = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Python"), TEXT("register_color_schemes.py"));
		const FString Database = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Synty/SidekickCharacters/Database/Synty_Sidekick.db"));
		if (!FPaths::FileExists(PythonExe) || !FPaths::FileExists(Registrar))
		{
			return;
		}

		const FString Args = FString::Printf(TEXT("\"%s\" \"%s\" \"%s\""), *Registrar, *Manifest, *Database);
		int32 ReturnCode = -1;
		FString StdOut, StdErr;
		FPlatformProcess::ExecProcess(*PythonExe, *Args, &ReturnCode, &StdOut, &StdErr);
		if (ReturnCode == 0)
		{
			IFileManager::Get().Delete(*Manifest);
			UE_LOG(LogTemp, Display, TEXT("[SidekickConverter] applied pending color schemes"));
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[SidekickConverter] color scheme apply failed (code %d): %s"), ReturnCode, *StdErr);
		}
	}

	void RegisterStyle()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SidekickConverter"));
		if (!Plugin.IsValid())
		{
			return;
		}
		StyleSet = MakeShareable(new FSlateStyleSet(SidekickConverterStyleName));
		StyleSet->SetContentRoot(Plugin->GetBaseDir() / TEXT("Resources"));
		StyleSet->Set(SidekickConverterIconName,
			new FSlateVectorImageBrush(StyleSet->RootToContentDir(TEXT("Icon"), TEXT(".svg")), FVector2D(16.0f, 16.0f)));
		FSlateStyleRegistry::RegisterSlateStyle(*StyleSet);
	}

	TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& /*Args*/)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(SSidekickConverterPanel)
			];
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);
		UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
		FToolMenuSection& Section = ToolsMenu->FindOrAddSection("Synty");
		Section.AddMenuEntry(
			"OpenSidekickConverter",
			FText::FromString(TEXT("Sidekick Converter")),
			FText::FromString(TEXT("Open the Synty Sidekick pack converter panel.")),
			FSlateIcon(SidekickConverterStyleName, SidekickConverterIconName),
			FUIAction(FExecuteAction::CreateRaw(this, &FSidekickConverterModule::OpenTab)));
	}

	void OpenTab()
	{
		FGlobalTabmanager::Get()->TryInvokeTab(SidekickConverterTabName);
	}
};

IMPLEMENT_MODULE(FSidekickConverterModule, SidekickConverter);

#else
IMPLEMENT_MODULE(FDefaultModuleImpl, SidekickConverter);
#endif
