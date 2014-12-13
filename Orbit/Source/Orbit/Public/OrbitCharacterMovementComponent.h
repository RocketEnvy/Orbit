// Fill out your copyright notice in the Description page of Project Settings.

#pragma once
#include "Orbit.h"
#include "OrbitCharacterMovementComponent.generated.h"
/**
 * 
 */
UCLASS(config = Game)
class ORBIT_API UOrbitCharacterMovementComponent : public UCharacterMovementComponent
{
	GENERATED_BODY()
public:
	UOrbitCharacterMovementComponent(const class FObjectInitializer& PCIP);

	virtual bool DoJump(bool bReplayingMoves) override;
	virtual float GetGravityZ() const override;
	FVector GetGravityV() const;
	FVector GetGravityDir() const;
	
	bool CheckLedgeDirection(const FVector& OldLocation, const FVector& SideStep, const FVector& GravDir);
	FVector GetLedgeMove(const FVector& OldLocation, const FVector& Delta, const FVector& GravDir);
	
	virtual void MoveAlongFloor(const FVector& InVelocity, float DeltaSeconds, FStepDownResult* OutStepDownResult = NULL) override;
	virtual void PhysWalking(float deltaTime, int32 Iterations) override;
	virtual void SimulateMovement(float DeltaSeconds) override;
	virtual void PerformAirControlForPathFollowing(FVector Direction, float ZDiff);
	virtual void PhysFalling(float deltaTime, int32 Iterations) override;
	virtual void SetPostLandedPhysics(const FHitResult& Hit) override;
	virtual void PhysSwimming(float deltaTime, int32 Iterations) override;
	virtual void MoveSmooth(const FVector& InVelocity, const float DeltaSeconds, FStepDownResult* OutStepDownResult = NULL) override;
	virtual void HandleImpact(FHitResult const& Hit, float TimeSlice = 0.f, const FVector& MoveDelta = FVector::ZeroVector) override;
	virtual FVector NewFallVelocity(const FVector& InitialVelocity, const FVector& Gravity, float DeltaTime) const override;
	virtual bool FindAirControlImpact(float DeltaTime, float AdditionalTime, const FVector& FallVelocity, const FVector& FallAcceleration, const FVector& Gravity, FHitResult& OutHitResult) override;
	virtual void ProcessLanded(const FHitResult& Hit, float remainingTime, int32 Iterations) override;
	virtual bool IsValidLandingSpot(const FVector& CapsuleLocation, const FHitResult& Hit) const override;
	virtual void CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration) override;
	virtual FVector ComputeGroundMovementDelta(const FVector& Delta, const FHitResult& RampHit, const bool bHitFromLineTrace) const override;
	virtual bool IsWalkable(const FHitResult& Hit) const override;
	virtual void SetWalkableFloorAngle(float InWalkableFloorAngle);
	virtual void SetWalkableFloorZ(float InWalkableFloorZ);
	float WalkableFloorAngle;
	float WalkableFloorZ;


protected:
	
	//Init
	virtual void InitializeComponent() override;

	//Tick
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction) override;



	/*
	//Kerplunk
	void PerformAirControl(FVector Direction, float ZDiff) override;
	virtual void PhysFalling(float deltaTime, int32 Iterations) override;
	virtual bool IsWalkable(const FHitResult& Hit) const;
	virtual void PhysicsRotation(float DeltaTime) override;
	*/
};
