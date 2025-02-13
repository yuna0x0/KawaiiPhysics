#include "KawaiiPhysicsEditMode.h"
#include "SceneManagement.h"
#include "IPersonaPreviewScene.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "EditorModeManager.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "KawaiiPhysics.h"
#include "KawaiiPhysicsCustomExternalForce.h"
#include "KawaiiPhysicsExternalForce.h"
#include "KawaiiPhysicsLimitsDataAsset.h"

#define LOCTEXT_NAMESPACE "KawaiiPhysicsEditMode"
DEFINE_LOG_CATEGORY(LogKawaiiPhysics);

struct HKawaiiPhysicsHitProxy : public HHitProxy
{
	DECLARE_HIT_PROXY()

	HKawaiiPhysicsHitProxy(ECollisionLimitType InType, int32 InIndex, bool InFromDataAsset = false)
		: HHitProxy(HPP_Wireframe)
		  , CollisionType(InType)
		  , CollisionIndex(InIndex)
		  , bFromDataAsset(InFromDataAsset)
	{
	}

	virtual EMouseCursor::Type GetMouseCursor() override
	{
		return EMouseCursor::Crosshairs;
	}

	ECollisionLimitType CollisionType;
	int32 CollisionIndex;
	bool bFromDataAsset;
};

IMPLEMENT_HIT_PROXY(HKawaiiPhysicsHitProxy, HHitProxy);


FKawaiiPhysicsEditMode::FKawaiiPhysicsEditMode()
	: RuntimeNode(nullptr)
	  , GraphNode(nullptr)
	  , bIsSelectCollisionFromDataAsset(false)
	  , CurWidgetMode(UE_WIDGET::EWidgetMode::WM_Translate)
{
}

void FKawaiiPhysicsEditMode::EnterMode(UAnimGraphNode_Base* InEditorNode, FAnimNode_Base* InRuntimeNode)
{
	RuntimeNode = static_cast<FAnimNode_KawaiiPhysics*>(InRuntimeNode);
	GraphNode = CastChecked<UAnimGraphNode_KawaiiPhysics>(InEditorNode);


	// for Sync DetailPanel
	GraphNode->Node.SphericalLimitsData = RuntimeNode->SphericalLimitsData;
	GraphNode->Node.CapsuleLimitsData = RuntimeNode->CapsuleLimitsData;
	GraphNode->Node.PlanarLimitsData = RuntimeNode->PlanarLimitsData;
	GraphNode->Node.BoneConstraintsData = RuntimeNode->BoneConstraintsData;
	GraphNode->Node.MergedBoneConstraints = RuntimeNode->MergedBoneConstraints;

	NodePropertyDelegateHandle = GraphNode->OnNodePropertyChanged().AddSP(
		this, &FKawaiiPhysicsEditMode::OnExternalNodePropertyChange);
	if (RuntimeNode->LimitsDataAsset)
	{
		LimitsDataAssetPropertyDelegateHandle =
			RuntimeNode->LimitsDataAsset->OnLimitsChanged.AddRaw(
				this, &FKawaiiPhysicsEditMode::OnLimitDataAssetPropertyChange);
	}

	FAnimNodeEditMode::EnterMode(InEditorNode, InRuntimeNode);
}

void FKawaiiPhysicsEditMode::ExitMode()
{
	GraphNode->OnNodePropertyChanged().Remove(NodePropertyDelegateHandle);
	if (RuntimeNode->LimitsDataAsset)
	{
		RuntimeNode->LimitsDataAsset->OnLimitsChanged.Remove(LimitsDataAssetPropertyDelegateHandle);
	}

	GraphNode = nullptr;
	RuntimeNode = nullptr;

	FAnimNodeEditMode::ExitMode();
}

void FKawaiiPhysicsEditMode::Render(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI)
{
	const USkeletalMeshComponent* SkelMeshComp = GetAnimPreviewScene().GetPreviewMeshComponent();

#if	ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
	if (SkelMeshComp && SkelMeshComp->GetSkeletalMeshAsset() && SkelMeshComp->GetSkeletalMeshAsset()->GetSkeleton())
#else
	if (SkelMeshComp && SkelMeshComp->SkeletalMesh && SkelMeshComp->SkeletalMesh->GetSkeleton())
#endif
	{
		RenderModifyBones(PDI);
		RenderSphericalLimits(PDI);
		RenderCapsuleLimit(PDI);
		RenderPlanerLimit(PDI);
		RenderBoneConstraint(PDI);
		RenderExternalForces(PDI);

		PDI->SetHitProxy(nullptr);

		if (IsValidSelectCollision())
		{
			if (const FCollisionLimitBase* Collision = GetSelectCollisionLimitRuntime())
			{
				FTransform BoneTransform = FTransform::Identity;
				if (Collision->DrivingBone.BoneIndex >= 0 && RuntimeNode->ForwardedPose.GetPose().GetNumBones() > 0)
				{
					BoneTransform = RuntimeNode->ForwardedPose.GetComponentSpaceTransform(
						Collision->DrivingBone.GetCompactPoseIndex(
							RuntimeNode->ForwardedPose.GetPose().GetBoneContainer()));
				}
				PDI->DrawPoint(BoneTransform.GetLocation(), FLinearColor::White, 10.0f, SDPG_Foreground);
				DrawDashedLine(PDI, Collision->Location, BoneTransform.GetLocation(),
				               FLinearColor::White, 1, SDPG_Foreground);
				DrawCoordinateSystem(PDI, BoneTransform.GetLocation(), Collision->Rotation.Rotator(), 20,
				                     SDPG_World + 1);
			}
		}
	}

	FAnimNodeEditMode::Render(View, Viewport, PDI);
}

