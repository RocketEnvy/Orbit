// Fill out your copyright notice in the Description page of Project Settings.

#pragma once
#include "Orbit.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "OrbitCharacterMovementComponent.generated.h"

/**
 * 
 */
USTRUCT(BlueprintType)
struct ORBIT_API FOrbitFindFloorResult : public FFindFloorResult
{
	GENERATED_USTRUCT_BODY();
	uint32 bBlockingHit;
	uint32 bWalkableFloor;
	uint32 bLineTrace;
	float FloorDist;
	float LineDist;
	FHitResult HitResult;
public:

	FOrbitFindFloorResult()
		: bBlockingHit(false)
		, bWalkableFloor(false)
		, bLineTrace(false)
		, FloorDist(0.f)
		, LineDist(0.f)
		, HitResult(1.f)
	{
	}

	/** Returns true if the floor result hit a walkable surface. */
	bool IsWalkableFloor() const
	{
		return bBlockingHit && bWalkableFloor;
	}

	void Clear()
	{
		bBlockingHit = false;
		bWalkableFloor = false;
		bLineTrace = false;
		FloorDist = 0.f;
		LineDist = 0.f;
		HitResult.Reset(1.f, false);
	}

	void SetFromSweep(const FHitResult& InHit, const float InSweepFloorDist, const bool bIsWalkableFloor);
	void SetFromLineTrace(const FHitResult& InHit, const float InSweepFloorDist, const float InLineDist, const bool bIsWalkableFloor);
};
UCLASS()
class ORBIT_API UOrbitCharacterMovementComponent : public UCharacterMovementComponent //, public UPrimitiveComponent
{
	GENERATED_BODY()
		UOrbitCharacterMovementComponent(const class FObjectInitializer& PCIP);
public:
	enum EOrbitMovementMode : uint8
	{
		CUSTOM_MoonJumping,
		CUSTOM_MoonRunning,
		CUSTOM_MoonWalking
	};
	/*
*/
	FVector GravityDirection, GravityDistanceVector, GravityVector;
	float GravityMagnitude, GravityDistance;
	float YawSum;
	void SumYaw(float yaw);

