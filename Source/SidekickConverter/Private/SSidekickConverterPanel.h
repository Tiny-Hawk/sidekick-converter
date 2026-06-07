#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "HAL/PlatformProcess.h"
#include "Containers/Ticker.h"

class SEditableTextBox;

/**
 * Dockable panel for the converter. The user points it at one or more Unity packs and the
 * shared assets to conform onto (defaulted to a free-pack install), then Convert launches
 * the headless conversion process and the panel polls its progress file to drive the bar.
 */
class SSidekickConverterPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSidekickConverterPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SSidekickConverterPanel() override;

private:
	enum class EJobState : uint8 { Idle, Converting, Done };

	FReply OnBrowseClicked();
	FReply OnConvertClicked();
	FReply OnConvertAnotherClicked();
	FReply OnShowInContentBrowserClicked();
	FReply OnCloseClicked();
	bool PollProgress(float DeltaTime);
	void FinishJob(bool bSuccess);
	void PromptRestartToApplyColors();

	bool DependenciesPresent() const;
	bool CanConvert() const;
	FText GetStatusText() const;
	FText GetPackageSummary() const;
	FText GetDependencyText() const;
	FSlateColor GetDependencyColor() const;
	TOptional<float> GetProgressFraction() const;
	EVisibility GetConvertVisibility() const;
	EVisibility GetCompletionVisibility() const;

	TSharedPtr<SEditableTextBox> SkeletonBox;
	TSharedPtr<SEditableTextBox> ReferenceBox;
	TSharedPtr<SEditableTextBox> MaterialBox;

	TArray<FString> PackagePaths;
	EJobState State = EJobState::Idle;

	FProcHandle JobProcess;
	FTSTicker::FDelegateHandle PollHandle;
	FString ProgressFilePath;
	FText StatusText;
	float ProgressFraction = 0.0f;
	bool bSawDone = false;
};