void FKawaiiPhysicsEditMode::RenderModifyBones(FPrimitiveDrawInterface* PDI) const
{
	if (GraphNode->bEnableDebugDrawBone)
	{
		for (auto& Bone : RuntimeNode->ModifyBones)
		{
			PDI->DrawPoint(Bone.Location, FLinearColor::White, 5.0f, SDPG_Foreground);

			if (Bone.PhysicsSettings.Radius > 0)
			{
				auto Color = Bone.bDummy ? FColor::Red : FColor::Yellow;
				DrawWireSphere(PDI, Bone.Location, Color, Bone.PhysicsSettings.Radius, 16, SDPG_Foreground);
			}

			for (const int32 ChildIndex : Bone.ChildIndexs)
			{
				DrawDashedLine(PDI, Bone.Location, RuntimeNode->ModifyBones[ChildIndex].Location,
				               FLinearColor::White, 1, SDPG_Foreground);
			}
		}
	}
}

void FKawaiiPhysicsEditMode::RenderSphericalLimits(FPrimitiveDrawInterface* PDI) const
{
	if (GraphNode->bEnableDebugDrawSphereLimit)
	{
		for (int32 i = 0; i < RuntimeNode->SphericalLimits.Num(); i++)
		{
			auto& Sphere = RuntimeNode->SphericalLimits[i];
			if (Sphere.bEnable && Sphere.Radius > 0)
			{
				PDI->SetHitProxy(new HKawaiiPhysicsHitProxy(ECollisionLimitType::Spherical, i));
				DrawSphere(PDI, Sphere.Location, FRotator::ZeroRotator, FVector(Sphere.Radius), 24, 6,
				           GEngine->ConstraintLimitMaterialPrismatic->GetRenderProxy(), SDPG_World);
				DrawWireSphere(PDI, Sphere.Location, FLinearColor::Black, Sphere.Radius, 24, SDPG_World);
				DrawCoordinateSystem(PDI, Sphere.Location, Sphere.Rotation.Rotator(), Sphere.Radius, SDPG_World + 1);
			}
		}

		for (int32 i = 0; i < RuntimeNode->SphericalLimitsData.Num(); i++)
		{
			auto& Sphere = RuntimeNode->SphericalLimitsData[i];
			if (Sphere.bEnable && Sphere.Radius > 0)
			{
				PDI->SetHitProxy(new HKawaiiPhysicsHitProxy(ECollisionLimitType::Spherical, i, true));
				DrawSphere(PDI, Sphere.Location, FRotator::ZeroRotator, FVector(Sphere.Radius), 24, 6,
				           GEngine->ConstraintLimitMaterialZ->GetRenderProxy(), SDPG_World);
				DrawWireSphere(PDI, Sphere.Location, FLinearColor::Black, Sphere.Radius, 24, SDPG_World);
				DrawCoordinateSystem(PDI, Sphere.Location, Sphere.Rotation.Rotator(), Sphere.Radius, SDPG_World + 1);
			}
		}
	}
}

