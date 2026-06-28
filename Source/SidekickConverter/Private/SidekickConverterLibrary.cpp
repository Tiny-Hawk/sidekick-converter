#include "SidekickConverterLibrary.h"

#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"
#include "MeshDescription.h"
#include "SkeletalMeshAttributes.h"
#include "BoneWeights.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsAssetUtils.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "AssetRegistry/AssetRegistryModule.h"

using UE::AnimationCore::FBoneWeight;
using UE::AnimationCore::FBoneWeights;

#if WITH_EDITOR
namespace
{
	struct FConformedBone
	{
		FName Name;
		FName ParentName;
		FTransform LocalPose;
	};

	// Build the conformed bone list: the shared skeleton's bones in canonical order, then
	// the part's own *_dyn_* chains appended after.
	//
	// Every canonical bone takes the reference part's rest transform rather than the part's
	// own. Exported bone positions carry noise, enough that two parts disagree by a hair on
	// where the elbow or wrist sits and visibly separate once assembled onto one body.
	// Pinning every part to the reference's transforms removes that drift. Only the dyn
	// bones stay part-local. With no reference the part keeps its own transforms.
	void ComputeConformedBones(const FReferenceSkeleton& OldMeshRef, const USkeleton* Shared, const FReferenceSkeleton* OrientRef, TArray<FConformedBone>& Out)
	{
		const FReferenceSkeleton& Canon = Shared->GetReferenceSkeleton();
		const int32 CanonNum = Canon.GetRawBoneNum();

		// Reference part's bones in world space, keyed by name.
		TMap<FName, FTransform> RefWorld;
		if (OrientRef)
		{
			TArray<FTransform> RW;
			RW.SetNum(OrientRef->GetRawBoneNum());
			for (int32 i = 0; i < OrientRef->GetRawBoneNum(); ++i)
			{
				const int32 Parent = OrientRef->GetParentIndex(i);
				RW[i] = (Parent != INDEX_NONE) ? OrientRef->GetRefBonePose()[i] * RW[Parent] : OrientRef->GetRefBonePose()[i];
				RefWorld.Add(OrientRef->GetBoneName(i), RW[i]);
			}
		}

		// The part's own world transforms, used to place dyn bones and as a fallback when
		// the reference lacks a bone or none was supplied.
		TMap<FName, FTransform> PartWorld;
		for (int32 i = 0; i < OldMeshRef.GetRawBoneNum(); ++i)
		{
			const int32 Parent = OldMeshRef.GetParentIndex(i);
			const FTransform Local = OldMeshRef.GetRefBonePose()[i];
			const FTransform World = (Parent != INDEX_NONE) ? Local * PartWorld[OldMeshRef.GetBoneName(Parent)] : Local;
			PartWorld.Add(OldMeshRef.GetBoneName(i), World);
		}

		TMap<FName, FTransform> TargetWorld;
		TSet<FName> Placed;
		for (int32 i = 0; i < CanonNum; ++i)
		{
			const FName Name = Canon.GetBoneName(i);
			const int32 CanonParent = Canon.GetParentIndex(i);
			const FName ParentName = (CanonParent != INDEX_NONE) ? Canon.GetBoneName(CanonParent) : NAME_None;
			const FTransform ParentWorld = ParentName.IsNone() ? FTransform::Identity : TargetWorld.FindRef(ParentName);

			FTransform World;
			if (const FTransform* Ref = RefWorld.Find(Name))
			{
				World = *Ref;
			}
			else if (const FTransform* PartW = PartWorld.Find(Name))
			{
				World = *PartW;
			}
			else
			{
				World = Canon.GetRefBonePose()[i] * ParentWorld;
			}
			TargetWorld.Add(Name, World);
			Placed.Add(Name);

			Out.Add({ Name, ParentName, World.GetRelativeTransform(ParentWorld) });
		}

		// Part-only bones (its *_dyn_* chains). Skip any whose parent isn't already
		// placed (e.g. the part's own root bone), so the result has a single root.
		for (int32 i = 0; i < OldMeshRef.GetRawBoneNum(); ++i)
		{
			const FName Name = OldMeshRef.GetBoneName(i);
			if (Canon.FindBoneIndex(Name) != INDEX_NONE)
			{
				continue;
			}
			const int32 Parent = OldMeshRef.GetParentIndex(i);
			const FName ParentName = (Parent != INDEX_NONE) ? OldMeshRef.GetBoneName(Parent) : NAME_None;
			if (ParentName.IsNone() || !Placed.Contains(ParentName))
			{
				continue;
			}
			Placed.Add(Name);
			Out.Add({ Name, ParentName, OldMeshRef.GetRefBonePose()[i] });
		}
	}

