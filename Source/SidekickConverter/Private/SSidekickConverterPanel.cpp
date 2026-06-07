#include "SSidekickConverterPanel.h"

#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Interfaces/IPluginManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Misc/MessageDialog.h"
#include "UnrealEdMisc.h"

namespace
{
	const FName SidekickConverterTabName(TEXT("SidekickConverter"));
	const TCHAR* PartsFolder = TEXT("/Game/Synty/SidekickCharacters/Resources/Meshes/Outfits");
	const TCHAR* DefaultSkeleton = TEXT("/Game/Synty/SidekickCharacters/Resources/Skeletons/SKEL_Default_Sidekick");
	const TCHAR* DefaultReference = TEXT("/Game/Synty/SidekickCharacters/Resources/Meshes/Species/Humans/SK_HUMN_BASE_01_10TORS_HU01");
	const TCHAR* DefaultMaterial = TEXT("/Game/Synty/SidekickCharacters/Resources/Materials/M_Default_Sidekick");

	TSharedRef<SWidget> MakePathRow(const FString& Label, TSharedPtr<SEditableTextBox>& OutBox, const FString& DefaultValue)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(0.30f).VAlign(VAlign_Center)
			[ SNew(STextBlock).Text(FText::FromString(Label)) ]
			+ SHorizontalBox::Slot().FillWidth(0.70f).VAlign(VAlign_Center)
			[ SAssignNew(OutBox, SEditableTextBox).Text(FText::FromString(DefaultValue)) ];
	}
}

void SSidekickConverterPanel::Construct(const FArguments& InArgs)
{
	StatusText = FText::FromString(TEXT("Idle."));

	ChildSlot
	[
		SNew(SBorder).Padding(12.0f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Convert one or more Synty Sidekick Unity packs into Unreal parts on the shared skeleton.")))
				.AutoWrapText(true)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(0.30f).VAlign(VAlign_Center)
				[ SNew(STextBlock).Text(FText::FromString(TEXT("Unity pack(s)"))) ]
				+ SHorizontalBox::Slot().FillWidth(0.55f).VAlign(VAlign_Center)
				[ SNew(STextBlock).Text(this, &SSidekickConverterPanel::GetPackageSummary) ]
				+ SHorizontalBox::Slot().FillWidth(0.15f).Padding(4, 0, 0, 0)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Browse...")))
					.OnClicked(this, &SSidekickConverterPanel::OnBrowseClicked)
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)[ MakePathRow(TEXT("Conform to skeleton"), SkeletonBox, DefaultSkeleton) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)[ MakePathRow(TEXT("Orientation reference part"), ReferenceBox, DefaultReference) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)[ MakePathRow(TEXT("Shared material"), MaterialBox, DefaultMaterial) ]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 4)
			[
				SNew(STextBlock)
				.Text(this, &SSidekickConverterPanel::GetDependencyText)
				.ColorAndOpacity(this, &SSidekickConverterPanel::GetDependencyColor)
				.AutoWrapText(true)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				SNew(SButton)
				.HAlign(HAlign_Center)
				.Text(FText::FromString(TEXT("Convert")))
				.Visibility(this, &SSidekickConverterPanel::GetConvertVisibility)
				.IsEnabled(this, &SSidekickConverterPanel::CanConvert)
				.OnClicked(this, &SSidekickConverterPanel::OnConvertClicked)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 2)
			[ SNew(SProgressBar).Percent(this, &SSidekickConverterPanel::GetProgressFraction) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[ SNew(STextBlock).Text(this, &SSidekickConverterPanel::GetStatusText) ]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
			[
				SNew(SHorizontalBox)
				.Visibility(this, &SSidekickConverterPanel::GetCompletionVisibility)
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 0, 4, 0)
				[
					SNew(SButton).HAlign(HAlign_Center)
					.Text(FText::FromString(TEXT("Show parts")))
					.ToolTipText(FText::FromString(TEXT("Reveal the converted parts in the Content Browser.")))
					.OnClicked(this, &SSidekickConverterPanel::OnShowInContentBrowserClicked)
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(4, 0)
				[
					SNew(SButton).HAlign(HAlign_Center)
					.Text(FText::FromString(TEXT("Convert another pack")))
					.OnClicked(this, &SSidekickConverterPanel::OnConvertAnotherClicked)
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(4, 0, 0, 0)
				[
					SNew(SButton).HAlign(HAlign_Center)
					.Text(FText::FromString(TEXT("Close")))
					.OnClicked(this, &SSidekickConverterPanel::OnCloseClicked)
				]
			]
		]
	];
}

SSidekickConverterPanel::~SSidekickConverterPanel()
{
	if (PollHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PollHandle);
		PollHandle.Reset();
	}
	if (JobProcess.IsValid())
	{
		FPlatformProcess::CloseProc(JobProcess);
	}
}