void FKawaiiPhysicsEditMode::RenderCapsuleLimit(FPrimitiveDrawInterface* PDI) const
{
	if (GraphNode->bEnableDebugDrawCapsuleLimit)
	{
		for (int32 i = 0; i < RuntimeNode->CapsuleLimits.Num(); i++)
		{
			auto& Capsule = RuntimeNode->CapsuleLimits[i];
			if (Capsule.bEnable && Capsule.Radius > 0 && Capsule.Length > 0)
			{
				FVector XAxis = Capsule.Rotation.GetAxisX();
				FVector YAxis = Capsule.Rotation.GetAxisY();
				FVector ZAxis = Capsule.Rotation.GetAxisZ();

				PDI->SetHitProxy(new HKawaiiPhysicsHitProxy(ECollisionLimitType::Capsule, i));
				DrawCylinder(PDI, Capsule.Location, XAxis, YAxis, ZAxis, Capsule.Radius, 0.5f * Capsule.Length, 25,
				             GEngine->ConstraintLimitMaterialPrismatic->GetRenderProxy(), SDPG_World);
				DrawSphere(PDI, Capsule.Location + ZAxis * Capsule.Length * 0.5f, Capsule.Rotation.Rotator(),
				           FVector(Capsule.Radius),
				           24, 6, GEngine->ConstraintLimitMaterialPrismatic->GetRenderProxy(), SDPG_World);
				DrawSphere(PDI, Capsule.Location - ZAxis * Capsule.Length * 0.5f, Capsule.Rotation.Rotator(),
				           FVector(Capsule.Radius),
				           24, 6, GEngine->ConstraintLimitMaterialPrismatic->GetRenderProxy(), SDPG_World);

				DrawWireCapsule(PDI, Capsule.Location, XAxis, YAxis, ZAxis,
				                FLinearColor::Black, Capsule.Radius, 0.5f * Capsule.Length + Capsule.Radius, 25,
				                SDPG_World);

				DrawCoordinateSystem(PDI, Capsule.Location, Capsule.Rotation.Rotator(), Capsule.Radius, SDPG_World + 1);
			}
		}

		for (int32 i = 0; i < RuntimeNode->CapsuleLimitsData.Num(); i++)
		{
			auto& Capsule = RuntimeNode->CapsuleLimitsData[i];
			if (Capsule.bEnable && Capsule.Radius > 0 && Capsule.Length > 0)
			{
				FVector XAxis = Capsule.Rotation.GetAxisX();
				FVector YAxis = Capsule.Rotation.GetAxisY();
				FVector ZAxis = Capsule.Rotation.GetAxisZ();

				PDI->SetHitProxy(new HKawaiiPhysicsHitProxy(ECollisionLimitType::Capsule, i, true));
				DrawCylinder(PDI, Capsule.Location, XAxis, YAxis, ZAxis, Capsule.Radius, 0.5f * Capsule.Length, 25,
				             GEngine->ConstraintLimitMaterialZ->GetRenderProxy(), SDPG_World);
				DrawSphere(PDI, Capsule.Location + ZAxis * Capsule.Length * 0.5f, Capsule.Rotation.Rotator(),
				           FVector(Capsule.Radius),
				           24, 6, GEngine->ConstraintLimitMaterialZ->GetRenderProxy(), SDPG_World);
				DrawSphere(PDI, Capsule.Location - ZAxis * Capsule.Length * 0.5f, Capsule.Rotation.Rotator(),
				           FVector(Capsule.Radius),
				           24, 6, GEngine->ConstraintLimitMaterialZ->GetRenderProxy(), SDPG_World);

				DrawWireCapsule(PDI, Capsule.Location, XAxis, YAxis, ZAxis,
				                FLinearColor::Black, Capsule.Radius, 0.5f * Capsule.Length + Capsule.Radius, 25,
				                SDPG_World);

				DrawCoordinateSystem(PDI, Capsule.Location, Capsule.Rotation.Rotator(), Capsule.Radius, SDPG_World + 1);
			}
		}
	}
}

void FKawaiiPhysicsEditMode::RenderPlanerLimit(FPrimitiveDrawInterface* PDI)
{
	if (GraphNode->bEnableDebugDrawPlanerLimit)
	{
		for (int32 i = 0; i < RuntimeNode->PlanarLimits.Num(); i++)
		{
			auto& Plane = RuntimeNode->PlanarLimits[i];
			FTransform PlaneTransform = FTransform(Plane.Rotation, Plane.Location);
			PlaneTransform.NormalizeRotation();

			PDI->SetHitProxy(new HKawaiiPhysicsHitProxy(ECollisionLimitType::Planar, i));
			DrawPlane10x10(PDI, PlaneTransform.ToMatrixWithScale(), 200.0f, FVector2D(0.0f, 0.0f),
			               FVector2D(1.0f, 1.0f), GEngine->ConstraintLimitMaterialPrismatic->GetRenderProxy(),
			               SDPG_World);
			DrawDirectionalArrow(PDI, FRotationMatrix(FRotator(90.0f, 0.0f, 0.0f)) * PlaneTransform.ToMatrixWithScale(),
			                     FLinearColor::Blue, 50.0f, 20.0f, SDPG_Foreground, 0.5f);
		}

		for (int32 i = 0; i < RuntimeNode->PlanarLimitsData.Num(); i++)
		{
			auto& Plane = RuntimeNode->PlanarLimitsData[i];
			FTransform PlaneTransform = FTransform(Plane.Rotation, Plane.Location);
			PlaneTransform.NormalizeRotation();

			PDI->SetHitProxy(new HKawaiiPhysicsHitProxy(ECollisionLimitType::Planar, i, true));
			DrawPlane10x10(PDI, PlaneTransform.ToMatrixWithScale(), 200.0f, FVector2D(0.0f, 0.0f),
			               FVector2D(1.0f, 1.0f), GEngine->ConstraintLimitMaterialZ->GetRenderProxy(), SDPG_World);
			DrawDirectionalArrow(PDI, FRotationMatrix(FRotator(90.0f, 0.0f, 0.0f)) * PlaneTransform.ToMatrixWithScale(),
			                     FLinearColor::Blue, 50.0f, 20.0f, SDPG_Foreground, 0.5f);
		}
	}
}