	FReferenceSkeleton BuildRefSkeleton(const TArray<FConformedBone>& Bones, const USkeleton* Shared)
	{
		FReferenceSkeleton NewRef;
		{
			FReferenceSkeletonModifier Modifier(NewRef, Shared);
			// Track indices ourselves; FindBoneIndex is unreliable mid-modify. Bones are
			// already ordered parent-before-child, so a parent is always present here.
			TMap<FName, int32> Index;
			for (int32 i = 0; i < Bones.Num(); ++i)
			{
				const FConformedBone& Bone = Bones[i];
				int32 ParentIdx = INDEX_NONE;
				if (!Bone.ParentName.IsNone())
				{
					if (const int32* Found = Index.Find(Bone.ParentName))
					{
						ParentIdx = *Found;
					}
				}
				Modifier.Add(FMeshBoneInfo(Bone.Name, Bone.Name.ToString(), ParentIdx), Bone.LocalPose);
				Index.Add(Bone.Name, i);
			}
		}
		return NewRef;
	}

	struct FCapturedInfluence
	{
		FName BoneName;
		float Weight;
	};
}
#endif

bool USidekickConverterLibrary::ConformToSharedSkeleton(USkeletalMesh* Mesh, USkeleton* SharedSkeleton, USkeletalMesh* OrientationReference)
{
#if WITH_EDITOR
	if (!Mesh || !SharedSkeleton)
	{
		return false;
	}

	const FReferenceSkeleton OldMeshRef = Mesh->GetRefSkeleton();
	const FReferenceSkeleton* OrientRef = OrientationReference ? &OrientationReference->GetRefSkeleton() : nullptr;

	TArray<FConformedBone> ConformedBones;
	ComputeConformedBones(OldMeshRef, SharedSkeleton, OrientRef, ConformedBones);

	TMap<FName, FName> NameToParent;
	for (const FConformedBone& Bone : ConformedBones)
	{
		NameToParent.Add(Bone.Name, Bone.ParentName);
	}

	const int32 LODNum = Mesh->GetLODNum();
	Mesh->Modify();

	for (int32 LODIndex = 0; LODIndex < LODNum; ++LODIndex)
	{
		if (!Mesh->HasMeshDescription(LODIndex))
		{
			continue;
		}

		Mesh->ModifyMeshDescription(LODIndex);
		FMeshDescription* MeshDescription = Mesh->GetMeshDescription(LODIndex);
		if (!MeshDescription)
		{
			continue;
		}

		FSkeletalMeshAttributes Attributes(*MeshDescription);

		TArray<FBoneID> OldBoneIDs;
		TArray<FName> OldBoneNames;
		for (const FBoneID BoneID : Attributes.Bones().GetElementIDs())
		{
			OldBoneIDs.Add(BoneID);
			OldBoneNames.Add(Attributes.GetBoneNames().Get(BoneID));
		}

		FSkinWeightsVertexAttributesRef SkinWeights = Attributes.GetVertexSkinWeights();
		TMap<FVertexID, TArray<FCapturedInfluence>> CapturedWeights;
		CapturedWeights.Reserve(MeshDescription->Vertices().Num());
		for (const FVertexID VertexID : MeshDescription->Vertices().GetElementIDs())
		{
			TArray<FCapturedInfluence> Influences;
			for (const FBoneWeight BoneWeight : SkinWeights.Get(VertexID))
			{
				const int32 OldPos = BoneWeight.GetBoneIndex();
				if (OldBoneNames.IsValidIndex(OldPos))
				{
					Influences.Add({ OldBoneNames[OldPos], BoneWeight.GetWeight() });
				}
			}
			CapturedWeights.Add(VertexID, MoveTemp(Influences));
		}

		for (const FBoneID BoneID : OldBoneIDs)
		{
			Attributes.DeleteBone(BoneID);
		}
		for (const FConformedBone& Bone : ConformedBones)
		{
			const FBoneID BoneID = Attributes.CreateBone();
			Attributes.GetBoneNames().Set(BoneID, Bone.Name);
			Attributes.GetBonePoses().Set(BoneID, Bone.LocalPose);
		}

		TMap<FName, int32> NameToPos;
		{
			int32 Pos = 0;
			for (const FBoneID BoneID : Attributes.Bones().GetElementIDs())
			{
				NameToPos.Add(Attributes.GetBoneNames().Get(BoneID), Pos++);
			}
		}
		for (const FBoneID BoneID : Attributes.Bones().GetElementIDs())
		{
			const FName ParentName = NameToParent.FindRef(Attributes.GetBoneNames().Get(BoneID));
			const int32 ParentPos = ParentName.IsNone() ? INDEX_NONE : NameToPos.FindChecked(ParentName);
			Attributes.GetBoneParentIndices().Set(BoneID, ParentPos);
		}
		for (const FVertexID VertexID : MeshDescription->Vertices().GetElementIDs())
		{
			TArray<FBoneWeight> Influences;
			for (const FCapturedInfluence& Captured : CapturedWeights.FindChecked(VertexID))
			{
				if (const int32* NewPos = NameToPos.Find(Captured.BoneName))
				{
					Influences.Add(FBoneWeight(static_cast<FBoneIndexType>(*NewPos), Captured.Weight));
				}
			}
			SkinWeights.Set(VertexID, FBoneWeights::Create(Influences));
		}

		Mesh->CommitMeshDescription(LODIndex);
	}

	Mesh->SetRefSkeleton(BuildRefSkeleton(ConformedBones, SharedSkeleton));
	Mesh->SetSkeleton(SharedSkeleton);
	Mesh->Build();
	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();
	return true;
#else
	return false;
#endif
}

