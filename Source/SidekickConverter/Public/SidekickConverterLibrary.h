#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SidekickConverterLibrary.generated.h"

class USkeletalMesh;
class USkeleton;

UCLASS()
class SIDEKICKCONVERTER_API USidekickConverterLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Conforms a freshly-imported Sidekick part onto the shared skeleton and assigns it.
	 *
	 * The raw Unity FBX use a reduced spine, with neck and clavicles parented straight to
	 * spine_03 where SKEL_Default_Sidekick inserts spine_04, spine_05, neck_02 and the
	 * *_twist_02_* bones. That parent mismatch is what makes a naively-imported part fail to
	 * merge. This rebuilds the part's reference skeleton as the shared skeleton's canonical
	 * bones, in canonical order, then appends the part's own *_dyn_* jiggle bones and remaps
	 * the skin weights by bone name.
	 *
	 * OrientationReference is a shipped HUMN_BASE part at the same rest pose as the parts
	 * being converted. The raw Unity rig orients some bones (right-side chain, pelvis, feet,
	 * root) with a different roll or axis than the canonical skeleton the animations are
	 * authored for, so those joints twist once an animation drives them. Each shared bone is
	 * given the reference's full rest transform, position and orientation both, so every
	 * converted part lands on one identical skeleton and the twist is gone. Body shape comes
	 * from the mesh and morphs, not these bone positions, so proportions are unchanged. Pass
	 * null to keep the part's own bone transforms instead.
	 *
	 * Returns false on a null mesh or skeleton, or a part with no editor mesh data.
	 */
	UFUNCTION(BlueprintCallable, Category = "Sidekick Converter")
	static bool ConformToSharedSkeleton(USkeletalMesh* Mesh, USkeleton* SharedSkeleton, USkeletalMesh* OrientationReference);
};