void FKawaiiPhysicsEditMode::RenderBoneConstraint(FPrimitiveDrawInterface* PDI) const
{
	if (GraphNode->bEnableDebugDrawBoneConstraint)
	{
		for (const FModifyBoneConstraint& BoneConstraint : RuntimeNode->MergedBoneConstraints)
		{
			if (BoneConstraint.IsBoneReferenceValid() && !RuntimeNode->ModifyBones.IsEmpty())
			{
				FTransform BoneTransform1 = FTransform(
					RuntimeNode->ModifyBones[BoneConstraint.ModifyBoneIndex1].PrevRotation,
					RuntimeNode->ModifyBones[BoneConstraint.ModifyBoneIndex1].PrevLocation);
				FTransform BoneTransform2 = FTransform(
					RuntimeNode->ModifyBones[BoneConstraint.ModifyBoneIndex2].PrevRotation,
					RuntimeNode->ModifyBones[BoneConstraint.ModifyBoneIndex2].PrevLocation);

				// 1 -> 2
				FVector Dir = (BoneTransform2.GetLocation() - BoneTransform1.GetLocation()).GetSafeNormal();
				FRotator LookAt = FRotationMatrix::MakeFromX(Dir).Rotator();
				FTransform DrawArrowTransform = FTransform(LookAt, BoneTransform1.GetLocation(),
				                                           BoneTransform1.GetScale3D());
				const float Distance = (BoneTransform1.GetLocation() - BoneTransform2.GetLocation()).Size();
				DrawDirectionalArrow(PDI, DrawArrowTransform.ToMatrixNoScale(), FLinearColor::Red,
				                     Distance, 1, SDPG_Foreground);
				// 2 -> 1
				LookAt = FRotationMatrix::MakeFromX(-Dir).Rotator();
				DrawArrowTransform = FTransform(LookAt, BoneTransform2.GetLocation(), BoneTransform2.GetScale3D());
				DrawDirectionalArrow(PDI, DrawArrowTransform.ToMatrixNoScale(), FLinearColor::Red,
				                     Distance, 1, SDPG_Foreground);
			}
		}
	}
}

void FKawaiiPhysicsEditMode::RenderExternalForces(FPrimitiveDrawInterface* PDI) const
{
	if (GraphNode->bEnableDebugDrawExternalForce)
	{
		for (const auto& Bone : RuntimeNode->ModifyBones)
		{
			for (auto& Force : RuntimeNode->ExternalForces)
			{
				if (Force.IsValid())
				{
					Force.GetMutablePtr<FKawaiiPhysics_ExternalForce>()->AnimDrawDebugForEditMode(
						Bone, *RuntimeNode, PDI);
				}
			}
		}
	}
}

FVector FKawaiiPhysicsEditMode::GetWidgetLocation(ECollisionLimitType CollisionType, int32 Index) const
{
	if (!IsValidSelectCollision())
	{
		return GetAnimPreviewScene().GetPreviewMeshComponent()->GetComponentLocation();
	}

	if (const FCollisionLimitBase* Collision = GetSelectCollisionLimitRuntime())
	{
		return Collision->Location;
	}

	return GetAnimPreviewScene().GetPreviewMeshComponent()->GetComponentLocation();
}

FVector FKawaiiPhysicsEditMode::GetWidgetLocation() const
{
	return GetWidgetLocation(SelectCollisionType, SelectCollisionIndex);
}

bool FKawaiiPhysicsEditMode::GetCustomDrawingCoordinateSystem(FMatrix& InMatrix, void* InData)
{
	if (!IsValidSelectCollision())
	{
		return false;
	}

	FQuat Rotation = FQuat::Identity;

	FCollisionLimitBase* Collision = GetSelectCollisionLimitRuntime();
	if (Collision)
	{
		Rotation = Collision->Rotation;
	}

	InMatrix = FTransform(Rotation).ToMatrixNoScale();
	return true;
}

UE_WIDGET::EWidgetMode FKawaiiPhysicsEditMode::GetWidgetMode() const
{
	if (FCollisionLimitBase* Collision = GetSelectCollisionLimitRuntime())
	{
		CurWidgetMode = FindValidWidgetMode(CurWidgetMode);
		return CurWidgetMode;
	}

	return UE_WIDGET::EWidgetMode::WM_Translate;
}

UE_WIDGET::EWidgetMode FKawaiiPhysicsEditMode::FindValidWidgetMode(UE_WIDGET::EWidgetMode InWidgetMode) const
{
	if (InWidgetMode == UE_WIDGET::EWidgetMode::WM_None)
	{
		return UE_WIDGET::EWidgetMode::WM_Translate;
	}

	switch (InWidgetMode)
	{
	case UE_WIDGET::EWidgetMode::WM_Translate:
		return UE_WIDGET::EWidgetMode::WM_Rotate;
	case UE_WIDGET::EWidgetMode::WM_Rotate:
		return UE_WIDGET::EWidgetMode::WM_Scale;
	case UE_WIDGET::EWidgetMode::WM_Scale:
		return UE_WIDGET::EWidgetMode::WM_Translate;
	default: ;
	}

	return UE_WIDGET::EWidgetMode::WM_None;
}