	virtual void CalculateGravity();
	virtual float GetGravityZ() const override;
	virtual void InitializeComponent() override;
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction);
	virtual void OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode) override;
	virtual void ApplyAccumulatedForces(float DeltaSeconds) override;
	virtual void SetMovementMode(EMovementMode NewMovementMode);
	virtual void SetMovementMode(EMovementMode NewMovementMode, uint8 NewCustomMode) override;
	/** @note Movement update functions should only be called through StartNewPhysics()*/
	virtual void PhysCustom(float deltaTime, int32 Iterations) override;
	virtual void PhysWalking(float deltaTime, int32 Iterations);
	virtual void PhysMoonWalking(float deltaTime, int32 Iterations);
	virtual bool IsMoonWalking() const;
	virtual void AdjustFloorHeight() override;
	virtual bool IsWalkable(const FHitResult& Hit) const override;
	virtual void FindFloor(const FVector& CapsuleLocation, FFindFloorResult& OutFloorResult, bool bZeroDelta, const FHitResult* DownwardSweepResult) const override;
	virtual void ComputeFloorDist(const FVector& CapsuleLocation, float LineDistance, float SweepDistance, FFindFloorResult& OutFloorResult, float SweepRadius, const FHitResult* DownwardSweepResult) const override;
	virtual void MaintainHorizontalGroundVelocity() override;
	virtual bool DoJump(bool bReplayingMoves) override;
	//virtual void SimulateMovement(float DeltaSeconds) override;
	virtual bool IsMovingOnGround() const override;//fixed not jumping , but now can't move
	virtual void OnTeleported() override;
	virtual void SetPostLandedPhysics(const FHitResult& Hit) override;
	virtual void CalcAvoidanceVelocity(float DeltaTime) override;
	virtual void SetDefaultMovementMode() override;
	virtual void PhysFalling(float deltaTime, int32 Iterations) override;
	virtual FVector GetFallingLateralAcceleration(float DeltaTime) override;
	virtual float GetMaxJumpHeight() const override;
	virtual bool SafeMoveUpdatedComponent(const FVector& Delta, const FRotator& NewRotation, bool bSweep, FHitResult& OutHit);
	virtual void StartFalling(int32 Iterations, float remainingTime, float timeTick, const FVector& Delta, const FVector& subLoc) override;
	virtual void ProcessLanded(const FHitResult& Hit, float remainingTime, int32 Iterations) override;
	virtual bool UOrbitCharacterMovementComponent::IsValidLandingSpot(const FVector& CapsuleLocation, const FHitResult& Hit) const override;

	virtual bool FloorSweepTest(
		FHitResult& OutHit,
		const FVector& Start,
		const FVector& End,
		ECollisionChannel TraceChannel,
		const struct FCollisionShape& CollisionShape,
		const struct FCollisionQueryParams& Params,
		const struct FCollisionResponseParams& ResponseParam
		) const override;
	
	virtual bool IsWithinEdgeTolerance(const FVector& CapsuleLocation, const FVector& TestImpactPoint, const float CapsuleRadius) const;
	virtual bool CanStepUp(const FHitResult& Hit) const override;
	virtual bool StepUp(const FVector& InGravDir, const FVector& Delta, const FHitResult &InHit, FStepDownResult* OutStepDownResult) override;
	virtual FVector ConstrainInputAcceleration(const FVector& InputAcceleration) const override;
	virtual void ApplyImpactPhysicsForces(const FHitResult& Impact, const FVector& ImpactAcceleration, const FVector& ImpactVelocity) override;
	virtual void HandleImpact(FHitResult const& Impact, float TimeSlice, const FVector& MoveDelta) override;
	virtual bool ComputePerchResult(const float TestRadius, const FHitResult& InHit, const float InMaxFloorDist, FFindFloorResult& OutPerchFloorResult) const override;
	virtual bool ShouldCheckForValidLandingSpot(float DeltaTime, const FVector& Delta, const FHitResult& Hit) const override;
	float GetPerchRadiusThreshold() const;
	float GetValidPerchRadius() const;
	virtual bool ShouldComputePerchResult(const FHitResult& InHit, bool bCheckRadius) const override;
	virtual void SmoothClientPosition(float DeltaSeconds) override;
	virtual void ApplyRepulsionForce(float DeltaSeconds) override;
	virtual void PhysicsVolumeChanged(APhysicsVolume* NewVolume) override;
	virtual bool ShouldJumpOutOfWater(FVector& JumpDir) override;
	virtual void MoveSmooth(const FVector& InVelocity, const float DeltaSeconds, FStepDownResult* OutStepDownResult) override;
	virtual void DisplayDebug(UCanvas* Canvas, const FDebugDisplayInfo& DebugDisplay, float& YL, float& YPos) override;
	virtual void MoveAlongFloor(const FVector& InVelocity, float DeltaSeconds, FStepDownResult* OutStepDownResult = NULL);
	virtual FVector ComputeGroundMovementDelta(const FVector& Delta, const FHitResult& RampHit, const bool bHitFromLineTrace) const override;
	virtual float SlideAlongSurface(const FVector& Delta, float Time, const FVector& InNormal, FHitResult &Hit, bool bHandleImpact) override;
	virtual FVector ConstrainDirectionToPlane(FVector Direction) const override;
	virtual bool MoveUpdatedComponent(const FVector& Delta, const FRotator& NewRotation, bool bSweep, FHitResult* OutHit) override;
//	virtual bool MoveComponent(const FVector& Delta, const FRotator& NewRotation, bool bSweep, FHitResult* OutHit = NULL, EMoveComponentFlags MoveFlags = MOVECOMP_NoFlags) override;
//	TEnumAsByte<EComponentMobility::Type> Mobility;
	virtual FVector GetImpartedMovementBaseVelocity() const override;
	virtual void UpdateBasedRotation(FRotator &FinalRotation, const FRotator& ReducedRotation) override;
	virtual void UpdateBasedMovement(float DeltaSeconds) override;
	virtual void UOrbitCharacterMovementComponent::TwoWallAdjust(FVector &Delta, const FHitResult& Hit, const FVector &OldHitNormal) const override;
};
