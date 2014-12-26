// Fill out your copyright notice in the Description page of Project Settings.

#include "Orbit.h"
#include "OrbitCharacterMovementComponent.h"
#define VERSION27

#include "GameFramework/PhysicsVolume.h"
#include "GameFramework/GameNetworkManager.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameState.h"
#include "Components/PrimitiveComponent.h"
#include "Animation/AnimMontage.h"
#include "PhysicsEngine/DestructibleActor.h"

// @todo this is here only due to circular dependency to AIModule. To be removed
#include "Navigation/PathFollowingComponent.h"
#include "AI/Navigation/AvoidanceManager.h"
#include "Components/CapsuleComponent.h"
#include "Components/BrushComponent.h"
#include "Components/DestructibleComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogCharacterMovement, Log, All);

/**
 * Character stats
 */
#ifndef STRUCTS_HERE
#define STRUCTS_HERE
DECLARE_STATS_GROUP(TEXT("Character"), STATGROUP_Character, STATCAT_Advanced);
/*
DECLARE_CYCLE_STAT(TEXT("Char Movement Tick"), STAT_CharacterMovementTick, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("Char Movement Authority Time"), STAT_CharacterMovementAuthority, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("Char Movement Simulated Time"), STAT_CharacterMovementSimulated, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("Char Physics Interation"), STAT_CharPhysicsInteraction, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("StepUp"), STAT_CharStepUp, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("Char Update Acceleration"), STAT_CharUpdateAcceleration, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("Char MoveUpdateDelegate"), STAT_CharMoveUpdateDelegate, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("PhysWalking"), STAT_CharPhysWalking, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("PhysFalling"), STAT_CharPhysFalling, STATGROUP_Character);
*/
#endif

// MAGIC NUMBERS
const float MAX_STEP_SIDE_Z = 0.08f;	// maximum z value for the normal on the vertical side of steps
const float SWIMBOBSPEED = -80.f;
const float VERTICAL_SLOPE_NORMAL_Z = 0.001f; // Slope is vertical if Abs(Normal.Z) <= this threshold. Accounts for precision problems that sometimes angle normals slightly off horizontal for vertical surface.

/* Don't know WTF 2do about these
const float UOrbitCharacterMovementComponent::MIN_TICK_TIME = 0.0002f;
const float UOrbitCharacterMovementComponent::MIN_FLOOR_DIST = 1.9f;
const float UOrbitCharacterMovementComponent::MAX_FLOOR_DIST = 2.4f;
const float UOrbitCharacterMovementComponent::BRAKE_TO_STOP_VELOCITY = 10.f;
const float UOrbitCharacterMovementComponent::SWEEP_EDGE_REJECT_DISTANCE = 0.15f;
*/

UOrbitCharacterMovementComponent::UOrbitCharacterMovementComponent(const class FObjectInitializer& PCIP)
	: Super(PCIP)
{
	RotationRate = FRotator(360.0f, 360.0f, 360.0f);
	bMaintainHorizontalGroundVelocity = false;
	GravityDistance = 0.f;
	GravityMagnitude = 0.f;
	GravityVector = FVector::ZeroVector;
	GravityDistanceVector = FVector::ZeroVector;
	YawSum = 0.0;
}

void UOrbitCharacterMovementComponent::InitializeComponent()
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	Super::InitializeComponent();
	CalculateGravity();
}
void UOrbitCharacterMovementComponent::TickComponent( float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction )
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	Super::Super::TickComponent(DeltaTime, TickType, ThisTickFunction);


	// SCOPE_CYCLE_COUNTER(Super.STAT_CharacterMovementTick);

	const FVector InputVector = ConsumeInputVector();
	if (!HasValidData() || ShouldSkipUpdate(DeltaTime) || UpdatedComponent->IsSimulatingPhysics())
	{
		return;
	}

	if (AvoidanceLockTimer > 0.0f)
	{
		AvoidanceLockTimer -= DeltaTime;
	}

	if (CharacterOwner->Role > ROLE_SimulatedProxy)
	{
		if (CharacterOwner->Role == ROLE_Authority)
		{
			// Check we are still in the world, and stop simulating if not.
			const bool bStillInWorld = (bCheatFlying || CharacterOwner->CheckStillInWorld());
			if (!bStillInWorld || !HasValidData())
			{
				return;
			}
		}

		// If we are a client we might have received an update from the server.
		const bool bIsClient = (GetNetMode() == NM_Client && CharacterOwner->Role == ROLE_AutonomousProxy);
		if (bIsClient)
		{
			ClientUpdatePositionAfterServerUpdate();
		}

		// Allow root motion to move characters that have no controller.
		if( CharacterOwner->IsLocallyControlled() || (!CharacterOwner->Controller && bRunPhysicsWithNoController) || (!CharacterOwner->Controller && CharacterOwner->IsPlayingRootMotion()) )
		{
			{
				//SCOPE_CYCLE_COUNTER(STAT_CharUpdateAcceleration);

				// We need to check the jump state before adjusting input acceleration, to minimize latency
				// and to make sure acceleration respects our potentially new falling state.
				CharacterOwner->CheckJumpInput(DeltaTime);

				// apply input to acceleration
				Acceleration = ScaleInputAcceleration(ConstrainInputAcceleration(InputVector));
				AnalogInputModifier = ComputeAnalogInputModifier();
			}

			if (CharacterOwner->Role == ROLE_Authority)
			{
				PerformMovement(DeltaTime);
			}
			else if (bIsClient)
			{
				ReplicateMoveToServer(DeltaTime, Acceleration);
			}
		}
		else if (CharacterOwner->GetRemoteRole() == ROLE_AutonomousProxy)
		{
			// Server ticking for remote client.
			// Between net updates from the client we need to update position if based on another object,
			// otherwise the object will move on intermediate frames and we won't follow it.
			MaybeUpdateBasedMovement(DeltaTime);
			SaveBaseLocation();
		}
	}
	else if (CharacterOwner->Role == ROLE_SimulatedProxy)
	{
		AdjustProxyCapsuleSize();
		SimulatedTick(DeltaTime);
	}

	UpdateDefaultAvoidance();

	if (bEnablePhysicsInteraction)
	{
		//SCOPE_CYCLE_COUNTER(STAT_CharPhysicsInteraction);

		if (CurrentFloor.HitResult.IsValidBlockingHit())
		{
			// Apply downwards force when walking on top of physics objects
			if (UPrimitiveComponent* BaseComp = CurrentFloor.HitResult.GetComponent())
			{
				if (StandingDownwardForceScale != 0.f && BaseComp->IsAnySimulatingPhysics())
				{
					//const float GravZ = GetGravityZ();
					//const float Grav = GravityMagnitude;//gdg
					const FVector ForceLocation = CurrentFloor.HitResult.ImpactPoint;
					//BaseComp->AddForceAtLocation(FVector(0.f, 0.f, GravZ * Mass * StandingDownwardForceScale), ForceLocation, CurrentFloor.HitResult.BoneName);
					BaseComp->AddForceAtLocation(GravityVector * Mass * StandingDownwardForceScale, 
						ForceLocation, CurrentFloor.HitResult.BoneName);
	//UE_LOG(LogTemp, Warning, TEXT("%d FORCE Added!!! ForceLoc:%s"), __LINE__, *ForceLocation.ToString());
				}
			}
		}

		ApplyRepulsionForce(DeltaTime);
	}





	//end Paste

	CalculateGravity();//FIXME: probably won't have to do the complete calculation every tick
	FRotator GravRot = GravityDirection.Rotation();
	GravRot.Pitch += 120;//not sure why this correction is needed 
	GetOwner()->SetActorRotation(GravRot);
	GetOwner()->AddActorLocalRotation(FRotator(0, YawSum, 0), true);
}
	
//yaw delta actually
void UOrbitCharacterMovementComponent::SumYaw(float yaw){
	YawSum += yaw;
}

void UOrbitCharacterMovementComponent::CalculateGravity()
{
		//FIXME Need to iterate over massive things to get vector sum of direction and force
		const float gravBodyMass = 900000000.f;
		GravityDistanceVector = FVector(0.f, 0.f, 5000.f) - GetOwner()->GetActorLocation();
		//GravityDistanceVector = FVector(0.f, 0.f, 5000.f) -  GetOwner()->GetTransform().GetLocation();
		GravityDistance = GravityDistanceVector.Size();
#ifdef VERSION27
		GravityDirection = GravityDistanceVector.GetSafeNormal();
#else
		GravityDirection = GravityDistanceVector.SafeNormal();
#endif
		//	GravityDirection = GravityDistanceVector/GravityDistance;
		GravityMagnitude = ((Mass * gravBodyMass) / FMath::Square(GravityDistance));
		GravityVector = GravityDirection * GravityMagnitude;
		//AddImpulse(GravityVector);
		AddForce(GravityVector);
	//UE_LOG(LogTemp, Warning, TEXT("CalGrav:%d GDir:%s GM:%f GV:%s"), __LINE__,*GravityDirection.ToString(), GravityMagnitude, *GravityVector.ToString());
}

float UOrbitCharacterMovementComponent::GetGravityZ() const
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	return GravityMagnitude;
}
void UOrbitCharacterMovementComponent::OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode){
//	Super::OnMovementModeChanged(PreviousMovementMode, PreviousCustomMode);
	
	UE_LOG(LogTemp, Warning, TEXT("%s %d"), *GetMovementName(), CustomMovementMode);
	if (!HasValidData())
	{
		return;
	}

	// Update collision settings if needed
/*	if (MovementMode == MOVE_NavWalking)
	{
		SetNavWalkingPhysics(true);
	}
	else if (PreviousMovementMode == MOVE_NavWalking)
	{
		if (MovementMode == DefaultLandMovementMode)
		{
			const bool bCanSwitchMode = TryToLeaveNavWalking();
			if (!bCanSwitchMode)
			{
				SetMovementMode(MOVE_NavWalking);
				return;
			}
		}
		else
		{
			SetNavWalkingPhysics(false);
		}
	}
	*/

	// React to changes in the movement mode.
	if (MovementMode == MOVE_Walking)
	{
		// Walking uses only XY velocity, and must be on a walkable floor, with a Base.
//		Velocity.Z = 0.f;
		//gdg
		bCrouchMaintainsBaseLocation = true;
		//GroundMovementMode = MovementMode;
		//gdg

		// make sure we update our new floor/base on initial entry of the walking physics
		FindFloor(UpdatedComponent->GetComponentLocation(), CurrentFloor, false, NULL);
		/*
FindFloor(const FVector& CapsuleLocation, FFindFloorResult& OutFloorResult, bool bZeroDelta, 
	const FHitResult* DownwardSweepResult) const
	*/
		AdjustFloorHeight();
		SetBase(CurrentFloor.HitResult.Component.Get(), CurrentFloor.HitResult.BoneName);
	}
	else
	{
		CurrentFloor.Clear();
		bCrouchMaintainsBaseLocation = false;

		if (MovementMode == MOVE_Falling)
		{
			Velocity += GetImpartedMovementBaseVelocity();
			CharacterOwner->Falling();
		}

		SetBase(NULL);

		if (MovementMode == MOVE_None)
		{
			// Kill velocity and clear queued up events
			// StopMovementKeepPathing();
			CharacterOwner->ClearJumpInput();
		}
	}

	CharacterOwner->OnMovementModeChanged(PreviousMovementMode, PreviousCustomMode);

}

void UOrbitCharacterMovementComponent::ApplyAccumulatedForces(float DeltaSeconds)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	
	PendingImpulseToApply = PendingImpulseToApply.ProjectOnTo(GravityDirection);//gdg
	PendingForceToApply = PendingForceToApply.ProjectOnTo(GravityDirection);//gdg
	if (PendingImpulseToApply.Size() != 0.f || PendingForceToApply.Size() != 0.f)//gdg
	{
		if (IsMovingOnGround() && ( PendingImpulseToApply  + (PendingForceToApply * DeltaSeconds)).Size() - (GravityVector * DeltaSeconds).Size() >SMALL_NUMBER )
		{
			SetMovementMode(MOVE_Falling,0);
		}
	}

	//UE_LOG(LogTemp, Warning, TEXT("%d AppAccForce Orbit Vel:%s Impulse:%s"), __LINE__, *Velocity.ToString(), *PendingImpulseToApply.ToString());
	Velocity += PendingImpulseToApply + (PendingForceToApply * DeltaSeconds);
	//UE_LOG(LogTemp, Warning, TEXT("%d AppAccForce Orbit Vel:%s Force:%s"), __LINE__, *Velocity.ToString(), *PendingForceToApply.ToString());

	PendingImpulseToApply = FVector::ZeroVector;
	PendingForceToApply = FVector::ZeroVector;
}

//probably doing something wrong that I need this to prevent errors about missing 2nd param
void UOrbitCharacterMovementComponent::SetMovementMode(EMovementMode NewMovementMode)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	Super::SetMovementMode(NewMovementMode, 0);
}

void UOrbitCharacterMovementComponent::SetMovementMode(EMovementMode NewMovementMode, uint8 NewCustomMode)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	Super::SetMovementMode(NewMovementMode, NewCustomMode);
}

void UOrbitCharacterMovementComponent::PhysCustom(float deltaTime, int32 Iterations)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if (CharacterOwner)
	{
		CharacterOwner->K2_UpdateCustomMovement(deltaTime);//not sure why I'd want to do this?
		switch (CustomMovementMode){
		case CUSTOM_MoonWalking:
			PhysMoonWalking(deltaTime, Iterations);
			break;
		}
	}
}

bool UOrbitCharacterMovementComponent::IsMoonWalking() const
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	return MovementMode == MOVE_Custom && CustomMovementMode == CUSTOM_MoonWalking;//fart
}