bool FKawaiiPhysicsEditMode::HandleClick(FEditorViewportClient* InViewportClient, HHitProxy* HitProxy,
                                         const FViewportClick& Click)
{
	bool bResult = FAnimNodeEditMode::HandleClick(InViewportClient, HitProxy, Click);

	if (HitProxy != nullptr && HitProxy->IsA(HKawaiiPhysicsHitProxy::StaticGetType()))
	{
		HKawaiiPhysicsHitProxy* KawaiiPhysicsHitProxy = static_cast<HKawaiiPhysicsHitProxy*>(HitProxy);
		SelectCollisionType = KawaiiPhysicsHitProxy->CollisionType;
		SelectCollisionIndex = KawaiiPhysicsHitProxy->CollisionIndex;
		bIsSelectCollisionFromDataAsset = KawaiiPhysicsHitProxy->bFromDataAsset;
		return true;
	}

	SelectCollisionType = ECollisionLimitType::None;
	SelectCollisionIndex = -1;

	return false;
}

bool FKawaiiPhysicsEditMode::InputKey(FEditorViewportClient* InViewportClient, FViewport* InViewport, FKey InKey,
                                      EInputEvent InEvent)
{
	bool bHandled = false;

#if	ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
	if ((InEvent == IE_Pressed) && !IsManipulatingWidget())
#else
	if ((InEvent == IE_Pressed)) //&& !bManipulating)
#endif
	{
		if (InKey == EKeys::SpaceBar)
		{
			GetModeManager()->SetWidgetMode(GetWidgetMode());
			bHandled = true;
			InViewportClient->Invalidate();
		}
		else if (InKey == EKeys::Q)
		{
			const auto CoordSystem = GetModeManager()->GetCoordSystem();
			GetModeManager()->SetCoordSystem(CoordSystem == COORD_Local ? COORD_World : COORD_Local);
		}
		else if (InKey == EKeys::Delete && IsValidSelectCollision())
		{
			switch (SelectCollisionType)
			{
			case ECollisionLimitType::Spherical:
				if (bIsSelectCollisionFromDataAsset)
				{
					RuntimeNode->LimitsDataAsset->SphericalLimitsData.RemoveAt(SelectCollisionIndex);
					RuntimeNode->LimitsDataAsset->Sync();
					RuntimeNode->LimitsDataAsset->MarkPackageDirty();
				}
				else
				{
					RuntimeNode->SphericalLimits.RemoveAt(SelectCollisionIndex);
					GraphNode->Node.SphericalLimits.RemoveAt(SelectCollisionIndex);
				}
				break;
			case ECollisionLimitType::Capsule:
				if (bIsSelectCollisionFromDataAsset)
				{
					RuntimeNode->LimitsDataAsset->CapsuleLimitsData.RemoveAt(SelectCollisionIndex);
					RuntimeNode->LimitsDataAsset->Sync();
					RuntimeNode->LimitsDataAsset->MarkPackageDirty();
				}
				else
				{
					RuntimeNode->CapsuleLimits.RemoveAt(SelectCollisionIndex);
					GraphNode->Node.CapsuleLimits.RemoveAt(SelectCollisionIndex);
				}
				break;
			case ECollisionLimitType::Planar:
				if (bIsSelectCollisionFromDataAsset)
				{
					RuntimeNode->LimitsDataAsset->PlanarLimitsData.RemoveAt(SelectCollisionIndex);
					RuntimeNode->LimitsDataAsset->Sync();
					RuntimeNode->LimitsDataAsset->MarkPackageDirty();
				}
				else
				{
					RuntimeNode->PlanarLimits.RemoveAt(SelectCollisionIndex);
					GraphNode->Node.PlanarLimits.RemoveAt(SelectCollisionIndex);
				}
				break;
			case ECollisionLimitType::None: break;
			default: ;
			}
		}
	}

	return bHandled;
}

ECoordSystem FKawaiiPhysicsEditMode::GetWidgetCoordinateSystem() const
{
	return COORD_Local;
}

void FKawaiiPhysicsEditMode::OnExternalNodePropertyChange(FPropertyChangedEvent& InPropertyEvent)
{
	if (!IsValidSelectCollision())
	{
		SelectCollisionIndex = -1;
		SelectCollisionType = ECollisionLimitType::None;
		CurWidgetMode = UE_WIDGET::EWidgetMode::WM_None;
	}

	if (InPropertyEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(FAnimNode_KawaiiPhysics, LimitsDataAsset))
	{
		if (RuntimeNode->LimitsDataAsset)
		{
			RuntimeNode->LimitsDataAsset->OnLimitsChanged.AddRaw(
				this, &FKawaiiPhysicsEditMode::OnLimitDataAssetPropertyChange);
		}
	}
}

void FKawaiiPhysicsEditMode::OnLimitDataAssetPropertyChange(FPropertyChangedEvent& InPropertyEvent)
{
	GraphNode->Node.SphericalLimitsData = RuntimeNode->SphericalLimitsData;
	GraphNode->Node.CapsuleLimitsData = RuntimeNode->CapsuleLimitsData;
	GraphNode->Node.PlanarLimitsData = RuntimeNode->PlanarLimitsData;
}