FReply SSidekickConverterPanel::OnBrowseClicked()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (DesktopPlatform)
	{
		TArray<FString> ChosenFiles;
		const void* ParentWindow = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
		const bool bPicked = DesktopPlatform->OpenFileDialog(
			ParentWindow,
			TEXT("Select one or more Synty Sidekick Unity packs"),
			TEXT(""), TEXT(""),
			TEXT("Unity package (*.unitypackage)|*.unitypackage"),
			EFileDialogFlags::Multiple,
			ChosenFiles);
		if (bPicked)
		{
			PackagePaths = ChosenFiles;
		}
	}
	return FReply::Handled();
}

bool SSidekickConverterPanel::DependenciesPresent() const
{
	if (!SkeletonBox.IsValid() || !ReferenceBox.IsValid() || !MaterialBox.IsValid())
	{
		return false;
	}
	return FPackageName::DoesPackageExist(SkeletonBox->GetText().ToString())
		&& FPackageName::DoesPackageExist(ReferenceBox->GetText().ToString())
		&& FPackageName::DoesPackageExist(MaterialBox->GetText().ToString());
}

bool SSidekickConverterPanel::CanConvert() const
{
	return State == EJobState::Idle && PackagePaths.Num() > 0 && DependenciesPresent();
}

FText SSidekickConverterPanel::GetPackageSummary() const
{
	if (PackagePaths.Num() == 0)
	{
		return FText::FromString(TEXT("(none selected)"));
	}
	if (PackagePaths.Num() == 1)
	{
		return FText::FromString(FPaths::GetCleanFilename(PackagePaths[0]));
	}
	return FText::FromString(FString::Printf(TEXT("%d packs selected"), PackagePaths.Num()));
}

FText SSidekickConverterPanel::GetDependencyText() const
{
	return DependenciesPresent()
		? FText::FromString(TEXT("Sidekick free-pack assets found."))
		: FText::FromString(TEXT("Shared skeleton, reference, or material not found at those paths. Install the Sidekick free pack or fix the paths."));
}

FSlateColor SSidekickConverterPanel::GetDependencyColor() const
{
	return DependenciesPresent() ? FSlateColor(FLinearColor(0.3f, 0.85f, 0.3f)) : FSlateColor(FLinearColor(0.95f, 0.7f, 0.2f));
}

TOptional<float> SSidekickConverterPanel::GetProgressFraction() const
{
	// Negative means indeterminate (a marquee bar) for the boot/extract phase before
	// the first part is imported.
	return ProgressFraction < 0.0f ? TOptional<float>() : TOptional<float>(ProgressFraction);
}

FText SSidekickConverterPanel::GetStatusText() const
{
	return StatusText;
}

EVisibility SSidekickConverterPanel::GetConvertVisibility() const
{
	return State == EJobState::Done ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility SSidekickConverterPanel::GetCompletionVisibility() const
{
	return State == EJobState::Done ? EVisibility::Visible : EVisibility::Collapsed;
}

FReply SSidekickConverterPanel::OnConvertClicked()
{
	if (!CanConvert())
	{
		return FReply::Handled();
	}

	const FString JobDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SidekickConverter"));
	IFileManager::Get().MakeDirectory(*JobDir, true);

	FString Job;
	for (const FString& Package : PackagePaths)
	{
		Job += FString::Printf(TEXT("PACKAGE=%s\n"), *Package);
	}
	Job += FString::Printf(TEXT("SKELETON=%s\nREFERENCE=%s\nMATERIAL=%s\n"),
		*SkeletonBox->GetText().ToString(), *ReferenceBox->GetText().ToString(), *MaterialBox->GetText().ToString());
	FFileHelper::SaveStringToFile(Job, *FPaths::Combine(JobDir, TEXT("job.txt")));

	ProgressFilePath = FPaths::Combine(JobDir, TEXT("progress.txt"));
	FFileHelper::SaveStringToFile(TEXT("0\t0\tstarting"), *ProgressFilePath);

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SidekickConverter"));
	if (!Plugin.IsValid())
	{
		return FReply::Handled();
	}
	const FString Script = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Python"), TEXT("convert_sidekick_pack.py"));
	const FString CommandletExe = FPaths::Combine(FPaths::EngineDir(), TEXT("Binaries"), TEXT("Win64"), TEXT("UnrealEditor-Cmd.exe"));
	const FString Project = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	const FString Args = FString::Printf(
		TEXT("\"%s\" -ExecutePythonScript=\"%s\" -unattended -nopause -nosplash -nullrhi"),
		*Project, *Script);

	JobProcess = FPlatformProcess::CreateProc(*CommandletExe, *Args, true, true, false, nullptr, 0, nullptr, nullptr);
	if (!JobProcess.IsValid())
	{
		StatusText = FText::FromString(TEXT("Failed to launch the conversion process."));
		return FReply::Handled();
	}

	State = EJobState::Converting;
	bSawDone = false;
	ProgressFraction = -1.0f;
	StatusText = FText::FromString(TEXT("Launching converter..."));
	PollHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateSP(this, &SSidekickConverterPanel::PollProgress), 0.5f);
	return FReply::Handled();
}