void UOrbitCharacterMovementComponent::PhysWalking(float deltaTime, int32 Iterations)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	//UE_LOG(LogTemp, Warning, TEXT("Physics Walking"));
//	SetMovementMode(MOVE_Custom, CUSTOM_MoonWalking);//fart
	Super::PhysWalking(deltaTime, Iterations);
	//all of the above is probably going away
	//end
}
void UOrbitCharacterMovementComponent::PhysMoonWalking(float deltaTime, int32 Iterations)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);

	if (deltaTime < MIN_TICK_TIME)
	{
		////UE_LOG(LogTemp, Warning, TEXT("%d Postpoone tick"), __LINE__);
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
		return;
	}

	if( (!CharacterOwner || !CharacterOwner->Controller) && !bRunPhysicsWithNoController && !HasRootMotion() )
	{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
		Acceleration = FVector::ZeroVector;
		Velocity = FVector::ZeroVector;
		//UE_LOG(LogTemp, Warning, TEXT("%d Zeroed movement "), __LINE__);
		return;
	}

	if (!UpdatedComponent->IsCollisionEnabled())
	{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
		SetMovementMode(MOVE_Custom, CUSTOM_MoonWalking);//fart
		return;
	}

	//checkf(!Velocity.ContainsNaN(), TEXT("PhysMoonWalking: Velocity contains NaN before Iteration (%s: %s)\n%s"), *GetPathNameSafe(this), *GetPathNameSafe(GetOuter()), *Velocity.ToString());
	
	bJustTeleported = false;
	bool bCheckedOrbit = false;
	bool bTriedLedgeMove = false;
	float remainingTime = deltaTime;

	// Perform the move
	while ( (remainingTime >= MIN_TICK_TIME) && (Iterations < MaxSimulationIterations) && (CharacterOwner->Controller || bRunPhysicsWithNoController || HasRootMotion()) )
	{
		Iterations++;
		bJustTeleported = false;
		const float timeTick = GetSimulationTimeStep(remainingTime, Iterations);
		remainingTime -= timeTick;

		// Save current values
		UPrimitiveComponent * const OldBase = GetMovementBase();
		const FVector PreviousBaseLocation = (OldBase != NULL) ? OldBase->GetComponentLocation() : FVector::ZeroVector;
		const FVector OldLocation = UpdatedComponent->GetComponentLocation();
		const FFindFloorResult OldFloor = CurrentFloor;

		// Ensure velocity is horizontal.
		MaintainHorizontalGroundVelocity();
		//Velocity.Z = 0.f;
		//gdg
		const FVector OldVelocity = Velocity;

		// Apply acceleration
		//Acceleration.Z = 0.f;
		//gdg
		if( !HasRootMotion() )
		{
			//UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
			CalcVelocity(timeTick, GroundFriction, false, BrakingDecelerationWalking);
		}
		//checkf(!Velocity.ContainsNaN(), TEXT("PhysMoonWalking: Velocity contains NaN after CalcVelocity (%s: %s)\n%s"), *GetPathNameSafe(this), *GetPathNameSafe(GetOuter()), *Velocity.ToString());

		// Compute move parameters
		const FVector MoveVelocity = Velocity;
		const FVector Delta = timeTick * MoveVelocity;
		const bool bZeroDelta = Delta.IsNearlyZero();
		FStepDownResult StepDownResult;

		if ( bZeroDelta )
		{
			//UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);//not moving
			remainingTime = 0.f;
		}
		else
		{
			//UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);//Hopping and Skating
			// try to move forward
			MoveAlongFloor(MoveVelocity, timeTick, &StepDownResult); // <-- hop/skate must differ in here

			if ( IsFalling() )
			{
			//UE_LOG(LogTemp, Warning, TEXT("%d DesiredDist"), __LINE__);
				// pawn decided to jump up
				const float DesiredDist = Delta.Size();
			//UE_LOG(LogTemp, Warning, TEXT("%d DesiredDist:%f"), __LINE__, DesiredDist);
				if (DesiredDist > KINDA_SMALL_NUMBER)
				{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
					//@todo gdg
					const float ActualDist = (CharacterOwner->GetActorLocation() - OldLocation).Size2D();
					remainingTime += timeTick * (1.f - FMath::Min(1.f,ActualDist/DesiredDist));
				}
				StartNewPhysics(remainingTime, Iterations);
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
				return;
			}
			else if ( IsSwimming() ) //just entered water
			{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
				StartSwimming(OldLocation, OldVelocity, timeTick, remainingTime, Iterations);
				return;
			}
		}

		// Update floor.
		// StepUp might have already done it for us.
		// new target in search for butteryness
		if (StepDownResult.bComputedFloor)
		{
			//like butter when it gets here
			UE_LOG(LogTemp, Warning, TEXT("%d %s: Skate"), __LINE__, __FUNCTIONW__);//Skating
			CurrentFloor = StepDownResult.FloorResult;
		}
		else
		{
			//UE_LOG(LogTemp, Warning, TEXT("%d %s: Hop"), __LINE__, __FUNCTIONW__);//Hopping, sometimes skating too
			
			//can walk smoothly to south if this is commented out!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
			//still have black holes at the poles.

			FindFloor(UpdatedComponent->GetComponentLocation(), CurrentFloor, bZeroDelta, NULL);

			//bWalkableFloor goes from 1 to 0 when transition from walk to crow hop
			//UE_LOG(LogTemp, Warning, TEXT("%d %s: %d"), __LINE__, __FUNCTIONW__, CurrentFloor.bWalkableFloor);
		}

		// check for ledges here
		const bool bCheckLedges = !CanWalkOffLedges();
		if ( bCheckLedges && !CurrentFloor.IsWalkableFloor() )
		{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
			// calculate possible alternate movement
			//const FVector GravDir = FVector(0.f,0.f,-1.f);
			const FVector GravDir = GravityDirection;//gdg
			//UE_LOG(LogTemp, Warning, TEXT("%d GravityDirection:%s"), __LINE__, *GravityDirection.ToString());
			const FVector NewDelta = bTriedLedgeMove ? FVector::ZeroVector : GetLedgeMove(OldLocation, Delta, GravDir);
			if ( !NewDelta.IsZero() )
			{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
				// first revert this move
				RevertMove(OldLocation, OldBase, PreviousBaseLocation, OldFloor, false);

				// avoid repeated ledge moves if the first one fails
				bTriedLedgeMove = true;

				// Try new movement direction
				Velocity = NewDelta/timeTick;
				remainingTime += timeTick;
				continue;
			}
			else
			{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
				// see if it is OK to jump
				// @todo collision : only thing that can be problem is that oldbase has world collision on
				bool bMustJump = bZeroDelta || (OldBase == NULL || (!OldBase->IsCollisionEnabled() && MovementBaseUtility::IsDynamicBase(OldBase)));
				if ( (bMustJump || !bCheckedOrbit) && CheckFall(CurrentFloor.HitResult, Delta, OldLocation, remainingTime, timeTick, Iterations, bMustJump) )
				{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
					return;
				}
				bCheckedOrbit = true;

			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
				// revert this move
				RevertMove(OldLocation, OldBase, PreviousBaseLocation, OldFloor, true);
				remainingTime = 0.f;
				break;
			}

		}
		else
		{
			//UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
			//UE_LOG(LogTemp, Warning, TEXT("you don't have a boner, to penetrate surface"));
			// Validate the floor check
			if (CurrentFloor.IsWalkableFloor())
			{
//			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
				if (ShouldCatchAir(OldFloor, CurrentFloor))
				{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
					CharacterOwner->OnWalkingOffLedge();
					if (IsMovingOnGround())
					{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
						// If still walking, then fall. If not, assume the user set a different mode they want to keep.
						StartFalling(Iterations, remainingTime, timeTick, Delta, OldLocation);//not problem
					}
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
					return;
				}

//			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
				AdjustFloorHeight();
				SetBase(CurrentFloor.HitResult.Component.Get(), CurrentFloor.HitResult.BoneName);
			}
			else if (CurrentFloor.HitResult.bStartPenetrating && remainingTime <= 0.f)
			{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
				// The floor check failed because it started in penetration
				// We do not want to try to move downward because the downward sweep failed, rather we'd like to try to pop out of the floor.
				FHitResult Hit(CurrentFloor.HitResult);
				//Hit.TraceEnd = Hit.TraceStart + FVector(0.f, 0.f, MAX_FLOOR_DIST);
				Hit.TraceEnd = Hit.TraceStart + (-GravityDirection*MAX_FLOOR_DIST);//gdg
				const FVector RequestedAdjustment = GetPenetrationAdjustment(Hit);
				ResolvePenetration(RequestedAdjustment, Hit, CharacterOwner->GetActorRotation());
			}

			// check if just entered water
			if ( IsSwimming() )
			{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
				StartSwimming(OldLocation, Velocity, timeTick, remainingTime, Iterations);
				return;
			}
			// See if we need to start falling.
			if (!CurrentFloor.IsWalkableFloor() && !CurrentFloor.HitResult.bStartPenetrating)
			{
			//UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);//hopping sometimes
				const bool bMustJump = bJustTeleported || bZeroDelta || (OldBase == NULL || 
					(!OldBase->IsCollisionEnabled() && MovementBaseUtility::IsDynamicBase(OldBase)));
				if (
					(bMustJump || !bCheckedOrbit) 
					&& CheckFall(CurrentFloor.HitResult, Delta, OldLocation, remainingTime, timeTick, Iterations, bMustJump) 
					) {
			//UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__); //hopping sometimes
					return;
				}
				bCheckedOrbit = true;
			}
		}

		//GETS to here normally
	//UE_LOG(LogTemp, Warning, TEXT("And the cow jumped over the Moon %d"), __LINE__);

		// Allow overlap events and such to change physics state and velocity
		if (IsMovingOnGround())
		{
//			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
			// Make velocity reflect actual move
			if( !bJustTeleported && !HasRootMotion() && timeTick >= MIN_TICK_TIME)
			{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	//UE_LOG(LogTemp, Warning, TEXT("And the cow jumped over the Moon %d"), __LINE__);
				Velocity = (CharacterOwner->GetActorLocation() - OldLocation) / timeTick;
			}
		}

		// If we didn't move at all this iteration then abort (since future iterations will also be stuck).
		if (CharacterOwner->GetActorLocation() == OldLocation)
		{
			//UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);//not moving
			remainingTime = 0.f;
			break;
		}	
	}

	if (IsMovingOnGround())
	{
//			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
		MaintainHorizontalGroundVelocity();
	}
}
//things that sweep
//ComputeFloorDist
void UOrbitCharacterMovementComponent::AdjustFloorHeight()
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);

	// If we have a floor check that hasn't hit anything, don't adjust height.
	if (!CurrentFloor.bBlockingHit)
	{
		return;
	}

	const float OldFloorDist = CurrentFloor.FloorDist;
	if (CurrentFloor.bLineTrace && OldFloorDist < MIN_FLOOR_DIST)
	{
		// This would cause us to scale unwalkable walls
		return;
	}

	// Move up or down to maintain floor height.
	if (OldFloorDist < MIN_FLOOR_DIST || OldFloorDist > MAX_FLOOR_DIST)
	{
		FHitResult AdjustHit(1.f);
		//const float InitialZ = UpdatedComponent->GetComponentLocation().Z;
		const FVector Initial = UpdatedComponent->GetComponentLocation();//gdg
		const float AvgFloorDist = (MIN_FLOOR_DIST + MAX_FLOOR_DIST) * 0.5f;
		const float MoveDist = AvgFloorDist - OldFloorDist;
		//const float MoveDist = FMath::Abs(AvgFloorDist - OldFloorDist);//gdg
		//SafeMoveUpdatedComponent( FVector(0.f,0.f,MoveDist), CharacterOwner->GetActorRotation(), true, AdjustHit );
		SafeMoveUpdatedComponent( GravityDirection * MoveDist, CharacterOwner->GetActorRotation(), true, AdjustHit );//gdg
		UE_LOG(LogTemp, VeryVerbose, TEXT("Adjust floor height %.3f (Hit = %d)"), MoveDist, AdjustHit.bBlockingHit);

		//	UE_LOG(LogTemp, Warning, TEXT("Mine %d Floor:%.3f BlockingHit:%d"), __LINE__, (GravityDirection * MoveDist).Size() , AdjustHit.bBlockingHit);
		//	UE_LOG(LogTemp, Warning, TEXT("Orig %d Floor:%.3f"), __LINE__, FVector(0.f,0.f,MoveDist).Size() );
		if (!AdjustHit.IsValidBlockingHit())
		{
			//UE_LOG(LogTemp, Warning, TEXT("And the cow jumped over the Moon %d"), __LINE__);//here
			CurrentFloor.FloorDist += MoveDist;
			//CurrentFloor.FloorDist -= MoveDist;
		}
		else if (MoveDist > 0.f)
		{
			//UE_LOG(LogTemp, Warning, TEXT("And the cow jumped over the Moon %d"), __LINE__);
			//const float CurrentZ = UpdatedComponent->GetComponentLocation().Z;
			const FVector Current = UpdatedComponent->GetComponentLocation();//gdg
			//CurrentFloor.FloorDist += CurrentZ - InitialZ;
			CurrentFloor.FloorDist += FVector::Dist(Initial, Current );//gdg
		}
		else
		{
			//UE_LOG(LogTemp, Warning, TEXT("And the cow jumped over the Moon %d"), __LINE__);
			checkSlow(MoveDist < 0.f);
			//const float CurrentZ = UpdatedComponent->GetComponentLocation().Z;
			const FVector Current = UpdatedComponent->GetComponentLocation();//gdg
			//CurrentFloor.FloorDist = CurrentZ - AdjustHit.Location.Z;
			CurrentFloor.FloorDist = FVector::Dist(AdjustHit.Location, Current );//gdg
			
			//UE_LOG(LogTemp, Warning, TEXT("Mine %d Floor:%.3f "), __LINE__,FVector::Dist(InitialZ, CurrentZ ) );
			//UE_LOG(LogTemp, Warning, TEXT("Orig %d Floor:%.3f"), __LINE__, CurrentZ.Z - AdjustHit.Location.Z );

			if (IsWalkable(AdjustHit))
			{
			UE_LOG(LogTemp, Warning, TEXT("And the cow jumped over the Moon %d"), __LINE__);
			CurrentFloor.SetFromSweep(AdjustHit, CurrentFloor.FloorDist, true);
			}
		}

		// Don't recalculate velocity based on this height adjustment, if considering vertical adjustments.
		// Also avoid it if we moved out of penetration
		bJustTeleported |= !bMaintainHorizontalGroundVelocity || (OldFloorDist < 0.f);

	}
		//UE_LOG(LogTemp, Warning, TEXT("%d GravityDirection:%s, Teleported:%d"), __LINE__, *GravityDirection.ToString(),bJustTeleported);
		//UE_LOG(LogTemp, Warning, TEXT("%d Floor Distance OLD DIST:%f, NEW:%f DIFF:%f"), __LINE__, 
			//OldFloorDist, CurrentFloor.FloorDist, (OldFloorDist - CurrentFloor.FloorDist));
}




















bool UOrbitCharacterMovementComponent::IsWalkable(const FHitResult& Hit) const
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if (!Hit.IsValidBlockingHit())
	{
			UE_LOG(LogTemp, Warning, TEXT("%d IsWalkable"), __LINE__);
		// No hit, or starting in penetration
		return false;
	}

	// Never walk up vertical surfaces. bah humbug
	/*
			FVector tmp = Velocity.ProjectOnTo( GravityVector);//gdg this one is smaller than one above
*/
	//if (Hit.ImpactNormal.Z < KINDA_SMALL_NUMBER)
	if (Hit.ImpactNormal.ProjectOnTo( GravityVector).Size() < KINDA_SMALL_NUMBER)//gdg
	{
			UE_LOG(LogTemp, Warning, TEXT("%d IsWalkable NOT"), __LINE__);
		return false;
	}
	
	//float TestWalkableZ = GetWalkableFloorZ();
	FVector TestWalkable = -GravityDirection;//gdg

	// See if this component overrides the walkable floor z.
	const UPrimitiveComponent* HitComponent = Hit.Component.Get();
	
/* no idea what 2do here	if (HitComponent)
	{
		UE_LOG(LogTemp, Warning, TEXT("%d IsWalkable"), __LINE__);
		const FWalkableSlopeOverride& SlopeOverride = HitComponent->GetWalkableSlopeOverride();
		TestWalkableZ = SlopeOverride.ModifyWalkableFloorZ(TestWalkableZ);
	}
	*/

	// Can't walk on this surface if it is too steep.
	//if (Hit.ImpactNormal.Z < TestWalkableZ)
//	if (Hit.ImpactNormal.ProjectOnTo( GravityDirection).Size() < TestWalkable.ProjectOnTo( GravityDirection).Size())//gdg
	float CurrAngle = FVector::DotProduct(TestWalkable, Hit.ImpactNormal);
	if ( CurrAngle < GetWalkableFloorZ())
	{
			UE_LOG(LogTemp, Warning, TEXT("%d IsWalkable NOT"), __LINE__);
		UE_LOG(LogTemp, Warning, TEXT("%d bert.earnie %f !< %f walkableZ"), __LINE__, CurrAngle,GetWalkableFloorZ() );
		return false;
	}

	return true;
}


void UOrbitCharacterMovementComponent::FindFloor(const FVector& CapsuleLocation, FFindFloorResult& OutFloorResult, bool bZeroDelta, const FHitResult* DownwardSweepResult) const
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
			//UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	// No collision, no floor...
	if (!UpdatedComponent->IsCollisionEnabled())
	{
		UE_LOG(LogTemp, Warning, TEXT("%d No collision, no floor"), __LINE__);
		OutFloorResult.Clear();
		return;
	}
			//UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);

	// Increase height check slightly if walking, to prevent floor height adjustment from later invalidating the floor result.
	const float HeightCheckAdjust = (IsMovingOnGround() ? MAX_FLOOR_DIST + KINDA_SMALL_NUMBER : -MAX_FLOOR_DIST);

	float FloorSweepTraceDist = FMath::Max(MAX_FLOOR_DIST, MaxStepHeight + HeightCheckAdjust);
	float FloorLineTraceDist = FloorSweepTraceDist;
	bool bNeedToValidateFloor = true;
	
	// Sweep floor
	if (FloorLineTraceDist > 0.f || FloorSweepTraceDist > 0.f)
	{
			//UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
//		UE_LOG(LogTemp, Warning, TEXT("%d FSTD:%f"), __LINE__, FloorSweepTraceDist);// Always FSTD : 47.400101
		UCharacterMovementComponent* MutableThis = const_cast<UOrbitCharacterMovementComponent*>(this);

		if ( bAlwaysCheckFloor || !bZeroDelta || bForceNextFloorCheck || bJustTeleported )
		{
		//	UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
			//Normally end up here
		// UE_LOG(LogTemp, Warning, TEXT("%d ACF:%d, ZD:%d, FNFC:%d, JT:%d"), __LINE__, bAlwaysCheckFloor, bZeroDelta, 
			//bForceNextFloorCheck, bJustTeleported);
		//True: AlwaysCheckFloor, !bZeroDelta
		//UE_LOG(LogTemp, Warning, TEXT("%d"), __LINE__);
			MutableThis->bForceNextFloorCheck = false;
			//get here a lot
			ComputeFloorDist(CapsuleLocation, FloorLineTraceDist, FloorSweepTraceDist, OutFloorResult, CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius(), DownwardSweepResult);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
			// Force floor check if base has collision disabled or if it does not block us.
			UPrimitiveComponent* MovementBase = CharacterOwner->GetMovementBase();
			const AActor* BaseActor = MovementBase ? MovementBase->GetOwner() : NULL;
			const ECollisionChannel CollisionChannel = UpdatedComponent->GetCollisionObjectType();

			if (MovementBase != NULL)
			{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
				MutableThis->bForceNextFloorCheck = !MovementBase->IsCollisionEnabled()
				|| MovementBase->GetCollisionResponseToChannel(CollisionChannel) != ECR_Block
				|| (MovementBase->Mobility == EComponentMobility::Movable)
				|| MovementBaseUtility::IsDynamicBase(MovementBase)
				|| (Cast<const ADestructibleActor>(BaseActor) != NULL);
			}

			const bool IsActorBasePendingKill = BaseActor && BaseActor->IsPendingKill();

			if ( !bForceNextFloorCheck && !IsActorBasePendingKill && MovementBase )
			{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
				//UE_LOG(LogCharacterMovement, Log, TEXT("%s SKIP check for floor"), *CharacterOwner->GetName());
				OutFloorResult = CurrentFloor;
				bNeedToValidateFloor = false;
			}
			else
			{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
				MutableThis->bForceNextFloorCheck = false;
				ComputeFloorDist(CapsuleLocation, FloorLineTraceDist, FloorSweepTraceDist, OutFloorResult, CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius(), DownwardSweepResult);
			}
		}
	}
		//UE_LOG(LogTemp, Warning, TEXT("Line:%d bNeedValidateFloor:%d bBlockingHit:%d bLineTrace:%d"),
			//__LINE__, bNeedToValidateFloor, OutFloorResult.bBlockingHit, OutFloorResult.bLineTrace);
		//The OutFloorResult.bBlockingHit goes from 1 to 0 when fall off and none of the code below executes

	// OutFloorResult.HitResult is now the result of the vertical floor check.
		//UE_LOG(LogTemp, Warning, TEXT("Line:%d HitResult.PenetrationDepth:%f"),__LINE__, OutFloorResult.HitResult.PenetrationDepth);
	// See if we should try to "perch" at this location.
	if (bNeedToValidateFloor && OutFloorResult.bBlockingHit && !OutFloorResult.bLineTrace)
	{
		//Normally end up here
		const bool bCheckRadius = true;
		if (ShouldComputePerchResult(OutFloorResult.HitResult, bCheckRadius))
		{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
			float MaxPerchFloorDist = FMath::Max(MAX_FLOOR_DIST, MaxStepHeight + HeightCheckAdjust);
			if (IsMovingOnGround())
			{
				MaxPerchFloorDist += FMath::Max(0.f, PerchAdditionalHeight);
			}

			FFindFloorResult PerchFloorResult;
			if (ComputePerchResult(GetValidPerchRadius(), OutFloorResult.HitResult, MaxPerchFloorDist, PerchFloorResult))
			{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
				// Don't allow the floor distance adjustment to push us up too high, or we will move beyond the perch distance and fall next time.
				const float AvgFloorDist = (MIN_FLOOR_DIST + MAX_FLOOR_DIST) * 0.5f;
				const float MoveUpDist = (AvgFloorDist - OutFloorResult.FloorDist);
				if (MoveUpDist + PerchFloorResult.FloorDist >= MaxPerchFloorDist)
				{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
					OutFloorResult.FloorDist = AvgFloorDist;
				}

				// If the regular capsule is on an unwalkable surface but the perched one would allow us to stand, override the normal to be one that is walkable.
				if (!OutFloorResult.bWalkableFloor)
				{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
					OutFloorResult.SetFromLineTrace(PerchFloorResult.HitResult, OutFloorResult.FloorDist, FMath::Min(PerchFloorResult.FloorDist, PerchFloorResult.LineDist), true);
				}
			}
			else
			{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
				// We had no floor (or an invalid one because it was unwalkable), and couldn't perch here, so invalidate floor (which will cause us to start falling).
				OutFloorResult.bWalkableFloor = false;
			UE_LOG(LogTemp, Warning, TEXT("%d %s bWalkableFloor:%d"), __LINE__, __FUNCTIONW__, OutFloorResult.bWalkableFloor);
			}
		} 
	}
}