bool FKawaiiPhysicsEditMode::IsValidSelectCollision() const
{
	if (RuntimeNode == nullptr || GraphNode == nullptr || SelectCollisionIndex < 0 || SelectCollisionType ==
		ECollisionLimitType::None)
	{
		return false;
	}

	switch (SelectCollisionType)
	{
	case ECollisionLimitType::Spherical:
		return bIsSelectCollisionFromDataAsset
			       ? RuntimeNode->SphericalLimitsData.IsValidIndex(SelectCollisionIndex)
			       : RuntimeNode->SphericalLimits.IsValidIndex(SelectCollisionIndex);
	case ECollisionLimitType::Capsule:
		return bIsSelectCollisionFromDataAsset
			       ? RuntimeNode->CapsuleLimitsData.IsValidIndex(SelectCollisionIndex)
			       : RuntimeNode->CapsuleLimits.IsValidIndex(SelectCollisionIndex);
	case ECollisionLimitType::Planar:
		return bIsSelectCollisionFromDataAsset
			       ? RuntimeNode->PlanarLimitsData.IsValidIndex(SelectCollisionIndex)
			       : RuntimeNode->PlanarLimits.IsValidIndex(SelectCollisionIndex);
	case ECollisionLimitType::None: break;
	default: ;
	}
	return false;
}

FCollisionLimitBase* FKawaiiPhysicsEditMode::GetSelectCollisionLimitRuntime() const
{
	if (!IsValidSelectCollision())
	{
		return nullptr;
	}

	switch (SelectCollisionType)
	{
	case ECollisionLimitType::Spherical:
		return bIsSelectCollisionFromDataAsset
			       ? &(RuntimeNode->SphericalLimitsData[SelectCollisionIndex])
			       : &(RuntimeNode->SphericalLimits[SelectCollisionIndex]);
	case ECollisionLimitType::Capsule:
		return bIsSelectCollisionFromDataAsset
			       ? &(RuntimeNode->CapsuleLimitsData[SelectCollisionIndex])
			       : &(RuntimeNode->CapsuleLimits[SelectCollisionIndex]);
	case ECollisionLimitType::Planar:
		return bIsSelectCollisionFromDataAsset
			       ? &(RuntimeNode->PlanarLimitsData[SelectCollisionIndex])
			       : &(RuntimeNode->PlanarLimits[SelectCollisionIndex]);
	case ECollisionLimitType::None: break;
	default: ;
	}

	return nullptr;
}

FCollisionLimitBase* FKawaiiPhysicsEditMode::GetSelectCollisionLimitGraph() const
{
	if (!IsValidSelectCollision())
	{
		return nullptr;
	}

	switch (SelectCollisionType)
	{
	case ECollisionLimitType::Spherical:
		{
			auto& CollisionLimits = bIsSelectCollisionFromDataAsset
				                        ? GraphNode->Node.SphericalLimitsData
				                        : GraphNode->Node.SphericalLimits;
			return CollisionLimits.IsValidIndex(SelectCollisionIndex)
				       ? &CollisionLimits[SelectCollisionIndex]
				       : nullptr;
		}
	case ECollisionLimitType::Capsule:
		{
			auto& CollisionLimits = bIsSelectCollisionFromDataAsset
				                        ? GraphNode->Node.CapsuleLimitsData
				                        : GraphNode->Node.CapsuleLimits;
			return CollisionLimits.IsValidIndex(SelectCollisionIndex)
				       ? &CollisionLimits[SelectCollisionIndex]
				       : nullptr;
		}
	case ECollisionLimitType::Planar:
		{
			auto& CollisionLimits = bIsSelectCollisionFromDataAsset
				                        ? GraphNode->Node.PlanarLimitsData
				                        : GraphNode->Node.PlanarLimits;
			return CollisionLimits.IsValidIndex(SelectCollisionIndex)
				       ? &CollisionLimits[SelectCollisionIndex]
				       : nullptr;
		}
	case ECollisionLimitType::None: break;
	default: ;
	}

	return nullptr;
}

void FKawaiiPhysicsEditMode::DoTranslation(FVector& InTranslation)
{
	if (InTranslation.IsNearlyZero())
	{
		return;
	}

	FCollisionLimitBase* CollisionRuntime = GetSelectCollisionLimitRuntime();
	FCollisionLimitBase* CollisionGraph = GetSelectCollisionLimitGraph();
	if (!CollisionRuntime || !CollisionGraph)
	{
		UE_LOG(LogKawaiiPhysics, Warning, TEXT( "Fail to edit limit." ));
		if (bIsSelectCollisionFromDataAsset)
		{
			UE_LOG(LogKawaiiPhysics, Warning, TEXT( "Please try saving the DataAsset (%s) and compile this ABP." ),
			       *RuntimeNode->LimitsDataAsset.GetName());
		}
		return;
	}

	FVector Offset;
	if (CollisionRuntime->DrivingBone.BoneIndex >= 0)
	{
		const USkeletalMeshComponent* SkelComp = GetAnimPreviewScene().GetPreviewMeshComponent();
		Offset = ConvertCSVectorToBoneSpace(SkelComp, InTranslation, RuntimeNode->ForwardedPose,
		                                    CollisionRuntime->DrivingBone.BoneName, BCS_BoneSpace);
	}
	else
	{
		Offset = InTranslation;
	}
	CollisionRuntime->OffsetLocation += Offset;
	CollisionGraph->OffsetLocation = CollisionRuntime->OffsetLocation;

	if (bIsSelectCollisionFromDataAsset)
	{
		RuntimeNode->LimitsDataAsset->UpdateLimit(CollisionRuntime);
	}
}