bool SSidekickConverterPanel::PollProgress(float /*DeltaTime*/)
{
	FString Content;
	if (FFileHelper::LoadFileToString(Content, *ProgressFilePath))
	{
		TArray<FString> Fields;
		Content.ParseIntoArray(Fields, TEXT("\t"), false);
		if (Fields.Num() >= 3)
		{
			const int32 Current = FCString::Atoi(*Fields[0]);
			const int32 Total = FCString::Atoi(*Fields[1]);
			const FString& Name = Fields[2];
			if (Name == TEXT("DONE"))
			{
				bSawDone = true;
				ProgressFraction = 1.0f;
				StatusText = FText::FromString(TEXT("Finishing up..."));
			}
			else if (Total <= 0 || Name == TEXT("starting") || Name == TEXT("extracting"))
			{
				ProgressFraction = -1.0f; // marquee
				StatusText = FText::FromString(TEXT("Starting up and extracting pack(s)..."));
			}
			else
			{
				ProgressFraction = static_cast<float>(Current) / static_cast<float>(Total);
				StatusText = FText::FromString(FString::Printf(TEXT("Importing: %s  (%d/%d)"), *Name, Current, Total));
			}
		}
	}

	// Finish only once the conversion editor has fully exited. It holds the database while
	// shutting down, so registering colors before then would be blocked.
	if (!FPlatformProcess::IsProcRunning(JobProcess))
	{
		FinishJob(bSawDone);
		return false;
	}
	return true;
}

void SSidekickConverterPanel::FinishJob(bool bSuccess)
{
	PollHandle.Reset();
	if (JobProcess.IsValid())
	{
		FPlatformProcess::CloseProc(JobProcess);
	}
	State = EJobState::Done;
	ProgressFraction = bSuccess ? 1.0f : 0.0f;
	StatusText = FText::FromString(bSuccess
		? TEXT("Conversion complete. Restart the editor to load the pack's colors.")
		: TEXT("Conversion ended early. Check the log."));

	FAssetRegistryModule& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	AssetRegistry.Get().ScanPathsSynchronous({ PartsFolder }, true);

	if (bSuccess)
	{
		PromptRestartToApplyColors();
	}
}

void SSidekickConverterPanel::PromptRestartToApplyColors()
{
	// The Sidekick toolkit holds the color database open for the whole session and never
	// releases it (a bug in its CloseDatabase), so the schemes can only be written before the
	// toolkit opens the file, at editor startup. The converter left them in a manifest; this
	// offers to restart the editor, which reopens this same project and applies them on the
	// way up. See the converter's startup hook and the README's Limitations section.
	const FString Manifest = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SidekickConverter"), TEXT("schemes.txt"));
	if (!FPaths::FileExists(Manifest))
	{
		return;
	}
	const EAppReturnType::Type Choice = FMessageDialog::Open(
		EAppMsgType::YesNo,
		FText::FromString(TEXT(
			"Conversion complete.\n\n"
			"The pack's colors are ready, but the Sidekick toolkit only loads color schemes when "
			"the editor starts. To finish, the editor needs to restart. This reopens the same "
			"project; you'll be prompted to save any unsaved work first.\n\n"
			"Restart now to load the colors?")));
	if (Choice == EAppReturnType::Yes)
	{
		FUnrealEdMisc::Get().RestartEditor(true);
	}
}

FReply SSidekickConverterPanel::OnConvertAnotherClicked()
{
	PackagePaths.Reset();
	State = EJobState::Idle;
	ProgressFraction = 0.0f;
	StatusText = FText::FromString(TEXT("Idle."));
	return FReply::Handled();
}

FReply SSidekickConverterPanel::OnShowInContentBrowserClicked()
{
	FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	ContentBrowser.Get().SyncBrowserToFolders({ PartsFolder });
	return FReply::Handled();
}

FReply SSidekickConverterPanel::OnCloseClicked()
{
	const TSharedPtr<SDockTab> Tab = FGlobalTabmanager::Get()->FindExistingLiveTab(FTabId(SidekickConverterTabName));
	if (Tab.IsValid())
	{
		Tab->RequestCloseTab();
	}
	return FReply::Handled();
}