void UOrbitCharacterMovementComponent::ComputeFloorDist(const FVector& CapsuleLocation, float LineDistance, 
	float SweepDistance, FFindFloorResult& OutFloorResult, float SweepRadius, const FHitResult* DownwardSweepResult) const
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	OutFloorResult.Clear();

	//	UE_LOG(LogTemp, Warning, TEXT("%d Comp SweepDistance:%f, SweepRadius:%f, LineDist:%f"), 
	//		__LINE__,SweepDistance, SweepRadius, LineDistance); //618 Comp
	// No collision, no floor...
	if (!UpdatedComponent->IsCollisionEnabled())
	{
		UE_LOG(LogTemp, Warning, TEXT("%d Comp"), __LINE__);
		return;
	}

	float PawnRadius, PawnHalfHeight;
	CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(PawnRadius, PawnHalfHeight);

	bool bSkipSweep = false;
	if (DownwardSweepResult != NULL && DownwardSweepResult->IsValidBlockingHit())
	{
		//UE_LOG(LogTemp, Warning, TEXT("%d Comp"), __LINE__);
		// Only if the supplied sweep was vertical and downward.
		float VertStart = DownwardSweepResult->TraceStart.ProjectOnTo(GravityDirection).Size();//gdg
		float VertEnd = DownwardSweepResult->TraceEnd.ProjectOnTo(GravityDirection).Size();//gdg
		//if ((DownwardSweepResult->TraceStart.Z > DownwardSweepResult->TraceEnd.Z) &&
		//	(DownwardSweepResult->TraceStart - DownwardSweepResult->TraceEnd).SizeSquared2D() <= KINDA_SMALL_NUMBER)
		if ((VertStart > VertEnd) &&
			FVector::DistSquared(DownwardSweepResult->TraceStart , DownwardSweepResult->TraceEnd) <= KINDA_SMALL_NUMBER)//gdg
		{
		//UE_LOG(LogTemp, Warning, TEXT("%d Comp"), __LINE__);
			// Reject hits that are barely on the cusp of the radius of the capsule
			if (IsWithinEdgeTolerance(DownwardSweepResult->Location, DownwardSweepResult->ImpactPoint, PawnRadius))
			{
		//UE_LOG(LogTemp, Warning, TEXT("%d Comp"), __LINE__);
				// Don't try a redundant sweep, regardless of whether this sweep is usable.
				bSkipSweep = true;

				const bool bIsWalkable = IsWalkable(*DownwardSweepResult);
				//const float FloorDist = (CapsuleLocation.Z - DownwardSweepResult->Location.Z);
				const float FloorDist = (CapsuleLocation - DownwardSweepResult->Location).ProjectOnTo(GravityDirection).Size();
			UE_LOG(LogTemp, Warning, TEXT("%d %s: %d"), __LINE__, __FUNCTIONW__, OutFloorResult.bWalkableFloor);
				OutFloorResult.SetFromSweep(*DownwardSweepResult, FloorDist, bIsWalkable);
			UE_LOG(LogTemp, Warning, TEXT("%d %s: %d"), __LINE__, __FUNCTIONW__, OutFloorResult.bWalkableFloor);
				
				if (bIsWalkable)
				{
		//UE_LOG(LogTemp, Warning, TEXT("%d Comp"), __LINE__);
					// Use the supplied downward sweep as the floor hit result.			
					return;
				}
			}
		}
	}

	// We require the sweep distance to be >= the line distance, otherwise the HitResult can't be interpreted as the sweep result.
	if (SweepDistance < LineDistance)
	{
		//UE_LOG(LogTemp, Warning, TEXT("%d Comp"), __LINE__);
		check(SweepDistance >= LineDistance);
		return;
	}

	bool bBlockingHit = false;
			//UE_LOG(LogTemp, Warning, TEXT("%d %s bBlockingHit:%d"), __LINE__, __FUNCTIONW__, bBlockingHit);
	FCollisionQueryParams QueryParams(NAME_None, false, CharacterOwner);
	FCollisionResponseParams ResponseParam;
	InitCollisionParams(QueryParams, ResponseParam);
	const ECollisionChannel CollisionChannel = UpdatedComponent->GetCollisionObjectType();

	// Sweep test
	if (!bSkipSweep && SweepDistance > 0.f && SweepRadius > 0.f)
	{
		// Use a shorter height to avoid sweeps giving weird results if we start on a surface.
		// This also allows us to adjust out of penetrations.
		const float ShrinkScale = 0.9f;
		const float ShrinkScaleOverlap = 0.1f;
		float ShrinkHeight = (PawnHalfHeight - PawnRadius) * (1.f - ShrinkScale);
		float TraceDist = SweepDistance + ShrinkHeight;

		static const FName ComputeFloorDistName(TEXT("ComputeFloorDistSweep"));
		QueryParams.TraceTag = ComputeFloorDistName;
		FCollisionShape CapsuleShape = FCollisionShape::MakeCapsule(SweepRadius, PawnHalfHeight - ShrinkHeight);
			//UE_LOG(LogTemp, Warning, TEXT("%d %s SweepRadius:%f"), __LINE__, __FUNCTIONW__, PawnHalfHeight - ShrinkHeight); //90
			//UE_LOG(LogTemp, Warning, TEXT("%d %s SweepRadius:%f"), __LINE__, __FUNCTIONW__, SweepRadius);
//		FCollisionShape CapsuleShape = FCollisionShape::MakeSphere(92.0);

		FHitResult Hit(1.f);

		//This must be what fails
		/*
		bBlockingHit = FloorSweepTest(Hit, CapsuleLocation, CapsuleLocation 
		+ FVector(0.f,0.f,-TraceDist), CollisionChannel, CapsuleShape, 
			QueryParams, ResponseParam);
*/

		/*
		started here
		*/
		bBlockingHit = FloorSweepTest(Hit, CapsuleLocation,  
		  CapsuleLocation+(GravityDirection * TraceDist), 
		  CollisionChannel, CapsuleShape, QueryParams, ResponseParam);//gdg
			//UE_LOG(LogTemp, Warning, TEXT("%d %s bBlockingHit:%d"), __LINE__, __FUNCTIONW__, bBlockingHit);
		//UE_LOG(LogTemp, Warning, TEXT("Line:%d Hit.PenetrationDepth:%f TraceDist:%f"),
			//__LINE__, Hit.PenetrationDepth, TraceDist);










		if (bBlockingHit)
		{
		//UE_LOG(LogTemp, Warning, TEXT("%d Comp"), __LINE__);
			// Reject hits adjacent to us, we only care about hits on the bottom portion of our capsule.
			// Check 2D distance to impact point, reject if within a tolerance from radius.
			if (Hit.bStartPenetrating || !IsWithinEdgeTolerance(CapsuleLocation, Hit.ImpactPoint, CapsuleShape.Capsule.Radius))
			{
		//UE_LOG(LogTemp, Warning, TEXT("%d Comp"), __LINE__);
				// Use a capsule with a slightly smaller radius and shorter height to avoid the adjacent object.
				ShrinkHeight = (PawnHalfHeight - PawnRadius) * (1.f - ShrinkScaleOverlap);


















				TraceDist = SweepDistance + ShrinkHeight;
				CapsuleShape.Capsule.Radius = FMath::Max(0.f, CapsuleShape.Capsule.Radius - SWEEP_EDGE_REJECT_DISTANCE - KINDA_SMALL_NUMBER);
				CapsuleShape.Capsule.HalfHeight = FMath::Max(PawnHalfHeight - ShrinkHeight, CapsuleShape.Capsule.Radius);
				Hit.Reset(1.f, false);

				//bBlockingHit = FloorSweepTest(Hit, CapsuleLocation, CapsuleLocation + FVector(0.f,0.f,-TraceDist), CollisionChannel, CapsuleShape, QueryParams, ResponseParam);
				bBlockingHit = FloorSweepTest(Hit, CapsuleLocation, CapsuleLocation + (GravityDirection * TraceDist), 
					CollisionChannel, CapsuleShape, QueryParams, ResponseParam);//gdg
			UE_LOG(LogTemp, Warning, TEXT("%d %s bBlockingHit:%d"), __LINE__, __FUNCTIONW__, bBlockingHit);
			}

			//constant UE_LOG(LogTemp, Warning, TEXT("%d %s: %f %f"), __LINE__, __FUNCTIONW__, SweepDistance, ShrinkHeight);
			// Reduce hit distance by ShrinkHeight because we shrank the capsule for the trace.
			// We allow negative distances here, because this allows us to pull out of penetrations.
			const float MaxPenetrationAdjust = FMath::Max(MAX_FLOOR_DIST, PawnRadius);
			//const float SweepResult = FMath::Max(-MaxPenetrationAdjust, Hit.Time * TraceDist - ShrinkHeight);
			/* This chunk can make get it over the equator, but I don't think this is the source of the hop.
			The crow hopping begins when Hit.Normal.Z < 0. It is like there is something trying to lift it up
			but I can't find the damn thing.
			float tmp = 0.f;
			if (Hit.Normal.Z < 0.f)
			{
		//		tmp = FMath::Max(MaxPenetrationAdjust, Hit.Time * -TraceDist + ShrinkHeight);
				tmp = 10.f;
			}
			else{
				tmp = FMath::Max(-MaxPenetrationAdjust, Hit.Time * TraceDist - ShrinkHeight);
			}
			*/
			const float SweepResult = FMath::Max(-MaxPenetrationAdjust, Hit.Time * TraceDist - ShrinkHeight);
			OutFloorResult.SetFromSweep(Hit, SweepResult, false);

			if (Hit.IsValidBlockingHit() && IsWalkable(Hit))
			{
				if (SweepResult <= SweepDistance)
				{
					// Hit within test distance.
					OutFloorResult.bWalkableFloor = true;
					//get here more frequently when hopping
					return;
				}
			}
		}
	}

	// Since we require a longer sweep than line trace, we don't want to run the line trace if the sweep missed everything.
	// We do however want to try a line trace if the sweep was stuck in penetration.
	/*gdg force it past this and it will still fail below 
	*/
	if (!OutFloorResult.bBlockingHit && !OutFloorResult.HitResult.bStartPenetrating)
	{
		//UE_LOG(LogTemp, Warning, TEXT("%d Comp Neither BlockingHit nor StartPenetrating. Floordist:%f"), __LINE__,SweepDistance);
		//gdg BAILS here when fallingng starts
		OutFloorResult.FloorDist = SweepDistance; //I think that's setting FloorDist to whatever was passed to this func 
		return;
	}

	// Line trace
	if (LineDistance > 0.f)
	{
		const float ShrinkHeight = PawnHalfHeight;
		const FVector LineTraceStart = CapsuleLocation;	
		const float TraceDist = LineDistance + ShrinkHeight;
		//const FVector Down = FVector(0.f, 0.f, -TraceDist);
		const FVector Down = (GravityDirection * TraceDist);//gdg not sure if - or +
		//UE_LOG(LogTemp, Warning, TEXT("%d TraceStart:%s, TraceEnd:%s"), __LINE__,
			//*LineTraceStart.ToString(), *(LineTraceStart + Down).ToString() );

		static const FName FloorLineTraceName = FName(TEXT("ComputeFloorDistLineTrace"));
		QueryParams.TraceTag = FloorLineTraceName;

		FHitResult Hit(1.f);
		bBlockingHit = GetWorld()->LineTraceSingle(Hit, LineTraceStart, LineTraceStart + Down, CollisionChannel, QueryParams, ResponseParam);

			UE_LOG(LogTemp, Warning, TEXT("%d %s bBlockingHit:%d"), __LINE__, __FUNCTIONW__, bBlockingHit);
		if (bBlockingHit)
		{
		//UE_LOG(LogTemp, Warning, TEXT("%d Comp"), __LINE__);
			if (Hit.Time > 0.f)
			{
		//UE_LOG(LogTemp, Warning, TEXT("%d Comp"), __LINE__);
				// Reduce hit distance by ShrinkHeight because we started the trace higher than the base.
				// We allow negative distances here, because this allows us to pull out of penetrations.
				const float MaxPenetrationAdjust = FMath::Max(MAX_FLOOR_DIST, PawnRadius);
				const float LineResult = FMath::Max(-MaxPenetrationAdjust, Hit.Time * TraceDist - ShrinkHeight);

				OutFloorResult.bBlockingHit = true;
			UE_LOG(LogTemp, Warning, TEXT("%d %s bBlockingHit:%d"), __LINE__, __FUNCTIONW__, bBlockingHit);
				if (LineResult <= LineDistance && IsWalkable(Hit))
				{
		//UE_LOG(LogTemp, Warning, TEXT("%d Comp"), __LINE__);
					OutFloorResult.SetFromLineTrace(Hit, OutFloorResult.FloorDist, LineResult, true);
					return;
				}
			}
		}
	}
	
		//UE_LOG(LogTemp, Warning, TEXT("%d Comp"), __LINE__);//tag:tennis
	// No hits were acceptable.
	OutFloorResult.bWalkableFloor = false;
			UE_LOG(LogTemp, Warning, TEXT("%d %s bWalkableFloor:%d"), __LINE__, __FUNCTIONW__, OutFloorResult.bWalkableFloor);
			UE_LOG(LogTemp, Warning, TEXT("%d %s bBlockingHit:%d"), __LINE__, __FUNCTIONW__, bBlockingHit);
	OutFloorResult.FloorDist = SweepDistance;
}
 void UOrbitCharacterMovementComponent::MaintainHorizontalGroundVelocity() { //if (Velocity.Z != 0.f)
	//FVector tmp = Velocity.ProjectOnTo( GravityDirection );//gdg
	//FVector tmp = GravityDirection.ProjectOnTo( Velocity );//gdg this one is smaller than one above
	//FVector tmp = GravityVector.ProjectOnTo( Velocity );//gdg this one is smaller than one above
	FVector tmp = Velocity.ProjectOnTo(GravityVector);//gdg this one is smaller than one above
	//UE_LOG(LogTemp, Warning, TEXT("tmp:%s"), *tmp.ToString());
	if (tmp.Size() != 0.f)//gdg
	{
		if (bMaintainHorizontalGroundVelocity)
		{
			// Ramp movement already maintained the velocity, so we just want to remove the vertical component.
			//Velocity.Z = 0.f; //Wish I knew, vertical relative to what?
			//			Velocity = Velocity + Velocity.ProjectOnTo(GravityDirection);//gdg
			Velocity -= tmp;

		}
		else
		{
			// Rescale velocity to be horizontal but maintain magnitude of last update.
			//			Velocity = Velocity.SafeNormal2D() * Velocity.Size();
			//not sure I grasp the intent here

			//Velocity = (Velocity - tmp) * tmp.Size();//gdg ... definately not right
			Velocity = (Velocity - tmp);//gdg ... legit? Math is hard.
			//UE_LOG(LogTemp, Warning, TEXT("Vel:%s"), *Velocity.ToString());
		}
	}
}

bool UOrbitCharacterMovementComponent::DoJump(bool bReplayingMoves)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if ( CharacterOwner && CharacterOwner->CanJump() )
	{
		FVector js = -GravityDirection * JumpZVelocity; //gdg shouldn't that be speed?
		// Don't jump if we can't move up/down.
		//if (!bConstrainToPlane || FMath::Abs(PlaneConstraintNormal.Z) != 1.f)
		if (!bConstrainToPlane || FMath::Abs(PlaneConstraintNormal.ProjectOnTo(GravityDirection).Size()) != 1.f)
		{
		//	Velocity.Z = JumpZVelocity;
			Velocity += js;//gdg
	//UE_LOG(LogTemp, Warning, TEXT("%d Jumped, going to fall"), __LINE__ );
			SetMovementMode(MOVE_Falling);
			return true;
		}
	}
	
	return false;
}

bool UOrbitCharacterMovementComponent::IsMovingOnGround() const
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if (!CharacterOwner || !UpdatedComponent)
	{
		return false;
	}

	return (MovementMode == MOVE_Walking) || (MovementMode == MOVE_Custom && CustomMovementMode == CUSTOM_MoonWalking);
}

void UOrbitCharacterMovementComponent::SetPostLandedPhysics(const FHitResult& Hit)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if( CharacterOwner )
	{
		if ( GetPhysicsVolume()->bWaterVolume && CanEverSwim() )
		{
			SetMovementMode(MOVE_Swimming);
		}
		else
		{
			const FVector PreImpactAccel = Acceleration + (IsFalling() ? GravityVector : FVector::ZeroVector);
			const FVector PreImpactVelocity = Velocity;
			//SetMovementMode(MOVE_Walking);
			SetMovementMode(MOVE_Custom, CUSTOM_MoonWalking);//gdg
			ApplyImpactPhysicsForces(Hit, PreImpactAccel, PreImpactVelocity);
		}
	}
}

void UOrbitCharacterMovementComponent::OnTeleported()
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	bJustTeleported = true;
	if (!HasValidData())
	{
		return;
	}

	// Find floor at current location
	UpdateFloorFromAdjustment();
	SaveBaseLocation();

	// Validate it. We don't want to pop down to walking mode from very high off the ground, but we'd like to keep walking if possible.
	UPrimitiveComponent* OldBase = CharacterOwner->GetMovementBase();
	UPrimitiveComponent* NewBase = NULL;
	
	//if (OldBase && CurrentFloor.IsWalkableFloor() && CurrentFloor.FloorDist <= MAX_FLOOR_DIST && Velocity.Z <= 0.f)
	if (OldBase && CurrentFloor.IsWalkableFloor() && CurrentFloor.FloorDist <= MAX_FLOOR_DIST && Velocity.ProjectOnTo(GravityDirection).Size() <= KINDA_SMALL_NUMBER)//gdg
	{
		// Close enough to land or just keep walking.
		NewBase = CurrentFloor.HitResult.Component.Get();
	}
	else
	{
		CurrentFloor.Clear();
	}

	// If we were walking but no longer have a valid base or floor, start falling.
	if (!CurrentFloor.IsWalkableFloor() || (OldBase && !NewBase))
	{
		if (DefaultLandMovementMode == MOVE_Walking || (DefaultLandMovementMode==MOVE_Custom && CustomMovementMode==CUSTOM_MoonWalking) )
		{
	//UE_LOG(LogTemp, Warning, TEXT("%d Teleported, going to fall"), __LINE__ );
			SetMovementMode(MOVE_Falling);
			//SetMovementMode(MOVE_Custom, CUSTOM_MoonWalking);//gdg
		}
		else
		{
			SetDefaultMovementMode();
		}
	}
}
void UOrbitCharacterMovementComponent::CalcAvoidanceVelocity(float DeltaTime)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	//UE_LOG(LogTemp, Warning, TEXT("CALC AVOID"));
	Super::CalcAvoidanceVelocity(DeltaTime);
}

void UOrbitCharacterMovementComponent::SetDefaultMovementMode()
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	// check for water volume
	if ( IsInWater() && CanEverSwim() )
	{
		SetMovementMode(DefaultWaterMovementMode);
	}
	else if ( !CharacterOwner || MovementMode != DefaultLandMovementMode )
	{
		SetMovementMode(DefaultLandMovementMode);

		// Avoid 1-frame delay if trying to walk but walking fails at this location.
		if ( (MovementMode == MOVE_Walking || (MovementMode==MOVE_Custom && CustomMovementMode==CUSTOM_MoonWalking) ) && GetMovementBase() == NULL)
		{
	//UE_LOG(LogTemp, Warning, TEXT("%d DefMoveMode, going to fall"), __LINE__ );
			SetMovementMode(MOVE_Falling);
		}
	}
}