void FKawaiiPhysicsEditMode::DoRotation(FRotator& InRotation)
{
	if (InRotation.IsNearlyZero())
	{
		return;
	}

	FCollisionLimitBase* CollisionRuntime = GetSelectCollisionLimitRuntime();
	FCollisionLimitBase* CollisionGraph = GetSelectCollisionLimitGraph();
	if (!CollisionRuntime || !CollisionGraph)
	{
		UE_LOG(LogKawaiiPhysics, Warning, TEXT( "Fail to edit limit." ));
		if (bIsSelectCollisionFromDataAsset)
		{
			UE_LOG(LogKawaiiPhysics, Warning, TEXT( "Please try saving the DataAsset (%s) and compile this ABP." ),
			       *RuntimeNode->LimitsDataAsset.GetName());
		}
		return;
	}

	FQuat DeltaQuat;
	if (CollisionRuntime->DrivingBone.BoneIndex >= 0)
	{
		const USkeletalMeshComponent* SkelComp = GetAnimPreviewScene().GetPreviewMeshComponent();
		DeltaQuat = ConvertCSRotationToBoneSpace(SkelComp, InRotation, RuntimeNode->ForwardedPose,
		                                         CollisionRuntime->DrivingBone.BoneName, BCS_BoneSpace);
	}
	else
	{
		DeltaQuat = InRotation.Quaternion();
	}

	CollisionRuntime->OffsetRotation = FRotator(DeltaQuat * CollisionRuntime->OffsetRotation.Quaternion());
	CollisionGraph->OffsetRotation = CollisionRuntime->OffsetRotation;

	if (bIsSelectCollisionFromDataAsset)
	{
		RuntimeNode->LimitsDataAsset->UpdateLimit(CollisionRuntime);
	}
}

void FKawaiiPhysicsEditMode::DoScale(FVector& InScale)
{
	if (!IsValidSelectCollision() || InScale.IsNearlyZero() || SelectCollisionType == ECollisionLimitType::Planar)
	{
		return;
	}
	FCollisionLimitBase* CollisionRuntime = GetSelectCollisionLimitRuntime();
	FCollisionLimitBase* CollisionGraph = GetSelectCollisionLimitGraph();
	if (!CollisionRuntime || !CollisionGraph)
	{
		UE_LOG(LogKawaiiPhysics, Warning, TEXT( "Fail to edit limit." ));
		if (bIsSelectCollisionFromDataAsset)
		{
			UE_LOG(LogKawaiiPhysics, Warning, TEXT( "Please try saving the DataAsset (%s) and compile this ABP." ),
			       *RuntimeNode->LimitsDataAsset.GetName());
		}
		return;
	}

	if (SelectCollisionType == ECollisionLimitType::Spherical)
	{
		FSphericalLimit& SphericalLimitRuntime = *static_cast<FSphericalLimit*>(CollisionRuntime);
		FSphericalLimit& SphericalLimitGraph = *static_cast<FSphericalLimit*>(CollisionGraph);

		SphericalLimitRuntime.Radius += InScale.X;
		SphericalLimitRuntime.Radius += InScale.Y;
		SphericalLimitRuntime.Radius += InScale.Z;
		SphericalLimitRuntime.Radius = FMath::Max(SphericalLimitRuntime.Radius, 0.0f);

		SphericalLimitGraph.Radius = SphericalLimitRuntime.Radius;

		if (bIsSelectCollisionFromDataAsset)
		{
			RuntimeNode->LimitsDataAsset->UpdateLimit(&SphericalLimitRuntime);
		}
	}
	else if (SelectCollisionType == ECollisionLimitType::Capsule)
	{
		FCapsuleLimit& CapsuleLimitRuntime = *static_cast<FCapsuleLimit*>(CollisionRuntime);
		FCapsuleLimit& CapsuleLimitGraph = *static_cast<FCapsuleLimit*>(CollisionGraph);

		CapsuleLimitRuntime.Radius += InScale.X;
		CapsuleLimitRuntime.Radius += InScale.Y;
		CapsuleLimitRuntime.Radius = FMath::Max(CapsuleLimitRuntime.Radius, 0.0f);

		CapsuleLimitRuntime.Length += InScale.Z;
		CapsuleLimitRuntime.Length = FMath::Max(CapsuleLimitRuntime.Length, 0.0f);

		CapsuleLimitGraph.Radius = CapsuleLimitRuntime.Radius;
		CapsuleLimitGraph.Length = CapsuleLimitRuntime.Length;

		if (bIsSelectCollisionFromDataAsset)
		{
			RuntimeNode->LimitsDataAsset->UpdateLimit(&CapsuleLimitRuntime);
		}
	}
}