UPhysicsAsset* USidekickConverterLibrary::CreatePhysicsAssetForMesh(USkeletalMesh* Mesh)
{
#if WITH_EDITOR
	if (!Mesh)
	{
		return nullptr;
	}

	// Name and place the asset as <MeshName>_PhysicsAsset alongside the mesh, matching the
	// per-mesh physics assets the official Unreal Sidekick packs ship. The Python caller deletes
	// any prior asset of this name first, so here we always build a fresh one.
	const FString MeshPackageName = Mesh->GetOutermost()->GetName();
	const FString PackagePath = FPackageName::GetLongPackagePath(MeshPackageName);
	const FString AssetName = Mesh->GetName() + TEXT("_PhysicsAsset");
	const FString NewPackageName = PackagePath + TEXT("/") + AssetName;

	UPackage* Package = CreatePackage(*NewPackageName);
	if (!Package)
	{
		return nullptr;
	}

	UPhysicsAsset* PhysicsAsset = NewObject<UPhysicsAsset>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!PhysicsAsset)
	{
		return nullptr;
	}

	// Default parameters mirror the editor's "Create Physics Asset" auto-generation: one body per
	// skinned bone above the minimum size, sized from the bone's vertices. bSetToMesh assigns it
	// to the mesh's Physics Asset slot. Run after the conform so bodies fit the shared skeleton.
	FPhysAssetCreateParams Params;
	FText ErrorMessage;
	// bShowProgress=false: the conversion runs headless (-unattended -nullrhi), so no Slate dialog.
	if (!FPhysicsAssetUtils::CreateFromSkeletalMesh(PhysicsAsset, Mesh, Params, ErrorMessage, /*bSetToMesh=*/true, /*bShowProgress=*/false))
	{
		return nullptr;
	}

	FAssetRegistryModule::AssetCreated(PhysicsAsset);
	PhysicsAsset->MarkPackageDirty();
	Mesh->MarkPackageDirty();
	return PhysicsAsset;
#else
	return nullptr;
#endif
}