float UOrbitCharacterMovementComponent::GetMaxJumpHeight() const
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	//const float Gravity = GetGravityZ();
	const float Gravity = GravityMagnitude;//gdg
	if (FMath::Abs(Gravity) > KINDA_SMALL_NUMBER)
	{
		return FMath::Square(JumpZVelocity) / (-2.f * Gravity);
	}
	else
	{
		return 0.f;
	}
}

FVector UOrbitCharacterMovementComponent::GetFallingLateralAcceleration(float DeltaTime)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	// No acceleration in Z
	//FVector OrbitAcceleration = FVector(Acceleration.X, Acceleration.Y, 0.f);
	FVector OrbitAcceleration = Acceleration - GravityVector;//gdg

	// bound acceleration, falling object has minimal ability to impact acceleration
	//if (!HasRootMotion() && OrbitAcceleration.SizeSquared2D() > 0.f)
	if (!HasRootMotion() && OrbitAcceleration.SizeSquared() > 0.f)//gdg
	{
		OrbitAcceleration = GetAirControl(DeltaTime, AirControl, OrbitAcceleration);
#ifdef VERSION27
		OrbitAcceleration = OrbitAcceleration.GetClampedToMaxSize(GetMaxAcceleration());
#else
		OrbitAcceleration = OrbitAcceleration.ClampMaxSize(GetMaxAcceleration());
#endif
	}

	return OrbitAcceleration;
}

void UOrbitCharacterMovementComponent::PhysFalling(float deltaTime, int32 Iterations)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if (deltaTime < MIN_TICK_TIME)
	{
		return;
	}

	FVector OrbitAcceleration = GetFallingLateralAcceleration(deltaTime);
	//OrbitAcceleration.Z = 0.f;
	OrbitAcceleration -= OrbitAcceleration.ProjectOnTo(GravityDirection);//gdg
	//const bool bHasAirControl = (OrbitAcceleration.SizeSquared2D() > 0.f);
	const bool bHasAirControl = (OrbitAcceleration.SizeSquared() > 0.f);

	float remainingTime = deltaTime;
	while( (remainingTime >= MIN_TICK_TIME) && (Iterations < MaxSimulationIterations) )
	{
		Iterations++;
		const float timeTick = GetSimulationTimeStep(remainingTime, Iterations);
		remainingTime -= timeTick;
		
		const FVector OldLocation = CharacterOwner->GetActorLocation();
		const FRotator PawnRotation = CharacterOwner->GetActorRotation();
		bJustTeleported = false;

		FVector OldVelocity = Velocity;
		FVector VelocityNoAirControl = Velocity;

		// Apply input
		if (!HasRootMotion())
		{
			// Compute VelocityNoAirControl
			if (bHasAirControl)
			{
				// Find velocity *without* acceleration.
				TGuardValue<FVector> RestoreAcceleration(Acceleration, FVector::ZeroVector);
				TGuardValue<FVector> RestoreVelocity(Velocity, Velocity);
				//Velocity.Z = 0.f;
				Velocity = Velocity - Velocity.ProjectOnTo(GravityVector);
				CalcVelocity(timeTick, FallingLateralFriction, false, BrakingDecelerationFalling);
				//VelocityNoAirControl = FVector(Velocity.X, Velocity.Y, OldVelocity.Z);
				//VelocityNoAirControl = Velocity - (GravityDirection.ProjectOnTo(OldVelocity));//gdg
				VelocityNoAirControl = Velocity - OldVelocity.ProjectOnTo(GravityVector);//gdg
			}

			// Compute Velocity
			{
				// Acceleration = OrbitAcceleration for CalcVelocity(), but we restore it after using it.
				TGuardValue<FVector> RestoreAcceleration(Acceleration, OrbitAcceleration);
				//Velocity.Z = 0.f;
				//Velocity = Velocity - (GravityDirection.ProjectOnTo(OldVelocity));//gdg
				Velocity = Velocity - (OldVelocity.ProjectOnTo(GravityVector));//gdg
				CalcVelocity(timeTick, FallingLateralFriction, false, BrakingDecelerationFalling);
				//Velocity.Z = OldVelocity.Z;
				Velocity = OldVelocity; //gdg
			}

			// Just copy Velocity to VelocityNoAirControl if they are the same (ie no acceleration).
			if (!bHasAirControl)
			{
				VelocityNoAirControl = Velocity;
			}
		}

		// Apply gravity
		//const FVector Gravity(0.f, 0.f, GetGravityZ());
		const FVector Gravity=GravityVector;//gdg
		Velocity = NewFallVelocity(Velocity, Gravity, timeTick);
		VelocityNoAirControl = NewFallVelocity(VelocityNoAirControl, Gravity, timeTick);
		const FVector AirControlAccel = (Velocity - VelocityNoAirControl) / timeTick;

		//if( bNotifyApex && CharacterOwner->Controller && (Velocity.Z <= 0.f) )
		if( bNotifyApex && CharacterOwner->Controller && ((GravityDirection.ProjectOnTo(Velocity)).Size() <= 0.f) )
		{
			// Just passed jump apex since now going down
			bNotifyApex = false;
			NotifyJumpApex();
		}

		// Move
		FHitResult Hit(1.f);
		FVector Adjusted = 0.5f*(OldVelocity + Velocity) * timeTick;
		SafeMoveUpdatedComponent( Adjusted, PawnRotation, true, Hit);
		
		if (!HasValidData())
		{
			return;
		}
		
		float LastMoveTimeSlice = timeTick;
		float subTimeTickRemaining = timeTick * (1.f - Hit.Time);
		
		if ( IsSwimming() ) //just entered water
		{
			remainingTime += subTimeTickRemaining;
			StartSwimming(OldLocation, OldVelocity, timeTick, remainingTime, Iterations);
			return;
		}
		else if ( Hit.bBlockingHit )
		{
		//UE_LOG(LogTemp, Warning, TEXT("%d BlockingHit "), __LINE__ );
			if (IsValidLandingSpot(UpdatedComponent->GetComponentLocation(), Hit))
			{
				remainingTime += subTimeTickRemaining;
		//UE_LOG(LogTemp, Warning, TEXT("%d Good place to land "), __LINE__ );
				ProcessLanded(Hit, remainingTime, Iterations);
				return;
			}
			else
			{
				// Compute impact deflection based on final velocity, not integration step.
				// This allows us to compute a new velocity from the deflected vector, and ensures the full gravity effect is included in the slide result.
				Adjusted = Velocity * timeTick;

				// See if we can convert a normally invalid landing spot (based on the hit result) to a usable one.
				if (!Hit.bStartPenetrating && ShouldCheckForValidLandingSpot(timeTick, Adjusted, Hit))
				{
					const FVector PawnLocation = UpdatedComponent->GetComponentLocation();
					FFindFloorResult FloorResult;
					//const FFindFloorResult* DownwardSweepResult;
					const FHitResult* DownwardSweepResult = new FHitResult();
					//FindFloor(PawnLocation, FloorResult, false);
					FindFloor(PawnLocation, FloorResult, false, DownwardSweepResult);
					//Super::FindFloor(PawnLocation, FloorResult, false);//gdg
					if (FloorResult.IsWalkableFloor() && IsValidLandingSpot(PawnLocation, FloorResult.HitResult))
					{
						remainingTime += subTimeTickRemaining;
						ProcessLanded(FloorResult.HitResult, remainingTime, Iterations);
						return;
					}
				}

				HandleImpact(Hit, LastMoveTimeSlice, Adjusted);
				
				// If we've changed physics mode, abort.
				if (!HasValidData() || !IsFalling())
				{
					return;
				}

				// Limit air control based on what we hit.
				// We moved to the impact point using air control, but may want to deflect from there based on a limited air control acceleration.
				if (bHasAirControl)
				{
					const bool bCheckLandingSpot = false; // we already checked above.
					const FVector AirControlDeltaV = LimitAirControl(LastMoveTimeSlice, AirControlAccel, Hit, bCheckLandingSpot) * LastMoveTimeSlice;
					Adjusted = (VelocityNoAirControl + AirControlDeltaV) * LastMoveTimeSlice;
				}

				const FVector OldHitNormal = Hit.Normal;
				const FVector OldHitImpactNormal = Hit.ImpactNormal;				
				FVector Delta = ComputeSlideVector(Adjusted, 1.f - Hit.Time, OldHitNormal, Hit);

				// Compute velocity after deflection (only gravity component for RootMotion)
				if (subTimeTickRemaining > KINDA_SMALL_NUMBER && !bJustTeleported)
				{
					const FVector NewVelocity = (Delta / subTimeTickRemaining);
					//Velocity = HasRootMotion() ? FVector(Velocity.X, Velocity.Y, NewVelocity.Z) : NewVelocity;
					Velocity = HasRootMotion() ? Velocity.ProjectOnTo( NewVelocity) : NewVelocity;//gdg not sure here
				}

				if (subTimeTickRemaining > KINDA_SMALL_NUMBER && (Delta | Adjusted) > 0.f)
				{
					// Move in deflected direction.
					SafeMoveUpdatedComponent( Delta, PawnRotation, true, Hit);
					
					if (Hit.bBlockingHit)
					{
						// hit second wall
						LastMoveTimeSlice = subTimeTickRemaining;
						subTimeTickRemaining = subTimeTickRemaining * (1.f - Hit.Time);

						if (IsValidLandingSpot(UpdatedComponent->GetComponentLocation(), Hit))
						{
							remainingTime += subTimeTickRemaining;
							ProcessLanded(Hit, remainingTime, Iterations);
							return;
						}

						HandleImpact(Hit, LastMoveTimeSlice, Delta);

						// If we've changed physics mode, abort.
						if (!HasValidData() || !IsFalling())
						{
							return;
						}

						// Act as if there was no air control on the last move when computing new deflection.
						//if (bHasAirControl && Hit.Normal.Z > VERTICAL_SLOPE_NORMAL_Z)
						if (bHasAirControl && Hit.Normal.Size() > (VERTICAL_SLOPE_NORMAL_Z*GravityDirection).Size())
						{
							const FVector LastMoveNoAirControl = VelocityNoAirControl * LastMoveTimeSlice;
							Delta = ComputeSlideVector(LastMoveNoAirControl, 1.f, OldHitNormal, Hit);
						}
							//gdg not sure what to do there

						FVector PreTwoWallDelta = Delta;
						TwoWallAdjust(Delta, Hit, OldHitNormal);

						// Limit air control, but allow a slide along the second wall.
						if (bHasAirControl)
						{
							const bool bCheckLandingSpot = false; // we already checked above.
							const FVector AirControlDeltaV = LimitAirControl(subTimeTickRemaining, AirControlAccel, Hit, bCheckLandingSpot) * subTimeTickRemaining;

							// Only allow if not back in to first wall
							if (FVector::DotProduct(AirControlDeltaV, OldHitNormal) > 0.f)
							{
								Delta += (AirControlDeltaV * subTimeTickRemaining);
							}
						}

						// Compute velocity after deflection (only gravity component for RootMotion)
						if (subTimeTickRemaining > KINDA_SMALL_NUMBER && !bJustTeleported)
						{
							const FVector NewVelocity = (Delta / subTimeTickRemaining);
							//gdg todo
							//Velocity = HasRootMotion() ? FVector(Velocity.X, Velocity.Y, NewVelocity.Z) : NewVelocity;
							Velocity = HasRootMotion() ? Velocity.ProjectOnTo(NewVelocity) : NewVelocity;
						}

						// bDitch=true means that pawn is straddling two slopes, neither of which he can stand on
						//bool bDitch = ( (OldHitImpactNormal.Z > 0.f) && (Hit.ImpactNormal.Z > 0.f) && (FMath::Abs(Delta.Z) <= KINDA_SMALL_NUMBER) && ((Hit.ImpactNormal | OldHitImpactNormal) < 0.f) );
						bool bDitch = ( (OldHitImpactNormal.ProjectOnTo(GravityDirection).Size() > 0.f) && 
							(Hit.ImpactNormal.ProjectOnTo(GravityDirection).Size() > 0.f) && 
							(FMath::Abs(Delta.ProjectOnTo(GravityDirection).Size()) <= KINDA_SMALL_NUMBER) && 
							((Hit.ImpactNormal | OldHitImpactNormal) < 0.f) );//gdg
						SafeMoveUpdatedComponent( Delta, PawnRotation, true, Hit);
						if ( Hit.Time == 0 )
						{
							// if we are stuck then try to side step
#ifdef VERSION27
							FVector SideDelta = (OldHitNormal + Hit.ImpactNormal).GetSafeNormal2D();
#else
							FVector SideDelta = (OldHitNormal + Hit.ImpactNormal).SafeNormal2D();
#endif
							if ( SideDelta.IsNearlyZero() )
							{
#ifdef VERSION27
								SideDelta = FVector(OldHitNormal.Y, -OldHitNormal.X, 0).GetSafeNormal();
#else
								SideDelta = FVector(OldHitNormal.Y, -OldHitNormal.X, 0).SafeNormal();
#endif
							}
							SafeMoveUpdatedComponent( SideDelta, PawnRotation, true, Hit);
						}
							
						if ( bDitch || IsValidLandingSpot(UpdatedComponent->GetComponentLocation(), Hit) || Hit.Time == 0  )
						{
							remainingTime = 0.f;
							ProcessLanded(Hit, remainingTime, Iterations);
							return;
						}
						//gdg todo
						//else if (GetPerchRadiusThreshold() > 0.f && Hit.Time == 1.f && OldHitImpactNormal.Z >= GetWalkableFloorZ())
						else if (GetPerchRadiusThreshold() > 0.f && Hit.Time == 1.f && 
							OldHitImpactNormal.ProjectOnTo(GravityDirection).Size() >= GravityDirection.Size())//gdg???
						{
							// We might be in a virtual 'ditch' within our perch radius. This is rare.
							const FVector PawnLocation = CharacterOwner->GetActorLocation();
							//const float ZMovedDist = FMath::Abs(PawnLocation.Z - OldLocation.Z);
							const float ZMovedDist = FMath::Abs( (PawnLocation.ProjectOnTo(GravityDirection) - OldLocation.ProjectOnTo(GravityDirection)).Size());
							//const float MovedDist2DSq = (PawnLocation - OldLocation).SizeSquared2D();
							const float MovedDist2DSq = (PawnLocation - OldLocation).SizeSquared();
							if (ZMovedDist <= 0.2f * timeTick && MovedDist2DSq <= 4.f * timeTick)
							{
								Velocity.X += 0.25f * GetMaxSpeed() * (FMath::FRand() - 0.5f);
								Velocity.Y += 0.25f * GetMaxSpeed() * (FMath::FRand() - 0.5f);
								Velocity.Z += 0.25f * GetMaxSpeed() * (FMath::FRand() - 0.5f);//gdg
								//Velocity = FMath::Max<float>(JumpZVelocity * 0.25f, 1.f);
								Velocity = Velocity.ProjectOnTo(FMath::Max<float>(JumpZVelocity * 0.25f, 1.f)*GravityDirection);
								Delta = Velocity * timeTick;
								SafeMoveUpdatedComponent(Delta, PawnRotation, true, Hit);
							}
						}
					}
				}
			}
		}

		//if (Velocity.SizeSquared() <= KINDA_SMALL_NUMBER * 10.f)//gdg
		if (Velocity.SizeSquared2D() <= KINDA_SMALL_NUMBER * 10.f)
		{
			Velocity.X = 0.f;
			Velocity.Y = 0.f;
		}
	}
}

//might not need this
bool UOrbitCharacterMovementComponent::SafeMoveUpdatedComponent(const FVector& Delta, const FRotator& NewRotation, bool bSweep, FHitResult& OutHit)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if (UpdatedComponent == NULL)
	{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);//!!!
		//UE_LOG(LogTemp, Warning, TEXT("%d SafeMove"), __LINE__ );
		OutHit.Reset(1.f);
		return false;
	}

	bool bMoveResult = MoveUpdatedComponent(Delta, NewRotation, bSweep, &OutHit);
	//always returns true

	// Handle initial penetrations
	if (OutHit.bStartPenetrating && UpdatedComponent)
	{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);//!!!
		const FVector RequestedAdjustment = GetPenetrationAdjustment(OutHit);
		if (ResolvePenetration(RequestedAdjustment, OutHit, NewRotation))
		{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);//!!!
			// Retry original move
			bMoveResult = MoveUpdatedComponent(Delta, NewRotation, bSweep, &OutHit);
		UE_LOG(LogTemp, Warning, TEXT("%d SafeMove, bMoveResult:%d"), __LINE__, bMoveResult );
		}
	}

	return bMoveResult;
}
void UOrbitCharacterMovementComponent::StartFalling(int32 Iterations, float remainingTime, float timeTick, const FVector& Delta, const FVector& subLoc)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	/*
	Don't have source compiled to know where this is actually being called
	but might not matter since it seems the landing is that bad part.
	*/
	// start falling 
	const float DesiredDist = Delta.Size();
	const float ActualDist = (CharacterOwner->GetActorLocation() - subLoc).Size();
	remainingTime = (DesiredDist < KINDA_SMALL_NUMBER) 
					? 0.f
					: remainingTime + timeTick * (1.f - FMath::Min(1.f,ActualDist/DesiredDist));

	//Velocity.Z = 0.f;			
	//gdg
	Velocity = Velocity - Velocity.ProjectOnTo(GravityDirection);
	if ( IsMovingOnGround() )
	{
		// This is to catch cases where the first frame of PIE is executed, and the
		// level is not yet visible. In those cases, the player will fall out of the
		// world... So, don't set MOVE_Falling straight away.
		if ( !GIsEditor || (GetWorld()->HasBegunPlay() && (GetWorld()->GetTimeSeconds() >= 1.f)) )
		{
		//UE_LOG(LogTemp, Warning, TEXT("%d Start falling "), __LINE__ );
			SetMovementMode(MOVE_Falling); //default behavior if script didn't change physics
		}
		else
		{
			// Make sure that the floor check code continues processing during this delay.
			bForceNextFloorCheck = true;
		}
	}
	StartNewPhysics(remainingTime,Iterations);
}
void UOrbitCharacterMovementComponent::ProcessLanded(const FHitResult& Hit, float remainingTime, int32 Iterations)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if( CharacterOwner && CharacterOwner->ShouldNotifyLanded(Hit) )
	{
		//UE_LOG(LogTemp, Warning, TEXT("%d post landed"), __LINE__ );
		CharacterOwner->Landed(Hit);
	}
	if( IsFalling() )
	{
		//UE_LOG(LogTemp, Warning, TEXT("%d post landed"), __LINE__ );
		SetPostLandedPhysics(Hit);
	}
/*	if (PathFollowingComp.IsValid())
	{
		//UE_LOG(LogTemp, Warning, TEXT("%d post landed"), __LINE__ );
		//PathFollowingComp->OnLanded();
	}
	*/

	StartNewPhysics(remainingTime, Iterations);
}