bool FKawaiiPhysicsEditMode::ShouldDrawWidget() const
{
	if (IsValidSelectCollision())
	{
		return true;
	}

	return false;
}

void FKawaiiPhysicsEditMode::DrawHUD(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FSceneView* View,
                                     FCanvas* Canvas)
{
	float FontWidth, FontHeight;
	GEngine->GetSmallFont()->GetCharSize(TEXT('L'), FontWidth, FontHeight);

	constexpr float XOffset = 5.0f;

	float DrawPositionY = Viewport->GetSizeXY().Y / Canvas->GetDPIScale() - (3 + FontHeight) - 100 / Canvas->
		GetDPIScale();

	DrawTextItem(LOCTEXT("", "Q : Cycle Transform Coordinate System"), Canvas, XOffset, DrawPositionY, FontHeight);
	DrawTextItem(
		LOCTEXT("", "Space : Cycle Between Translate, Rotate and Scale"), Canvas, XOffset, DrawPositionY, FontHeight);
	DrawTextItem(LOCTEXT("", "R : Scale Mode"), Canvas, XOffset, DrawPositionY, FontHeight);
	DrawTextItem(LOCTEXT("", "E : Rotate Mode"), Canvas, XOffset, DrawPositionY, FontHeight);
	DrawTextItem(LOCTEXT("", "W : Translate Mode"), Canvas, XOffset, DrawPositionY, FontHeight);
	DrawTextItem(LOCTEXT("", "------------------"), Canvas, XOffset, DrawPositionY, FontHeight);


	FString CollisionDebugInfo = FString(TEXT("Select Collision : "));
	switch (SelectCollisionType)
	{
	case ECollisionLimitType::Spherical:
		CollisionDebugInfo.Append(FString(TEXT("Spherical")));
		break;
	case ECollisionLimitType::Capsule:
		CollisionDebugInfo.Append(FString(TEXT("Capsule")));
		break;
	case ECollisionLimitType::Planar:
		CollisionDebugInfo.Append(FString(TEXT("Planar")));
		break;
	default:
		CollisionDebugInfo.Append(FString(TEXT("None")));
		break;
	}
	if (SelectCollisionIndex >= 0)
	{
		CollisionDebugInfo.Append(FString(TEXT("[")));
		CollisionDebugInfo.Append(FString::FromInt(SelectCollisionIndex));
		CollisionDebugInfo.Append(FString(TEXT("]")));
	}
	DrawTextItem(FText::FromString(CollisionDebugInfo), Canvas, XOffset, DrawPositionY, FontHeight);

	const UDebugSkelMeshComponent* PreviewMeshComponent = GetAnimPreviewScene().GetPreviewMeshComponent();
	if (GraphNode->bEnableDebugBoneLengthRate)
	{
		if (PreviewMeshComponent != nullptr && PreviewMeshComponent->MeshObject != nullptr)
		{
			for (auto& Bone : RuntimeNode->ModifyBones)
			{
				// Refer to FAnimationViewportClient::ShowBoneNames
				const FVector BonePos = PreviewMeshComponent->GetComponentTransform().TransformPosition(Bone.Location);
				Draw3DTextItem(FText::AsNumber(Bone.LengthFromRoot / RuntimeNode->GetTotalBoneLength()), Canvas, View,
				               Viewport, BonePos);
			}
		}
	}

	FAnimNodeEditMode::DrawHUD(ViewportClient, Viewport, View, Canvas);
}

void FKawaiiPhysicsEditMode::DrawTextItem(const FText& Text, FCanvas* Canvas, float X, float& Y, float FontHeight)
{
	FCanvasTextItem TextItem(FVector2D::ZeroVector, Text, GEngine->GetSmallFont(), FLinearColor::White);
	TextItem.EnableShadow(FLinearColor::Black);
	Canvas->DrawItem(TextItem, X, Y);
	Y -= (3 + FontHeight);
}

void FKawaiiPhysicsEditMode::Draw3DTextItem(const FText& Text, FCanvas* Canvas, const FSceneView* View,
                                            const FViewport* Viewport, FVector Location)
{
	const int32 HalfX = Viewport->GetSizeXY().X / 2 / Canvas->GetDPIScale();
	const int32 HalfY = Viewport->GetSizeXY().Y / 2 / Canvas->GetDPIScale();

	const FPlane proj = View->Project(Location);
	if (proj.W > 0.f)
	{
		const int32 XPos = HalfX + (HalfX * proj.X);
		const int32 YPos = HalfY + (HalfY * (proj.Y * -1));
		FCanvasTextItem TextItem(FVector2D(XPos, YPos), Text, GEngine->GetSmallFont(), FLinearColor::White);
		TextItem.EnableShadow(FLinearColor::Black);
		Canvas->DrawItem(TextItem);
	}
}

#undef LOCTEXT_NAMESPACE