bool UOrbitCharacterMovementComponent::IsValidLandingSpot(const FVector& CapsuleLocation, const FHitResult& Hit) const
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if (!Hit.bBlockingHit)
	{
		return false;
	}

	// Skip some checks if penetrating. Penetration will be handled by the FindFloor call (using a smaller capsule)
	if (!Hit.bStartPenetrating)
	{
		// Reject unwalkable floor normals.
		if (!IsWalkable(Hit))
		{
			return false;
		}

		float PawnRadius, PawnHalfHeight;
		CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(PawnRadius, PawnHalfHeight);

		// Reject hits that are above our lower hemisphere (can happen when sliding down a vertical surface).
		const float LowerHemisphereZ = Hit.Location.Z - PawnHalfHeight + PawnRadius;
		//bolloxed up here
		//UE_LOG(LogTemp, Warning, TEXT("%d checking landing spot. Imp:%f, Hemi:%f"), __LINE__ ,Hit.ImpactPoint.Z,LowerHemisphereZ);
		/*
		if (Hit.ImpactPoint.Z >= LowerHemisphereZ)
		{
			return false;
		}
		*/

		// Reject hits that are barely on the cusp of the radius of the capsule
		if (!IsWithinEdgeTolerance(Hit.Location, Hit.ImpactPoint, PawnRadius))
		{
		//UE_LOG(LogTemp, Warning, TEXT("%d checking landing spot"), __LINE__ );
			return false;
		}
	}

		//UE_LOG(LogTemp, Warning, TEXT("%d checking landing spot"), __LINE__ );
	FFindFloorResult FloorResult;
	FindFloor(CapsuleLocation, FloorResult, false, &Hit);

	if (!FloorResult.IsWalkableFloor())
	{
		//UE_LOG(LogTemp, Warning, TEXT("%d checking landing spot"), __LINE__ );
		return false;
	}

		//UE_LOG(LogTemp, Warning, TEXT("%d checking landing spot"), __LINE__ );
	return true;
}

bool UOrbitCharacterMovementComponent::FloorSweepTest(
	FHitResult& OutHit,
	const FVector& Start,
	const FVector& End,
	ECollisionChannel TraceChannel,
	const struct FCollisionShape& CollisionShape,
	const struct FCollisionQueryParams& Params,
	const struct FCollisionResponseParams& ResponseParam
	) const
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	bool bBlockingHit = false;

	if (!bUseFlatBaseForFloorChecks)
	{
		bBlockingHit = GetWorld()->SweepSingle(OutHit, Start, End, GetPawnOwner()->GetActorRotation().Quaternion(), TraceChannel, CollisionShape, Params, ResponseParam);//gdg
		//bBlockingHit = GetWorld()->SweepSingle(OutHit, Start, End, FQuat::Identity, TraceChannel, CollisionShape, Params, ResponseParam);//original
			//UE_LOG(LogTemp, Warning, TEXT("%d %s bBlockingHit:%d"), __LINE__, __FUNCTIONW__, bBlockingHit);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("%d FloorSweepTest, Use flat base for floor checks"),__LINE__);
		//UE_LOG(LogTemp, Warning, TEXT("%d Sweeping the Floor"), __LINE__ );
		// Test with a box that is enclosed by the capsule.
		const float CapsuleRadius = CollisionShape.GetCapsuleRadius();
		const float CapsuleHeight = CollisionShape.GetCapsuleHalfHeight();
		const FCollisionShape BoxShape = FCollisionShape::MakeBox(FVector(CapsuleRadius * 0.707f, CapsuleRadius * 0.707f, CapsuleHeight));

		// First test with the box rotated so the corners are along the major axes (ie rotated 45 degrees).
		//bBlockingHit = GetWorld()->SweepSingle(OutHit, Start, End, FQuat(FVector(0.f, 0.f, -1.f), PI * 0.25f), TraceChannel, BoxShape, Params, ResponseParam);
		bBlockingHit = GetWorld()->SweepSingle(OutHit, Start, End, GetPawnOwner()->GetActorRotation().Add(0,45,0).Quaternion(), TraceChannel, BoxShape, Params, ResponseParam);
			UE_LOG(LogTemp, Warning, TEXT("%d %s bBlockingHit:%d"), __LINE__, __FUNCTIONW__, bBlockingHit);

		if (!bBlockingHit)
		{
		UE_LOG(LogTemp, Warning, TEXT("%d FloorSweepTest, NOT a blocking hit, trying again"),__LINE__);
		//UE_LOG(LogTemp, Warning, TEXT("%d Sweeping the Floor"), __LINE__ );
			// Test again with the same box, not rotated.
			OutHit.Reset(1.f, false);			
			//bBlockingHit = GetWorld()->SweepSingle(OutHit, Start, End, FQuat::Identity, TraceChannel, BoxShape, Params, ResponseParam);
			bBlockingHit = GetWorld()->SweepSingle(OutHit, Start, End, FQuat::Identity, TraceChannel, BoxShape, Params, ResponseParam);
			UE_LOG(LogTemp, Warning, TEXT("%d %s bBlockingHit:%d"), __LINE__, __FUNCTIONW__, bBlockingHit);
		}
	}

	return bBlockingHit;
}

bool UOrbitCharacterMovementComponent::IsWithinEdgeTolerance(const FVector& CapsuleLocation, const FVector& TestImpactPoint, const float CapsuleRadius) const
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	const float DistFromCenterSqo = (TestImpactPoint - CapsuleLocation).SizeSquared2D();
	const float DistFromCenterSq = FVector::DistSquared(TestImpactPoint, CapsuleLocation);
	const float ReducedRadiusSq = FMath::Square(FMath::Max(KINDA_SMALL_NUMBER, CapsuleRadius - SWEEP_EDGE_REJECT_DISTANCE));
	return true;
	return DistFromCenterSq < ReducedRadiusSq;
}

bool UOrbitCharacterMovementComponent::CanStepUp(const FHitResult& Hit) const
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if (!Hit.IsValidBlockingHit() || !HasValidData() || MovementMode == MOVE_Falling)
	{
	UE_LOG(LogTemp, Warning, TEXT("%d Can't StepUP"), __LINE__);
		return false;
	}
	// No component for "fake" hits when we are on a known good base.
	const UPrimitiveComponent* HitComponent = Hit.Component.Get();
	if (!HitComponent)
	{
	UE_LOG(LogTemp, Warning, TEXT("%d CanStepUP"), __LINE__);
		return true;
	}

	if (!HitComponent->CanCharacterStepUp(CharacterOwner))
	{
	UE_LOG(LogTemp, Warning, TEXT("%d CanStepUP"), __LINE__);
		return false;
	}

	// No actor for "fake" hits when we are on a known good base.
	const AActor* HitActor = Hit.GetActor();
	if (!HitActor)
	{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
		 return true;
	}

	if (!HitActor->CanBeBaseForCharacter(CharacterOwner))
	{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
		return false;
	}

			//UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	return true;
}


bool UOrbitCharacterMovementComponent::StepUp(const FVector& InGravDir, const FVector& Delta, const FHitResult &InHit, FStepDownResult* OutStepDownResult)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	/*
	//gdg Crude and I forgot why I did it...
	FVector* StupidShit;
	StupidShit = (FVector*) (&InGravDir);
	*StupidShit = GravityDirection;
*/
	//UE_LOG(LogTemp, Warning, TEXT("%d StepUP Gravity Vect:%s"), __LINE__, *InGravDir.ToString());
//	SCOPE_CYCLE_COUNTER(super::STAT_CharStepUp);


	if (!CanStepUp(InHit))	  
	{
	UE_LOG(LogTemp, Warning, TEXT("%d Can't step up"), __LINE__);
		return false;
	}

	if (MaxStepHeight <= 0.f)
	{
	UE_LOG(LogTemp, Warning, TEXT("%d Can't step up"), __LINE__);
		return false;
	}

	const FVector OldLocation = UpdatedComponent->GetComponentLocation();
	float PawnRadius, PawnHalfHeight;
	CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(PawnRadius, PawnHalfHeight);

	// Don't bother stepping up if top of capsule is hitting something.
	//const float InitialImpactZ = InHit.ImpactPoint.Z;
	//gdg really uncertain here
	//if (InitialImpactZ > OldLocation.Z + (PawnHalfHeight - PawnRadius))
	if (InHit.ImpactPoint.ProjectOnTo(GravityDirection).Size() > OldLocation.ProjectOnTo(GravityDirection).Size() + (PawnHalfHeight - PawnRadius))//gdg
	{
	UE_LOG(LogTemp, Warning, TEXT("%d Can't step up"), __LINE__);
		return false;
	}

	// Don't step up if the impact is below us
	//if (InitialImpactZ <= OldLocation.Z - PawnHalfHeight)
	if (InHit.ImpactPoint.ProjectOnTo(GravityDirection).Size() <= OldLocation.ProjectOnTo(GravityDirection).Size() - PawnHalfHeight)//gdg
	{
	UE_LOG(LogTemp, Warning, TEXT("%d Can't step up"), __LINE__);
		return false;
	}

	const FVector GravDir = InGravDir.GetSafeNormal();
	if (GravDir.IsZero())
	{
	UE_LOG(LogTemp, Warning, TEXT("%d Can't step up"), __LINE__);
		return false;
	}
	//here		UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	float StepTravelUpHeight = MaxStepHeight;
	float StepTravelDownHeight = StepTravelUpHeight;
	const float StepSideZ = -1.f * (InHit.ImpactNormal | GravDir);
	//float PawnInitialFloorBaseZ = OldLocation.Z - PawnHalfHeight;
	float PawnInitialFloorBase = OldLocation.ProjectOnTo(GravityDirection).Size() - PawnHalfHeight;
	//float PawnFloorPointZ = PawnInitialFloorBaseZ;
	float PawnFloorPoint = PawnInitialFloorBase;

	if (IsMovingOnGround() && CurrentFloor.IsWalkableFloor())
	{
	//		UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
		// Since we float a variable amount off the floor, we need to enforce max step height off the actual point of impact with the floor.
		const float FloorDist = FMath::Max(0.f, CurrentFloor.FloorDist);
		//PawnInitialFloorBaseZ -= FloorDist;
		PawnInitialFloorBase -= FloorDist;
		StepTravelUpHeight = FMath::Max(StepTravelUpHeight - FloorDist, 0.f);
		StepTravelDownHeight = (MaxStepHeight + MAX_FLOOR_DIST*2.f);

		const bool bHitVerticalFace = !IsWithinEdgeTolerance(InHit.Location, InHit.ImpactPoint, PawnRadius);
		if (!CurrentFloor.bLineTrace && !bHitVerticalFace)
		{
	//1987		UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
			//PawnFloorPointZ = CurrentFloor.HitResult.ImpactPoint.Z;
			PawnFloorPoint = CurrentFloor.HitResult.ImpactPoint.ProjectOnTo(GravityDirection).Size();
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
			// Base floor point is the base of the capsule moved down by how far we are hovering over the surface we are hitting.
			//PawnFloorPointZ -= CurrentFloor.FloorDist;
			PawnFloorPoint -= CurrentFloor.FloorDist;
		}
	}

	// Scope our movement updates, and do not apply them until all intermediate moves are completed.
	FScopedMovementUpdate ScopedStepUpMovement(UpdatedComponent, EScopedUpdate::DeferredUpdates);

	// step up - treat as vertical wall
	FHitResult SweepUpHit(1.f);
	const FRotator PawnRotation = CharacterOwner->GetActorRotation();
	SafeMoveUpdatedComponent(-GravDir * StepTravelUpHeight, PawnRotation, true, SweepUpHit);

	// step fwd
	FHitResult Hit(1.f);
	SafeMoveUpdatedComponent( Delta, PawnRotation, true, Hit);

	// If we hit something above us and also something ahead of us, we should notify about the upward hit as well.
	// The forward hit will be handled later (in the bSteppedOver case below).
	// In the case of hitting something above but not forward, we are not blocked from moving so we don't need the notification.
	if (SweepUpHit.bBlockingHit && Hit.bBlockingHit)
	{
			//UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
		//HandleImpact(SweepUpHit);
		HandleImpact(SweepUpHit, 0.f, FVector::ZeroVector); //gdg
	}

	// Check result of forward movement
	if (Hit.bBlockingHit)
	{
//			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
		if (Hit.bStartPenetrating)
		{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
			// Undo movement
			ScopedStepUpMovement.RevertMove();
			return false;
		}

		// pawn ran into a wall
	//	HandleImpact(Hit);
		HandleImpact(Hit, 0.f, FVector::ZeroVector);//gdg
		if ( IsFalling() )
		{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
			return true;
		}

		// adjust and try again
		const float ForwardHitTime = Hit.Time;
		const float ForwardSlideAmount = SlideAlongSurface(Delta, 1.f - Hit.Time, Hit.Normal, Hit, true);
		
		if (IsFalling())
		{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
			ScopedStepUpMovement.RevertMove();
	UE_LOG(LogTemp, Warning, TEXT("%d Can't step up"), __LINE__);
			return false;
		}

		// If both the forward hit and the deflection got us nowhere, there is no point in this step up.
		if (ForwardHitTime == 0.f && ForwardSlideAmount == 0.f)
		{
			ScopedStepUpMovement.RevertMove();
	UE_LOG(LogTemp, Warning, TEXT("%d Can't step up"), __LINE__);
			return false;
		}
	}
	
	// Step down
	SafeMoveUpdatedComponent(GravDir * StepTravelDownHeight, CharacterOwner->GetActorRotation(), true, Hit);

	// If step down was initially penetrating abort the step up
	if (Hit.bStartPenetrating)
	{
		ScopedStepUpMovement.RevertMove();
	UE_LOG(LogTemp, Warning, TEXT("%d Can't step up"), __LINE__);
		return false;
	}

	FStepDownResult StepDownResult;
	if (Hit.IsValidBlockingHit())
	{	
			//UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
		// See if this step sequence would have allowed us to travel higher than our max step height allows.
		//const float DeltaZ = Hit.ImpactPoint.Z - PawnFloorPointZ;
		const float DeltaZ = Hit.ImpactPoint.ProjectOnTo(GravityDirection).Size() - PawnFloorPoint;
		if (DeltaZ > MaxStepHeight)
		{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
			//UE_LOG(LogCharacterMovement, VeryVerbose, TEXT("- Reject StepUp (too high Height %.3f) up from floor base %f to %f"), DeltaZ, PawnInitialFloorBaseZ, NewLocation.Z);
			ScopedStepUpMovement.RevertMove();
	UE_LOG(LogTemp, Warning, TEXT("%d Can't step up"), __LINE__);
			return false;
		}

		// Reject unwalkable surface normals here.
		if (!IsWalkable(Hit))
		{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
			// Reject if normal opposes movement direction
			const bool bNormalTowardsMe = (Delta | Hit.ImpactNormal) < 0.f;
			if (bNormalTowardsMe)
			{
				UE_LOG(LogCharacterMovement, VeryVerbose, TEXT("- Reject StepUp (unwalkable normal %s opposed to movement)"), *Hit.ImpactNormal.ToString());
				ScopedStepUpMovement.RevertMove();
	UE_LOG(LogTemp, Warning, TEXT("%d Can't step up"), __LINE__);
				return false;
			}

			// Also reject if we would end up being higher than our starting location by stepping down.
			// It's fine to step down onto an unwalkable normal below us, we will just slide off. Rejecting those moves would prevent us from being able to walk off the edge.
			if (Hit.Location.ProjectOnTo(GravityDirection).Size() > OldLocation.ProjectOnTo(GravityDirection).Size())//gdg
			{
				UE_LOG(LogCharacterMovement, VeryVerbose, TEXT("- Reject StepUp (unwalkable normal %s above old position)"), *Hit.ImpactNormal.ToString());
				ScopedStepUpMovement.RevertMove();
	UE_LOG(LogTemp, Warning, TEXT("%d Can't step up"), __LINE__);
				return false;
			}
		}

		// Reject moves where the downward sweep hit something very close to the edge of the capsule. This maintains consistency with FindFloor as well.
		if (!IsWithinEdgeTolerance(Hit.Location, Hit.ImpactPoint, PawnRadius))
		{
			UE_LOG(LogCharacterMovement, VeryVerbose, TEXT("- Reject StepUp (outside edge tolerance)"));
			ScopedStepUpMovement.RevertMove();
	UE_LOG(LogTemp, Warning, TEXT("%d Can't step up"), __LINE__);
			return false;
		}

		// Don't step up onto invalid surfaces if traveling higher.
		if (DeltaZ > 0.f && !CanStepUp(Hit))
		{
			UE_LOG(LogCharacterMovement, VeryVerbose, TEXT("- Reject StepUp (up onto surface with !CanStepUp())"));
			ScopedStepUpMovement.RevertMove();
	UE_LOG(LogTemp, Warning, TEXT("%d Can't step up"), __LINE__);
			return false;
		}

		// See if we can validate the floor as a result of this step down. In almost all cases this should succeed, and we can avoid computing the floor outside this method.
		if (OutStepDownResult != NULL)
		{
			//UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
			FindFloor(UpdatedComponent->GetComponentLocation(), StepDownResult.FloorResult, false, &Hit);

			// Reject unwalkable normals if we end up higher than our initial height.
			// It's fine to walk down onto an unwalkable surface, don't reject those moves.
			//if (Hit.Location.Z > OldLocation.Z)
			if (Hit.Location.ProjectOnTo(GravityDirection).Size() > OldLocation.ProjectOnTo(GravityDirection).Size())//gdg
			{
			//UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
				// We should reject the floor result if we are trying to step up an actual step where we are not able to perch (this is rare).
				// In those cases we should instead abort the step up and try to slide along the stair.
				if (!StepDownResult.FloorResult.bBlockingHit && StepSideZ < MAX_STEP_SIDE_Z)
				{
					ScopedStepUpMovement.RevertMove();
	UE_LOG(LogTemp, Warning, TEXT("%d Can't step up"), __LINE__);
					return false;
				}
			}

			//UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
			StepDownResult.bComputedFloor = true;
		}
	}
	
	// Copy step down result.
	if (OutStepDownResult != NULL)
	{
		//	UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
		*OutStepDownResult = StepDownResult;
	}

	// Don't recalculate velocity based on this height adjustment, if considering vertical adjustments.
	bJustTeleported |= !bMaintainHorizontalGroundVelocity;

		//	UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	return true;
}

bool UOrbitCharacterMovementComponent::ShouldCheckForValidLandingSpot(float DeltaTime, const FVector& Delta, const FHitResult& Hit) const
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	// See if we hit an edge of a surface on the lower portion of the capsule.
	// In this case the normal will not equal the impact normal, and a downward sweep may find a walkable surface on top of the edge.
	//if (Hit.Normal.Z > KINDA_SMALL_NUMBER && !Hit.Normal.Equals(Hit.ImpactNormal))
	if (Hit.Normal.ProjectOnTo(GravityDirection).Size() > KINDA_SMALL_NUMBER && !Hit.Normal.Equals(Hit.ImpactNormal))
	{
		const FVector PawnLocation = UpdatedComponent->GetComponentLocation();
		if (IsWithinEdgeTolerance(PawnLocation, Hit.ImpactPoint, CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius()))
		{						
			return true;
		}
	}

	return false;
}


bool UOrbitCharacterMovementComponent::ComputePerchResult(const float TestRadius, const FHitResult& InHit, const float InMaxFloorDist, FFindFloorResult& OutPerchFloorResult) const
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if (InMaxFloorDist <= 0.f)
	{
		return 0.f;
	}

	// Sweep further than actual requested distance, because a reduced capsule radius means we could miss some hits that the normal radius would contact.
	float PawnRadius, PawnHalfHeight;
	CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(PawnRadius, PawnHalfHeight);

	//const float InHitAboveBase = FMath::Max(0.f, InHit.ImpactPoint.Z - (InHit.Location.Z - PawnHalfHeight));
	const float InHitAboveBase = FMath::Max(0.f, InHit.ImpactPoint.ProjectOnTo(GravityDirection).Size() - 
		(InHit.Location.ProjectOnTo(GravityDirection).Size() - PawnHalfHeight));
	const float PerchLineDist = FMath::Max(0.f, InMaxFloorDist - InHitAboveBase);
	const float PerchSweepDist = FMath::Max(0.f, InMaxFloorDist);

	const float ActualSweepDist = PerchSweepDist + PawnRadius;
	//ComputeFloorDist(InHit.Location, PerchLineDist, ActualSweepDist, OutPerchFloorResult, TestRadius);
	//gdg
	UE_LOG(LogTemp, Warning, TEXT("%d FuUUUUUKKKKK!!!"), __LINE__);

	if (!OutPerchFloorResult.IsWalkableFloor())
	{
		return false;
	}
	else if (InHitAboveBase + OutPerchFloorResult.FloorDist > InMaxFloorDist)
	{
		// Hit something past max distance
		OutPerchFloorResult.bWalkableFloor = false;
			UE_LOG(LogTemp, Warning, TEXT("%d %s bWalkableFloor:%d"), __LINE__, __FUNCTIONW__, OutPerchFloorResult.bWalkableFloor);
		return false;
	}

	return true;
}

void UOrbitCharacterMovementComponent::HandleImpact(FHitResult const& Impact, float TimeSlice, const FVector& MoveDelta)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if (CharacterOwner)
	{
		CharacterOwner->MoveBlockedBy(Impact);
	}

/*	if (PathFollowingComp.IsValid())
	{	// Also notify path following!
	//PathFollowingComp->OnMoveBlockedBy(Impact);
		//gdg
	}
	*/

	APawn* OtherPawn = Cast<APawn>(Impact.GetActor());
	if (OtherPawn)
	{
		NotifyBumpedPawn(OtherPawn);
	}

	if (bEnablePhysicsInteraction)
	{
		//const FVector ForceAccel = Acceleration + (IsFalling() ? FVector(0.f, 0.f, GetGravityZ()) : FVector::ZeroVector);
		const FVector ForceAccel = Acceleration + (IsFalling() ? GravityVector : FVector::ZeroVector);
		ApplyImpactPhysicsForces(Impact, ForceAccel, Velocity);
	}
}

FVector UOrbitCharacterMovementComponent::ConstrainInputAcceleration(const FVector& InputAcceleration) const
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	FVector NewAccel = InputAcceleration;

	// walking or falling pawns ignore up/down sliding
	if (IsMovingOnGround() || IsFalling())
	{
		// This definately had something to do with sticking at equator
		//	NewAccel.Z = 0.f;
		//gdg
		NewAccel = NewAccel - NewAccel.ProjectOnTo(GravityDirection);
	}

	return NewAccel;
}
void UOrbitCharacterMovementComponent::ApplyImpactPhysicsForces(const FHitResult& Impact, const FVector& ImpactAcceleration, const FVector& ImpactVelocity)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if (bEnablePhysicsInteraction && Impact.bBlockingHit)
	{
		UPrimitiveComponent* ImpactComponent = Impact.GetComponent();
		if (ImpactComponent != NULL && ImpactComponent->IsAnySimulatingPhysics())
		{
			FVector ForcePoint = Impact.ImpactPoint;

			FBodyInstance* BI = ImpactComponent->GetBodyInstance(Impact.BoneName);

			float BodyMass = 1.0f;

			if (BI != NULL)
			{
				BodyMass = FMath::Max(BI->GetBodyMass(), 1.0f);

				FBox Bounds = BI->GetBodyBounds();

				FVector Center, Extents;
				Bounds.GetCenterAndExtents(Center, Extents);

				if (!Extents.IsNearlyZero())
				{
					//ForcePoint.Z = Center.Z + Extents.Z * PushForcePointZOffsetFactor;
					ForcePoint = Center.ProjectOnTo(GravityDirection) + Extents.ProjectOnTo(GravityDirection) * PushForcePointZOffsetFactor;
				}
			}

			FVector Force = Impact.ImpactNormal * -1.0f;

			float PushForceModificator = 1.0f;

			const FVector ComponentVelocity = ImpactComponent->GetPhysicsLinearVelocity();
			const FVector VirtualVelocity = ImpactAcceleration.IsZero() ? ImpactVelocity : ImpactAcceleration.GetSafeNormal() * GetMaxSpeed();

			float Dot = 0.0f;

			if (bScalePushForceToVelocity && !ComponentVelocity.IsNearlyZero())
			{			
				Dot = ComponentVelocity | VirtualVelocity;

				if (Dot > 0.0f && Dot < 1.0f)
				{
					PushForceModificator *= Dot;
				}
			}

			if (bPushForceScaledToMass)
			{
				PushForceModificator *= BodyMass;
			}

			Force *= PushForceModificator;

			if (ComponentVelocity.IsNearlyZero())
			{
				Force *= InitialPushForceFactor;
				ImpactComponent->AddImpulseAtLocation(Force, ForcePoint, Impact.BoneName);
			}
			else
			{
				Force *= PushForceFactor;
				ImpactComponent->AddForceAtLocation(Force, ForcePoint, Impact.BoneName);
			}
		}
	}
}

float UOrbitCharacterMovementComponent::GetPerchRadiusThreshold() const
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	float ret;
	// Don't allow negative values.
	ret=FMath::Max(0.f, PerchRadiusThreshold);
//			UE_LOG(LogTemp, Warning, TEXT("%d %s: Perch: %f"), __LINE__, __FUNCTIONW__);
	return ret;
//	return PerchRadiusThreshold;//gdg
}


float UOrbitCharacterMovementComponent::GetValidPerchRadius() const
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if (CharacterOwner)
	{
		const float PawnRadius = CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius();
		return FMath::Clamp(PawnRadius - GetPerchRadiusThreshold(), 0.1f, PawnRadius);
	}
	return 0.f;
}


bool UOrbitCharacterMovementComponent::ShouldComputePerchResult(const FHitResult& InHit, bool bCheckRadius) const
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if (!InHit.IsValidBlockingHit())
	{
		return false;
	}

	// Don't try to perch if the edge radius is very small.
	if (GetPerchRadiusThreshold() <= SWEEP_EDGE_REJECT_DISTANCE)
	{
		return false;
	}

	if (bCheckRadius)
	{
		//const float DistFromCenterSq = (InHit.ImpactPoint - InHit.Location).SizeSquared2D();
		const float DistFromCenterSq = FVector::DistSquared(InHit.ImpactPoint , InHit.Location);//gdg
		const float StandOnEdgeRadius = GetValidPerchRadius();
		if (DistFromCenterSq <= FMath::Square(StandOnEdgeRadius))
		{
			// Already within perch radius.
			return false;
		}
	}
	
	return true;
}

void UOrbitCharacterMovementComponent::SmoothClientPosition(float DeltaSeconds)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if (!HasValidData() || GetNetMode() != NM_Client)
	{
		return;
	}

	FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character();
	if (ClientData && ClientData->bSmoothNetUpdates && CharacterOwner->GetMesh() && !CharacterOwner->GetMesh()->IsSimulatingPhysics())
	{
		// smooth interpolation of mesh translation to avoid popping of other client pawns unless under a low tick rate
		if (DeltaSeconds < ClientData->SmoothNetUpdateTime)
		{
			ClientData->MeshTranslationOffset = (ClientData->MeshTranslationOffset * (1.f - DeltaSeconds / ClientData->SmoothNetUpdateTime));
		}
		else
		{
			ClientData->MeshTranslationOffset = FVector::ZeroVector;
		}
		/*
		if (IsMovingOnGround())
		{
			// don't smooth Z position if walking on ground
			ClientData->MeshTranslationOffset.Z = 0;
		}
*/
		const FVector NewRelTranslation = CharacterOwner->ActorToWorld().InverseTransformVectorNoScale(ClientData->MeshTranslationOffset + CharacterOwner->GetBaseTranslationOffset());
		CharacterOwner->GetMesh()->SetRelativeLocation(NewRelTranslation);
	}
}

//skipping for now. Doesn't seem to be involved in southern skipping
void UOrbitCharacterMovementComponent::ApplyRepulsionForce(float DeltaSeconds)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if (UpdatedComponent && RepulsionForce > 0.0f)
	{
		FCollisionQueryParams QueryParams;
		QueryParams.bReturnFaceIndex = false;
		QueryParams.bReturnPhysicalMaterial = false;

		const FCollisionShape CollisionShape = UpdatedComponent->GetCollisionShape();
		const float CapsuleRadius = CollisionShape.GetCapsuleRadius();
		const float CapsuleHalfHeight = CollisionShape.GetCapsuleHalfHeight();
		const float RepulsionForceRadius = CapsuleRadius * 1.2f;
		const float StopBodyDistance = 2.5f;

		const TArray<FOverlapInfo>& Overlaps = UpdatedComponent->GetOverlapInfos();
		const FVector MyLocation = UpdatedComponent->GetComponentLocation();

	//UE_LOG(LogTemp, Warning, TEXT("NumOverlaps:%d"), Overlaps.Num());
		for (int32 i=0; i < Overlaps.Num(); i++)
		{
			const FOverlapInfo& Overlap = Overlaps[i];

			UPrimitiveComponent* OverlapComp = Overlap.OverlapInfo.Component.Get();
			if (!OverlapComp || OverlapComp->Mobility < EComponentMobility::Movable)
			{ 
				continue; 
			}

			FName BoneName = NAME_None;
			if (Overlap.GetBodyIndex() != INDEX_NONE && OverlapComp->IsA(USkinnedMeshComponent::StaticClass()))
			{
				BoneName = ((USkinnedMeshComponent*)OverlapComp)->GetBoneName(Overlap.GetBodyIndex());
			}

			// Use the body instead of the component for cases where we have multi-body overlaps enabled
			FBodyInstance* OverlapBody = OverlapComp->GetBodyInstance(BoneName);

			if (!OverlapBody)
			{
				UE_LOG(LogCharacterMovement, Warning, TEXT("%s could not find overlap body for bone %s"), *GetName(), *BoneName.ToString());
				continue;
			}

			// Early out if this is not a destructible and the body is not simulated
			bool bIsCompDestructible = OverlapComp->IsA(UDestructibleComponent::StaticClass());
			if (!bIsCompDestructible && !OverlapBody->IsInstanceSimulatingPhysics())
			{
				continue;
			}

			FTransform BodyTransform = OverlapBody->GetUnrealWorldTransform();

			FVector BodyVelocity = OverlapBody->GetUnrealWorldVelocity();
			FVector BodyLocation = BodyTransform.GetLocation();

			// Trace to get the hit location on the capsule
			FHitResult Hit;
			//WTF is this?
			bool bHasHit = UpdatedComponent->LineTraceComponent(Hit, BodyLocation, 
																FVector(MyLocation.X, MyLocation.Y, BodyLocation.Z),
																QueryParams);

			FVector HitLoc = Hit.ImpactPoint;
			bool bIsPenetrating = Hit.bStartPenetrating || Hit.PenetrationDepth > 2.5f;

			// If we didn't hit the capsule, we're inside the capsule
			if(!bHasHit) 
			{ 
				HitLoc = BodyLocation; 
				bIsPenetrating = true;
			}

			const float DistanceNow = (HitLoc - BodyLocation).SizeSquared2D();
			const float DistanceLater = (HitLoc - (BodyLocation + BodyVelocity * DeltaSeconds)).SizeSquared2D();

			if (BodyLocation.SizeSquared() > 0.1f && bHasHit && DistanceNow < StopBodyDistance && !bIsPenetrating)
			{
	UE_LOG(LogTemp, Warning, TEXT("ApplyRepulsionForce"));
				OverlapBody->SetLinearVelocity(FVector(0.0f, 0.0f, 0.0f), false);
			}
			else if (DistanceLater <= DistanceNow || bIsPenetrating)
			{
				FVector ForceCenter(MyLocation.X, MyLocation.Y, bHasHit ? HitLoc.Z : MyLocation.Z);

				if (!bHasHit)
				{
					ForceCenter.Z = FMath::Clamp(BodyLocation.Z, MyLocation.Z - CapsuleHalfHeight, MyLocation.Z + CapsuleHalfHeight);
				}

				OverlapBody->AddRadialForceToBody(ForceCenter, RepulsionForceRadius, RepulsionForce * Mass, ERadialImpulseFalloff::RIF_Constant);
			}
		}
	}
}


void UOrbitCharacterMovementComponent::PhysicsVolumeChanged( APhysicsVolume* NewVolume )
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if (!HasValidData())
	{
		return;
	}
	if ( NewVolume && NewVolume->bWaterVolume )
	{
		// just entered water
		if ( !CanEverSwim() )
		{
			// AI needs to stop any current moves
			/*
			if (PathFollowingComp.IsValid())
			{
				PathFollowingComp->AbortMove(TEXT("water"));
			}			
			*/
		}
		else if ( !IsSwimming() )
		{
			SetMovementMode(MOVE_Swimming);
		}
	}
	else if ( IsSwimming() )
	{
		// just left the water - check if should jump out
		SetMovementMode(MOVE_Falling);
		FVector JumpDir(0.f);
		FVector WallNormal(0.f);
		//if ( Acceleration.Z > 0.f && ShouldJumpOutOfWater(JumpDir)
		if ( Acceleration.ProjectOnTo(GravityDirection).Size() > 0.f && ShouldJumpOutOfWater(JumpDir)//gdg
			&& ((JumpDir | Acceleration) > 0.f) && CheckWaterJump(JumpDir, WallNormal) ) 
		{
			JumpOutOfWater(WallNormal);
			//Velocity.Z = OutofWaterZ; //set here so physics uses this for remainder of tick
			Velocity += -GravityDirection*OutofWaterZ;
		}
	}
}

bool UOrbitCharacterMovementComponent::ShouldJumpOutOfWater(FVector& JumpDir)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	AController* OwnerController = CharacterOwner->GetController();
	if (OwnerController)
	{
		const FRotator ControllerRot = OwnerController->GetControlRotation();
		//if ( (Velocity.Z > 0.0f) && (ControllerRot.Pitch > JumpOutOfWaterPitch) )
		if ( (Velocity.ProjectOnTo(GravityDirection).Size() > 0.0f) && (ControllerRot.Pitch > JumpOutOfWaterPitch) )
		{
			// if Pawn is going up and looking up, then make him jump
			JumpDir = ControllerRot.Vector();
			return true;
		}
	}
	
	return false;
}

void UOrbitCharacterMovementComponent::MoveSmooth(const FVector& InVelocity, const float DeltaSeconds, FStepDownResult* OutStepDownResult)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if (!HasValidData())
	{
		return;
	}

	// Custom movement mode.
	// Custom movement may need an update even if there is zero velocity.
	if (MovementMode == MOVE_Custom)
	{
		FScopedMovementUpdate ScopedMovementUpdate(UpdatedComponent, bEnableScopedMovementUpdates ? EScopedUpdate::DeferredUpdates : EScopedUpdate::ImmediateUpdates);
		PhysCustom(DeltaSeconds, 0);
		return;
	}

	FVector Delta = InVelocity * DeltaSeconds;
	if (Delta.IsZero())
	{
		return;
	}

	FScopedMovementUpdate ScopedMovementUpdate(UpdatedComponent, bEnableScopedMovementUpdates ? EScopedUpdate::DeferredUpdates : EScopedUpdate::ImmediateUpdates);

	if (IsMovingOnGround())
	{
		MoveAlongFloor(InVelocity, DeltaSeconds, OutStepDownResult);
	}
	else
	{
		FHitResult Hit(1.f);
		SafeMoveUpdatedComponent(Delta, CharacterOwner->GetActorRotation(), true, Hit);
	
		if (Hit.IsValidBlockingHit())
		{
			bool bSteppedUp = false;

			if (IsFlying())
			{
				if (CanStepUp(Hit))
				{
					OutStepDownResult = NULL; // No need for a floor when not walking.
					//if (FMath::Abs(Hit.ImpactNormal.Z) < 0.2f)
					if (FMath::Abs(Hit.ImpactNormal.ProjectOnTo(GravityDirection).Size()) < 0.2f)
					{
						//const FVector GravDir = FVector(0.f,0.f,-1.f);
						const FVector GravDir = GravityDirection;
						const FVector DesiredDir = Delta.GetSafeNormal();
						const float UpDown = GravDir | DesiredDir;
						if ((UpDown < 0.5f) && (UpDown > -0.2f))
						{
							bSteppedUp = StepUp(GravDir, Delta * (1.f - Hit.Time), Hit, OutStepDownResult);
						}			
					}
				}
			}
				
			// If StepUp failed, try sliding.
			if (!bSteppedUp)
			{
				SlideAlongSurface(Delta, 1.f - Hit.Time, Hit.Normal, Hit, false);
			}
		}
	}
}

void UOrbitCharacterMovementComponent::DisplayDebug(UCanvas* Canvas, const FDebugDisplayInfo& DebugDisplay, float& YL, float& YPos)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if ( CharacterOwner == NULL )
	{
		return;
	}

	Canvas->SetDrawColor(255,255,255);
	UFont* RenderFont = GEngine->GetSmallFont();
	FString T = FString::Printf(TEXT("CHARACTER MOVEMENT Floor %s Crouched %i"), *CurrentFloor.HitResult.ImpactNormal.ToString(), IsCrouching());
	Canvas->DrawText(RenderFont, T, 4.0f, YPos );
	YPos += YL;

	T = FString::Printf(TEXT("Updated Component: %s"), *UpdatedComponent->GetName());
	Canvas->DrawText(RenderFont, T, 4.0f, YPos );
	YPos += YL;

	T = FString::Printf(TEXT("bForceMaxAccel: %i"), bForceMaxAccel);
	Canvas->DrawText(RenderFont, T, 4.0f, YPos );
	YPos += YL;

	APhysicsVolume * PhysicsVolume = GetPhysicsVolume();

	const UPrimitiveComponent* BaseComponent = CharacterOwner->GetMovementBase();
	const AActor* BaseActor = BaseComponent ? BaseComponent->GetOwner() : NULL;

	T = FString::Printf(TEXT("%s In physicsvolume %s on base %s component %s gravity %f"), *GetMovementName(), (PhysicsVolume ? *PhysicsVolume->GetName() : TEXT("None")),
		(BaseActor ? *BaseActor->GetName() : TEXT("None")), (BaseComponent ? *BaseComponent->GetName() : TEXT("None")), GetGravityZ());
	Canvas->DrawText(RenderFont, T, 4.0f, YPos );
	YPos += YL;
	//mine
	Canvas->SetDrawColor(255,255,127);
	T = FString::Printf(TEXT("Acceleration: %s, Grav:%s "), *Acceleration.ToString(), *GravityVector.ToString());
	Canvas->DrawText(RenderFont, T, 4.0f, YPos );
	YPos += YL;
	
	Canvas->SetDrawColor(255,255,127);
	T = FString::Printf(TEXT("FLOOR: BlockingHit:%d, LineTrace:%d, WalkableFloor:%d"), 
		CurrentFloor.bBlockingHit, CurrentFloor.bLineTrace, CurrentFloor.bWalkableFloor );
	T.Append(FString::Printf(TEXT("FloorDist:%f, LineDist:%f"), CurrentFloor.FloorDist, CurrentFloor.LineDist));
	Canvas->DrawText(RenderFont, T, 4.0f, YPos );
	YPos += YL;
}

//This gets called but doesn't appear to do anything in parent
//oh but it does something, can't walk w/o it
void UOrbitCharacterMovementComponent::MoveAlongFloor(const FVector& InVelocity, float DeltaSeconds, FStepDownResult* OutStepDownResult)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
//	Super::MoveAlongFloor(InVelocity, DeltaSeconds, OutStepDownResult);
//	UE_LOG(LogTemp, Warning, TEXT("%d MoveAlongFloor v2:%s"), __LINE__, *InVelocity.ToString());

	//const FVector Delta = FVector(InVelocity.X, InVelocity.Y, 0.f) * DeltaSeconds;
	const FVector Delta =  InVelocity * DeltaSeconds;
	
	if (!CurrentFloor.IsWalkableFloor())
	{
		//	UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);//HOPPING!!!
		return;
	}

	// Move along the current floor
	FHitResult Hit(1.f);
	FVector RampVector = ComputeGroundMovementDelta(Delta, CurrentFloor.HitResult, CurrentFloor.bLineTrace);
	SafeMoveUpdatedComponent(RampVector, CharacterOwner->GetActorRotation(), true, Hit);

/*	UE_LOG(LogTemp, Warning, TEXT("%d %s Hit.IsValidBlockingHit():%d"), __LINE__, __FUNCTIONW__,Hit.IsValidBlockingHit());
	UE_LOG(LogTemp, Warning, TEXT("%d %s RampV:%s, CharActorRot:%s"), __LINE__, __FUNCTIONW__,*RampVector.ToString(),
		*CharacterOwner->GetActorRotation().ToString());
		*/
	float LastMoveTimeSlice = DeltaSeconds;
	
	if (Hit.bStartPenetrating)
	{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
		OnCharacterStuckInGeometry();
	}

	if (Hit.IsValidBlockingHit())
	{
	//		UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);//Skating!!!!
		// We impacted something (most likely another ramp, but possibly a barrier).
		float PercentTimeApplied = Hit.Time;
		//if ((Hit.Time > 0.f) && (Hit.Normal.Z > KINDA_SMALL_NUMBER) && IsWalkable(Hit))
		if ((Hit.Time > 0.f) && (Hit.Normal.ProjectOnTo(GravityDirection).Size() > KINDA_SMALL_NUMBER) && IsWalkable(Hit))
		{
			//UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
			// Another walkable ramp.
			const float InitialPercentRemaining = 1.f - PercentTimeApplied;
			RampVector = ComputeGroundMovementDelta(Delta * InitialPercentRemaining, Hit, false);
			LastMoveTimeSlice = InitialPercentRemaining * LastMoveTimeSlice;
			SafeMoveUpdatedComponent(RampVector, CharacterOwner->GetActorRotation(), true, Hit);

			const float SecondHitPercent = Hit.Time * InitialPercentRemaining;
			PercentTimeApplied = FMath::Clamp(PercentTimeApplied + SecondHitPercent, 0.f, 1.f);
		}
		if (Hit.IsValidBlockingHit())
		{
	//		UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
			if (CanStepUp(Hit) || (CharacterOwner->GetMovementBase() != NULL && CharacterOwner->GetMovementBase()->GetOwner() == Hit.GetActor()))
			{
			//UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
				// hit a barrier, try to step up
				//const FVector GravDir(0.f, 0.f, -1.f);
				const FVector GravDir = -GravityDirection;
				if (!StepUp(GravDir, Delta * (1.f - PercentTimeApplied), Hit, OutStepDownResult))
				{
					UE_LOG(LogCharacterMovement, Warning, TEXT("- StepUp (ImpactNormal %s, Normal %s"), *Hit.ImpactNormal.ToString(), *Hit.Normal.ToString());
					HandleImpact(Hit, LastMoveTimeSlice, RampVector);
					SlideAlongSurface(Delta, 1.f - PercentTimeApplied, Hit.Normal, Hit, true);
					//SlideAlongSurface(Delta, 1.f - PercentTimeApplied, Hit.Normal, Hit, false);//gdg
//:SlideAlongSurface(const FVector& Delta, float Time, const FVector& InNormal, FHitResult &Hit, bool bHandleImpact)
				}
				else
				{
			//UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
					// Don't recalculate velocity based on this height adjustment, if considering vertical adjustments.
					UE_LOG(LogCharacterMovement, Warning, TEXT("+ StepUp (ImpactNormal %s, Normal %s"), *Hit.ImpactNormal.ToString(), *Hit.Normal.ToString());
					bJustTeleported |= !bMaintainHorizontalGroundVelocity;
				}
			}
			else if ( Hit.Component.IsValid() && !Hit.Component.Get()->CanCharacterStepUp(CharacterOwner) )
			{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
				HandleImpact(Hit, LastMoveTimeSlice, RampVector);
				SlideAlongSurface(Delta, 1.f - PercentTimeApplied, Hit.Normal, Hit, true);
				//SlideAlongSurface(Delta, 1.f - PercentTimeApplied, Hit.Normal, Hit, false);//gdg
			}
		}
	}
}

FVector UOrbitCharacterMovementComponent::ComputeGroundMovementDelta(const FVector& Delta, const FHitResult& RampHit, const bool bHitFromLineTrace) const
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	const FVector FloorNormal = RampHit.ImpactNormal;
	const FVector ContactNormal = RampHit.Normal;

	//if (FloorNormal.Z < (1.f - KINDA_SMALL_NUMBER) && FloorNormal.Z > KINDA_SMALL_NUMBER && ContactNormal.Z > KINDA_SMALL_NUMBER && !bHitFromLineTrace && IsWalkable(RampHit))
	if (FloorNormal.ProjectOnTo(GravityDirection).Size() < (1.f - KINDA_SMALL_NUMBER) && 
		FloorNormal.ProjectOnTo(GravityDirection).Size() > KINDA_SMALL_NUMBER && 
		ContactNormal.ProjectOnTo(GravityDirection).Size() > KINDA_SMALL_NUMBER && 
		!bHitFromLineTrace && IsWalkable(RampHit))
	{
		// Compute a vector that moves parallel to the surface, by projecting the horizontal movement 
		// direction onto the ramp.
		const float FloorDotDelta = (FloorNormal | Delta);
		//FVector RampMovement(Delta.X, Delta.Y, -FloorDotDelta / FloorNormal.Z);
		//FVector RampMovement(Delta.X, Delta.Y, -FloorDotDelta / FloorNormal.ProjectOnTo(GravityDirection).Size() );//gdg
		FVector RampMovement = Delta;//gdg
		
		if (bMaintainHorizontalGroundVelocity)
		{
			return RampMovement;
		}
		else
		{
			return RampMovement.GetSafeNormal() * Delta.Size();
		}
	}

	return Delta;
}
float UOrbitCharacterMovementComponent::SlideAlongSurface(const FVector& Delta, float Time, const FVector& InNormal, FHitResult &Hit, bool bHandleImpact)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if (!Hit.bBlockingHit)
	{
		return 0.f;
	}

	FVector Normal(InNormal);
	if (IsMovingOnGround())
	{
		// We don't want to be pushed up an unwalkable surface.
		//if (Normal.Z > 0.f)
		if (Normal.ProjectOnTo(GravityDirection).Size() > 0.f)
		{
			if (!IsWalkable(Hit))
			{
				//Normal = Normal.GetSafeNormal2D();
				Normal = Normal.GetSafeNormal();
			}
		}
		//else if (Normal.Z < -KINDA_SMALL_NUMBER)
		else if (Normal.ProjectOnTo(GravityDirection).Size() < -KINDA_SMALL_NUMBER)
		{
			// Don't push down into the floor when the impact is on the upper portion of the capsule.
			if (CurrentFloor.FloorDist < MIN_FLOOR_DIST && CurrentFloor.bBlockingHit)
			{
				const FVector FloorNormal = CurrentFloor.HitResult.Normal;
				//const bool bFloorOpposedToMovement = (Delta | FloorNormal) < 0.f && (FloorNormal.Z < 1.f - DELTA);
				const bool bFloorOpposedToMovement = (Delta | FloorNormal) < 0.f && (FloorNormal.ProjectOnTo(GravityDirection).Size() < 1.f - DELTA);
				if (bFloorOpposedToMovement)
				{
					Normal = FloorNormal;
				}
				
				//Normal = Normal.GetSafeNormal2D();
				Normal = Normal.GetSafeNormal();
			}
		}
	}
	//return Super::SlideAlongSurface(Delta, Time, Normal, Hit, bHandleImpact);
	//transplanted this from MovementComponent.cpp
	if (!Hit.bBlockingHit)
	{
		return 0.f;
	}

	float PercentTimeApplied = 0.f;
	const FVector OldHitNormal = Normal;

	FVector SlideDelta = ComputeSlideVector(Delta, Time, Normal, Hit);

	if ((SlideDelta | Delta) > 0.f)
	{
		const FRotator Rotation = UpdatedComponent->GetComponentRotation();
		SafeMoveUpdatedComponent(SlideDelta, Rotation, true, Hit);

		const float FirstHitPercent = Hit.Time;
		PercentTimeApplied = FirstHitPercent;
		if (Hit.IsValidBlockingHit())
		{
			// Notify first impact
			if (bHandleImpact)
			{
				HandleImpact(Hit, FirstHitPercent * Time, SlideDelta);
			}

			// Compute new slide normal when hitting multiple surfaces.
			TwoWallAdjust(SlideDelta, Hit, OldHitNormal);

			// Only proceed if the new direction is of significant length.
			if (!SlideDelta.IsNearlyZero(1e-3f))
			{
				// Perform second move
				SafeMoveUpdatedComponent(SlideDelta, Rotation, true, Hit);
				const float SecondHitPercent = Hit.Time * (1.f - FirstHitPercent);
				PercentTimeApplied += SecondHitPercent;

				// Notify second impact
				if (bHandleImpact && Hit.bBlockingHit)
				{
					HandleImpact(Hit, SecondHitPercent * Time, SlideDelta);
				}
			}
		}

		return FMath::Clamp(PercentTimeApplied, 0.f, 1.f);
	}

	return 0.f;
}

void FOrbitFindFloorResult::SetFromSweep(const FHitResult& InHit, const float InSweepFloorDist, const bool bIsWalkableFloor)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	bBlockingHit = InHit.IsValidBlockingHit(); // return bBlockingHit && !bStartPenetrating;
			UE_LOG(LogTemp, Warning, TEXT("%d %s bBlockingHit:%d"), __LINE__, __FUNCTIONW__, bBlockingHit);
	bWalkableFloor = bIsWalkableFloor;
			UE_LOG(LogTemp, Warning, TEXT("%d %s bWalkableFloor:%d"), __LINE__, __FUNCTIONW__, bWalkableFloor);
	bLineTrace = false;
	FloorDist = InSweepFloorDist;
	LineDist = 0.f;
	HitResult = InHit;
}

void FOrbitFindFloorResult::SetFromLineTrace(const FHitResult& InHit, const float InSweepFloorDist, const float InLineDist, const bool bIsWalkableFloor)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	// We require a sweep that hit if we are going to use a line result.
	check(HitResult.bBlockingHit);
	if (HitResult.bBlockingHit && InHit.bBlockingHit)
	{
		// Override most of the sweep result with the line result, but save some values
		FHitResult OldHit(HitResult);
		HitResult = InHit;

		// Restore some of the old values. We want the new normals and hit actor, however.
		HitResult.Time = OldHit.Time;
		HitResult.ImpactPoint = OldHit.ImpactPoint;
		HitResult.Location = OldHit.Location;
		HitResult.TraceStart = OldHit.TraceStart;
		HitResult.TraceEnd = OldHit.TraceEnd;

		bLineTrace = true;
		LineDist = InLineDist;
		bWalkableFloor = bIsWalkableFloor;
			UE_LOG(LogTemp, Warning, TEXT("%d %s bWalkableFloor:%d"), __LINE__, __FUNCTIONW__, bWalkableFloor);
	}
}
bool UOrbitCharacterMovementComponent::MoveUpdatedComponent( const FVector& Delta, const FRotator& NewRotation, bool bSweep, FHitResult* OutHit)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if (UpdatedComponent)
	{
		const FVector NewDelta = ConstrainDirectionToPlane(Delta);
	//UE_LOG(LogTemp, Warning, TEXT("%d %s: Component:%s, class:%s"), __LINE__, __FUNCTIONW__,*UpdatedComponent->GetName(),*UpdatedComponent->GetClass()->GetName());
		bool ret = UpdatedComponent->MoveComponent(NewDelta, NewRotation, bSweep, OutHit, MoveComponentFlags);
		//UE_LOG(LogTemp, Warning, TEXT("%d %s: MoveCompReturned:%d"), __LINE__, __FUNCTIONW__,ret);
		return ret;
	}

	return false;
}

FVector UOrbitCharacterMovementComponent::ConstrainDirectionToPlane(FVector Direction) const
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);

	if (bConstrainToPlane)
	{
		Direction = FVector::VectorPlaneProject(Direction, PlaneConstraintNormal);
		UE_LOG(LogTemp, Warning, TEXT("%d %s: Direction:%s"), __LINE__, __FUNCTIONW__,*Direction.ToString());
	}

	return Direction;
}

/*
bool UOrbitCharacterMovementComponent::MoveComponent( const FVector& Delta, const FRotator& NewRotation, bool bSweep, FHitResult* OutHit, EMoveComponentFlags MoveFlags)
{
	UE_LOG(LogTemp, Warning, TEXT("%d %s: MINE"), __LINE__, __FUNCTIONW__);
//	SCOPE_CYCLE_COUNTER(STAT_MoveComponentTime);

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST) && PERF_MOVECOMPONENT_STATS
	FScopedMoveCompTimer MoveTimer(this, Delta);
#endif // !(UE_BUILD_SHIPPING || UE_BUILD_TEST) && PERF_MOVECOMPONENT_STATS

#if defined(PERF_SHOW_MOVECOMPONENT_TAKING_LONG_TIME) || LOOKING_FOR_PERF_ISSUES
	uint32 MoveCompTakingLongTime=0;
	CLOCK_CYCLES(MoveCompTakingLongTime);
#endif

	if ( Super::IsPendingKill() )
	{
		//UE_LOG(LogPrimitiveComponent, Log, TEXT("%s deleted move physics %d"),*Actor->GetName(),Actor->Physics);
		if (OutHit)
		{
			*OutHit = FHitResult();
		}
		return false;
	}

	AActor* const Actor = Super::Super::Super::Super::GetOwner();

	// static things can move before they are registered (e.g. immediately after streaming), but not after.
	if (Mobility == EComponentMobility::Static && Actor && Actor->bActorInitialized)
	//if (GetMobility() == EComponentMobility::Static && Actor && Actor->bActorInitialized)
	{
		// TODO: Static components without an owner can move, should they be able to?
		//UE_LOG(LogPrimitiveComponent, Warning, TEXT("Trying to move static component '%s' after initialization"), *GetFullName());
		if (OutHit)
		{
			*OutHit = FHitResult();
		}
		return false;
	}

	if (!bWorldToComponentUpdated)
	{
		Super::Super::Super::Super::UpdateComponentToWorld();
	}

	// Init HitResult
	FHitResult BlockingHit(1.f);
	const FVector TraceStart = GetComponentLocation();
	const FVector TraceEnd = TraceStart + Delta;
	BlockingHit.TraceStart = TraceStart;
	BlockingHit.TraceEnd = TraceEnd;

	// Set up.
	float DeltaSizeSq = Delta.SizeSquared();

	// ComponentSweepMulti does nothing if moving < KINDA_SMALL_NUMBER in distance, so it's important to not try to sweep distances smaller than that. 
	const float MinMovementDistSq = (bSweep ? FMath::Square(4.f*KINDA_SMALL_NUMBER) : 0.f);
	if (DeltaSizeSq <= MinMovementDistSq)
	{
		// Skip if no vector or rotation.
		const FQuat NewQuat = NewRotation.Quaternion();
		if( NewQuat.Equals(ComponentToWorld.GetRotation()) )
		{
			// copy to optional output param
			if (OutHit)
			{
				*OutHit = BlockingHit;
			}
			return true;
		}
		DeltaSizeSq = 0.f;
	}

	const bool bSkipPhysicsMove = ((MoveFlags & MOVECOMP_SkipPhysicsMove) != MOVECOMP_NoFlags);

	bool bMoved = false;
	TArray<FOverlapInfo> PendingOverlaps;
	TArray<FOverlapInfo> OverlapsAtEndLocation;
	TArray<FOverlapInfo>* OverlapsAtEndLocationPtr = NULL; // When non-null, used as optimization to avoid work in UpdateOverlaps.
	
	if ( !bSweep )
	{
		// not sweeping, just go directly to the new transform
		bMoved = InternalSetWorldLocationAndRotation(TraceEnd, NewRotation, bSkipPhysicsMove);
	}
	else
	{
		TArray<FHitResult> Hits;
		FVector NewLocation = TraceStart;

		// Perform movement collision checking if needed for this actor.
		const bool bCollisionEnabled = IsCollisionEnabled();
		if( bCollisionEnabled && (DeltaSizeSq > 0.f))
		{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
			if( !Super::Super::Super::Super::IsRegistered() )
			{
				if (Actor)
				{
					//UE_LOG(LogPrimitiveComponent, Fatal,TEXT("%s MovedComponent %s not initialized deleteme %d"),*Actor->GetName(), *GetName(), Actor->IsPendingKill());
				}
				else
				{
					//UE_LOG(LogPrimitiveComponent, Fatal,TEXT("MovedComponent %s not initialized"), *GetFullName());
				}
			}
#endif

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST) && PERF_MOVECOMPONENT_STATS
			MoveTimer.bDidLineCheck = true;
#endif 
			static const FName Name_MoveComponent(TEXT("MoveComponent"));

			FComponentQueryParams Params(Name_MoveComponent, Actor);
			FCollisionResponseParams ResponseParam;
			InitSweepCollisionParams(Params, ResponseParam);
			bool const bHadBlockingHit = Super::Super::Super::Super::GetWorld()->ComponentSweepMulti(Hits, this, TraceStart, TraceEnd, GetComponentRotation(), Params);

			if (Hits.Num() > 0)
			{
				const float DeltaSize = FMath::Sqrt(DeltaSizeSq);
				for(int32 HitIdx=0; HitIdx<Hits.Num(); HitIdx++)
				{
					Super::PullBackHit(Hits[HitIdx], TraceStart, TraceEnd, DeltaSize);
				}
			}

			// If we had a valid blocking hit, store it.
			// If we are looking for overlaps, store those as well.
			uint32 FirstNonInitialOverlapIdx = INDEX_NONE;
			if (bHadBlockingHit || bGenerateOverlapEvents)
			{
				int32 BlockingHitIndex = INDEX_NONE;
				float BlockingHitNormalDotDelta = BIG_NUMBER;
				for( int32 HitIdx = 0; HitIdx < Hits.Num(); HitIdx++ )
				{
					const FHitResult& TestHit = Hits[HitIdx];

					if ( !ShouldIgnoreHitResult(GetWorld(), TestHit, Delta, Actor, MoveFlags) )
					{
						if (TestHit.bBlockingHit)
						{
							if (TestHit.Time == 0.f)
							{
								// We may have multiple initial hits, and want to choose the one with the normal most opposed to our movement.
								const float NormalDotDelta = (TestHit.ImpactNormal | Delta);
								if (NormalDotDelta < BlockingHitNormalDotDelta)
								{
									BlockingHitNormalDotDelta = NormalDotDelta;
									BlockingHitIndex = HitIdx;
								}
							}
							else if (BlockingHitIndex == INDEX_NONE)
							{
								// First non-overlapping blocking hit should be used, if an overlapping hit was not.
								// This should be the only non-overlapping blocking hit, and last in the results.
								BlockingHitIndex = HitIdx;
								break;
							}							
						}
						else if (bGenerateOverlapEvents)
						{
							UPrimitiveComponent * OverlapComponent = TestHit.Component.Get();
							if (OverlapComponent && OverlapComponent->bGenerateOverlapEvents)
							{
								if (!ShouldIgnoreOverlapResult(GetWorld(), Actor, *this, TestHit.GetActor(), *OverlapComponent))
								{
									// don't process touch events after initial blocking hits
									if (BlockingHitIndex >= 0 && TestHit.Time > Hits[BlockingHitIndex].Time)
									{
										break;
									}

									if (FirstNonInitialOverlapIdx == INDEX_NONE && TestHit.Time > 0.f)
									{
										// We are about to add the first non-initial overlap.
										FirstNonInitialOverlapIdx = PendingOverlaps.Num();
									}

									// cache touches
									PendingOverlaps.AddUnique(FOverlapInfo(TestHit));
								}
							}
						}
					}
				}

				// Update blocking hit, if there was a valid one.
				if (BlockingHitIndex >= 0)
				{
					BlockingHit = Hits[BlockingHitIndex];
				}
			}
		
			// Update NewLocation based on the hit result
			if (!BlockingHit.bBlockingHit)
			{
				NewLocation = TraceEnd;
			}
			else
			{
				NewLocation = TraceStart + (BlockingHit.Time * (TraceEnd - TraceStart));

				// Sanity check
				const FVector ToNewLocation = (NewLocation - TraceStart);
				if (ToNewLocation.SizeSquared() <= MinMovementDistSq)
				{
					// We don't want really small movements to put us on or inside a surface.
					NewLocation = TraceStart;
					BlockingHit.Time = 0.f;

					// Remove any pending overlaps after this point, we are not going as far as we swept.
					if (FirstNonInitialOverlapIdx != INDEX_NONE)
					{
						PendingOverlaps.SetNum(FirstNonInitialOverlapIdx);
					}
				}
			}

			// We have performed a sweep that tested for all overlaps (not including components on the owning actor).
			// However any rotation was set at the end and not swept, so we can't assume the overlaps at the end location are correct if rotation changed.
			if (PendingOverlaps.Num() == 0 && CVarAllowCachedOverlaps->GetInt())
			{
				if (Actor && Actor->GetRootComponent() == this && AreSymmetricRotations(NewRotation.Quaternion(), ComponentToWorld.GetRotation(), ComponentToWorld.GetScale3D()))
				{
					// We know we are not overlapping any new components at the end location.
					// Keep known overlapping child components, as long as we know their overlap status could not have changed (ie they are positioned relative to us).
					if (AreAllCollideableDescendantsRelative())
					{
						GetOverlapsWithActor(Actor, OverlapsAtEndLocation);
						OverlapsAtEndLocationPtr = &OverlapsAtEndLocation;
					}
				}
			}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
			if ( (BlockingHit.Time < 1.f) && !IsZeroExtent() )
			{
				// this is sole debug purpose to find how capsule trace information was when hit 
				// to resolve stuck or improve our movement system - To turn this on, use DebugCapsuleSweepPawn
				APawn const* const ActorPawn = (Actor ? Cast<APawn>(Actor) : NULL);
				if (ActorPawn && ActorPawn->Controller && ActorPawn->Controller->IsLocalPlayerController())
				{
					APlayerController const* const PC = CastChecked<APlayerController>(ActorPawn->Controller);
					if (PC->CheatManager && PC->CheatManager->bDebugCapsuleSweepPawn)
					{
						FVector CylExtent = ActorPawn->GetSimpleCollisionCylinderExtent()*FVector(1.001f,1.001f,1.0f);							
						FCollisionShape CapsuleShape = FCollisionShape::MakeCapsule(CylExtent);
						PC->CheatManager->AddCapsuleSweepDebugInfo(TraceStart, TraceEnd, BlockingHit.ImpactPoint, BlockingHit.Normal, BlockingHit.ImpactNormal, BlockingHit.Location, CapsuleShape.GetCapsuleHalfHeight(), CapsuleShape.GetCapsuleRadius(), true, (BlockingHit.bStartPenetrating && BlockingHit.bBlockingHit) ? true: false);
					}
				}
			}
#endif
		}
		else if (DeltaSizeSq > 0.f)
		{
			// apply move delta even if components has collisions disabled
			NewLocation += Delta;
		}
		else if (DeltaSizeSq == 0.f && bCollisionEnabled)
		{
			// We didn't move, and any rotation that doesn't change our overlap bounds means we already know what we are overlapping at this point.
			// Can only do this if current known overlaps are valid (not deferring updates)
			if (CVarAllowCachedOverlaps->GetInt() && Actor && Actor->GetRootComponent() == this && !IsDeferringMovementUpdates())
			{
				// Only if we know that we won't change our overlap status with children by moving.
				if (AreSymmetricRotations(NewRotation.Quaternion(), ComponentToWorld.GetRotation(), ComponentToWorld.GetScale3D()) &&
					AreAllCollideableDescendantsRelative())
				{
					OverlapsAtEndLocation = OverlappingComponents;
					OverlapsAtEndLocationPtr = &OverlapsAtEndLocation;
				}
			}
		}

		// Update the location.  This will teleport any child components as well (not sweep).
		bMoved = InternalSetWorldLocationAndRotation(NewLocation, NewRotation, bSkipPhysicsMove);
	}

	// Handle overlap notifications.
	if (bMoved)
	{
		// Check if we are deferring the movement updates.
		if (IsDeferringMovementUpdates())
		{
			// Defer UpdateOverlaps until the scoped move ends.
			FScopedMovementUpdate* ScopedUpdate = GetCurrentScopedMovement();
			checkSlow(ScopedUpdate != NULL);
			ScopedUpdate->AppendOverlaps(PendingOverlaps, OverlapsAtEndLocationPtr);
		}
		else
		{
			// still need to do this even if bGenerateOverlapEvents is false for this component, since we could have child components where it is true
			UpdateOverlaps(&PendingOverlaps, true, OverlapsAtEndLocationPtr);
		}
	}

	// Handle blocking hit notifications.
	if (BlockingHit.bBlockingHit)
	{
		if (IsDeferringMovementUpdates())
		{
			FScopedMovementUpdate* ScopedUpdate = GetCurrentScopedMovement();
			checkSlow(ScopedUpdate != NULL);
			ScopedUpdate->AppendBlockingHit(BlockingHit);
		}
		else
		{
			DispatchBlockingHit(*Actor, BlockingHit);
		}
	}

#if defined(PERF_SHOW_MOVECOMPONENT_TAKING_LONG_TIME) || LOOKING_FOR_PERF_ISSUES
	UNCLOCK_CYCLES(MoveCompTakingLongTime);
	const float MSec = FPlatformTime::ToMilliseconds(MoveCompTakingLongTime);
	if( MSec > PERF_SHOW_MOVECOMPONENT_TAKING_LONG_TIME_AMOUNT )
	{
		if (Actor)
		{
			//UE_LOG(LogPrimitiveComponent, Log, TEXT("%10f executing MoveComponent for %s owned by %s"), MSec, *GetName(), *Actor->GetFullName() );
		}
		else
		{
			//UE_LOG(LogPrimitiveComponent, Log, TEXT("%10f executing MoveComponent for %s"), MSec, *GetFullName() );
		}
	}
#endif

	// copy to optional output param
	if (OutHit)
	{
		*OutHit = BlockingHit;
	}

	// Return whether we moved at all.
	return bMoved;
}*/
FVector UOrbitCharacterMovementComponent::GetImpartedMovementBaseVelocity() const
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	FVector Result = FVector::ZeroVector;
	if (CharacterOwner)
	{
		UPrimitiveComponent* MovementBase = CharacterOwner->GetMovementBase();
		if (MovementBaseUtility::IsDynamicBase(MovementBase))
		{
			FVector BaseVelocity = MovementBaseUtility::GetMovementBaseVelocity(MovementBase, CharacterOwner->GetBasedMovement().BoneName);
			
			if (bImpartBaseAngularVelocity)
			{
				/*const FVector CharacterBasePosition = 
					(UpdatedComponent->GetComponentLocation() - 
					FVector(0.f, 0.f, CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()));
				*/
				const FVector CharacterBasePosition = 
					(UpdatedComponent->GetComponentLocation() - 
					GravityDirection * CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
				const FVector BaseTangentialVel = MovementBaseUtility::GetMovementBaseTangentialVelocity(MovementBase, CharacterOwner->GetBasedMovement().BoneName, CharacterBasePosition);
				BaseVelocity += BaseTangentialVel;
			}

			if (bImpartBaseVelocityX)
			{
				Result.X = BaseVelocity.X;
			}
			if (bImpartBaseVelocityY)
			{
				Result.Y = BaseVelocity.Y;
			}
			if (bImpartBaseVelocityZ)
			{
				Result.Z = BaseVelocity.Z;
			}
		}
	}
	
	return Result;
}
void UOrbitCharacterMovementComponent::UpdateBasedRotation(FRotator &FinalRotation, const FRotator& ReducedRotation)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	AController* Controller = CharacterOwner ? CharacterOwner->Controller : NULL;
	float ControllerRoll = 0.f;
	if( Controller && !bIgnoreBaseRotation )
	{
		FRotator const ControllerRot = Controller->GetControlRotation();
		ControllerRoll = ControllerRot.Roll;
		Controller->SetControlRotation(ControllerRot + ReducedRotation);
	}

	// Remove roll
//	FinalRotation.Roll = 0.f;
	if( Controller )
	{
		FinalRotation.Roll = CharacterOwner->GetActorRotation().Roll;
		FRotator NewRotation = Controller->GetControlRotation();
		NewRotation.Roll = ControllerRoll;
		Controller->SetControlRotation(NewRotation);
	}
}
void UOrbitCharacterMovementComponent::UpdateBasedMovement(float DeltaSeconds)
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	if (!HasValidData())
	{
		return;
	}

	const UPrimitiveComponent* MovementBase = CharacterOwner->GetMovementBase();
	if (!MovementBaseUtility::UseRelativeLocation(MovementBase))
	{
		return;
	}

	if (!IsValid(MovementBase) || !IsValid(MovementBase->GetOwner()))
	{
		SetBase(NULL);
		return;
	}

	// Ignore collision with bases during these movements.
	TGuardValue<EMoveComponentFlags> ScopedFlagRestore(MoveComponentFlags, MoveComponentFlags | MOVECOMP_IgnoreBases);

	FQuat DeltaQuat = FQuat::Identity;
	FVector DeltaPosition = FVector::ZeroVector;

	FQuat NewBaseQuat;
	FVector NewBaseLocation;
	if (!MovementBaseUtility::GetMovementBaseTransform(MovementBase, CharacterOwner->GetBasedMovement().BoneName, NewBaseLocation, NewBaseQuat))
	{
		return;
	}

	// Find change in rotation
	const bool bRotationChanged = !OldBaseQuat.Equals(NewBaseQuat);
	if (bRotationChanged)
	{
		DeltaQuat = NewBaseQuat * OldBaseQuat.Inverse();
	}

	// only if base moved
	if (bRotationChanged || (OldBaseLocation != NewBaseLocation))
	{
		// Calculate new transform matrix of base actor (ignoring scale).
		const FQuatRotationTranslationMatrix OldLocalToWorld(OldBaseQuat, OldBaseLocation);
		const FQuatRotationTranslationMatrix NewLocalToWorld(NewBaseQuat, NewBaseLocation);

		if( CharacterOwner->IsMatineeControlled() )
		{
			FRotationTranslationMatrix HardRelMatrix(CharacterOwner->GetBasedMovement().Rotation, CharacterOwner->GetBasedMovement().Location);
			const FMatrix NewWorldTM = HardRelMatrix * NewLocalToWorld;
			const FRotator NewWorldRot = bIgnoreBaseRotation ? CharacterOwner->GetActorRotation() : NewWorldTM.Rotator();
				FHitResult MoveOnBaseHit(1.f);
			MoveUpdatedComponent( NewWorldTM.GetOrigin() - CharacterOwner->GetActorLocation(), NewWorldRot, true, &MoveOnBaseHit);
		}
		else
		{
			FQuat FinalQuat = CharacterOwner->GetActorQuat();
			
			if (bRotationChanged && !bIgnoreBaseRotation)
			{
				// Apply change in rotation and pipe through FaceRotation to maintain axis restrictions
				const FQuat PawnOldQuat = CharacterOwner->GetActorQuat();
				FinalQuat = DeltaQuat * FinalQuat;
				CharacterOwner->FaceRotation(FinalQuat.Rotator(), 0.f);
				FinalQuat = CharacterOwner->GetActorQuat();

				// Pipe through ControlRotation, to affect camera.
				if (CharacterOwner->Controller)
				{
					const FQuat PawnDeltaRotation = FinalQuat * PawnOldQuat.Inverse();
					FRotator FinalRotation = FinalQuat.Rotator();
					UpdateBasedRotation(FinalRotation, PawnDeltaRotation.Rotator());
					FinalQuat = FinalRotation.Quaternion();
				}
			}

			// We need to offset the base of the character here, not its origin, so offset by half height
			float HalfHeight, Radius;
			CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(Radius, HalfHeight);

			//FVector const BaseOffset(0.0f, 0.0f, HalfHeight);
			FVector const BaseOffset = GravityDirection * HalfHeight;
			FVector const LocalBasePos = OldLocalToWorld.InverseTransformPosition(CharacterOwner->GetActorLocation() - BaseOffset);
			FVector const NewWorldPos = ConstrainLocationToPlane(NewLocalToWorld.TransformPosition(LocalBasePos) + BaseOffset);
			DeltaPosition = ConstrainDirectionToPlane(NewWorldPos - CharacterOwner->GetActorLocation());

			// move attached actor
			if (bFastAttachedMove)
			{
				// we're trusting no other obstacle can prevent the move here
				UpdatedComponent->SetWorldLocationAndRotation(NewWorldPos, FinalQuat.Rotator(), false);
			}
			else
			{
				FHitResult MoveOnBaseHit(1.f);
				const FVector OldLocation = UpdatedComponent->GetComponentLocation();
				MoveUpdatedComponent(DeltaPosition, FinalQuat.Rotator(), true, &MoveOnBaseHit);
				if ((UpdatedComponent->GetComponentLocation() - (OldLocation + DeltaPosition)).IsNearlyZero() == false)
				{
					OnUnableToFollowBaseMove(DeltaPosition, OldLocation, MoveOnBaseHit);
				}
			}
		}

		if (MovementBase->IsSimulatingPhysics() && CharacterOwner->GetMesh())
		{
			CharacterOwner->GetMesh()->ApplyDeltaToAllPhysicsTransforms(DeltaPosition, DeltaQuat);
		}
	}
}
void UOrbitCharacterMovementComponent::TwoWallAdjust(FVector &Delta, const FHitResult& Hit, const FVector &OldHitNormal) const
{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);
	FVector InDelta = Delta;
	Super::TwoWallAdjust(Delta, Hit, OldHitNormal);

	if (IsMovingOnGround())
	{
		// Allow slides up walkable surfaces, but not unwalkable ones (treat those as vertical barriers).
		//if (Delta.Z > 0.f)
		if (Delta.ProjectOnTo(GravityDirection).Size() > 0.f)
		{
			//if ((Hit.Normal.Z >= WalkableFloorZ || IsWalkable(Hit)) && Hit.Normal.Z > KINDA_SMALL_NUMBER)
			if ((Hit.Normal.ProjectOnTo(GravityDirection).Size() >= GetWalkableFloorZ() || IsWalkable(Hit)) && Hit.Normal.Z > KINDA_SMALL_NUMBER)
			{
				// Maintain horizontal velocity
				const float Time = (1.f - Hit.Time);
				const FVector ScaledDelta = Delta.GetSafeNormal() * InDelta.Size();
				//Delta = FVector(InDelta.X, InDelta.Y, ScaledDelta.Z / Hit.Normal.Z) * Time;
				Delta = InDelta.ProjectOnTo(GravityDirection) / Hit.Normal.ProjectOnTo(GravityDirection) * Time;
			}
			else
			{
				//Delta.Z = 0.f;
				Delta = Delta - Delta.ProjectOnTo(GravityDirection);
			}
		}
		//else if (Delta.Z < 0.f)
		//is this right? Can size be < 0?
		else if (Delta.ProjectOnTo(GravityDirection).Size() < 0.f)
		{
			// Don't push down into the floor.
			if (CurrentFloor.FloorDist < MIN_FLOOR_DIST && CurrentFloor.bBlockingHit)
			{
				//Delta.Z = 0.f;
				Delta = Delta - Delta.ProjectOnTo(GravityDirection);
			}
		}
	}
}
