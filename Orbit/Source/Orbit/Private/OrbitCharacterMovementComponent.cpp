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
	Super::InitializeComponent();
	CalculateGravity();
}
void UOrbitCharacterMovementComponent::TickComponent( float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction )
{
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
				}
			}
		}

		ApplyRepulsionForce(DeltaTime);
	}





	//end Paste

	CalculateGravity();//FIXME: probably won't have to do the complete calculation every tick
	FRotator GravRot = GravityDirection.Rotation();
	GravRot.Pitch += 120;//not sure why this correction is needed 
		checkf(!GravRot.ContainsNaN(), TEXT("Tick: GravRot contains NaN"));
	//GetOwner()->SetActorRotation(GravRot * FMath::DegreesToRadians( YawSum) );//interesting

	GetOwner()->SetActorRotation(GravRot  );
	GetOwner()->AddActorLocalRotation(FRotator(0, YawSum, 0), true);

	checkf(!GetOwner()->GetActorRotation().ContainsNaN(), TEXT("Tick: Actor Rotation contains NaN "));
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
		//AddForce(GravityVector);
		AddForce(GravityVector);
		//GetActorFeetLocation();
		//AddRadialForce(GetActorFeetLocation(), 50.f, 50.f, ERadialImpulseFalloff::RIF_Constant);
		//UPrimitiveComponent* MutableThis = const_cast<UPrimitiveComponent*>( Get );
		//MutableThis
		
}

float UOrbitCharacterMovementComponent::GetGravityZ() const
{
	return GravityMagnitude;
}
void UOrbitCharacterMovementComponent::OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode){
//	Super::OnMovementModeChanged(PreviousMovementMode, PreviousCustomMode);
	
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
	
	PendingImpulseToApply = PendingImpulseToApply.VectorPlaneProject(PendingImpulseToApply, GravityDirection);//gdg
	PendingForceToApply = PendingForceToApply.VectorPlaneProject(PendingForceToApply, GravityDirection);//gdg
	if (PendingImpulseToApply.Size() != 0.f || PendingForceToApply.Size() != 0.f)//gdg
	{
		if (IsMovingOnGround() && ( PendingImpulseToApply  + (PendingForceToApply * DeltaSeconds)).Size() - (GravityVector * DeltaSeconds).Size() >SMALL_NUMBER )
		{
			SetMovementMode(MOVE_Falling,0);
		}
	}

	Velocity += PendingImpulseToApply + (PendingForceToApply * DeltaSeconds);

	PendingImpulseToApply = FVector::ZeroVector;
	PendingForceToApply = FVector::ZeroVector;
}

//probably doing something wrong that I need this to prevent errors about missing 2nd param
void UOrbitCharacterMovementComponent::SetMovementMode(EMovementMode NewMovementMode)
{
	Super::SetMovementMode(NewMovementMode, 0);
}

void UOrbitCharacterMovementComponent::SetMovementMode(EMovementMode NewMovementMode, uint8 NewCustomMode)
{
	Super::SetMovementMode(NewMovementMode, NewCustomMode);
}

void UOrbitCharacterMovementComponent::PhysCustom(float deltaTime, int32 Iterations)
{
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
	return MovementMode == MOVE_Custom && CustomMovementMode == CUSTOM_MoonWalking;//fart
}


void UOrbitCharacterMovementComponent::PhysWalking(float deltaTime, int32 Iterations)
{
//	SetMovementMode(MOVE_Custom, CUSTOM_MoonWalking);//fart
	Super::PhysWalking(deltaTime, Iterations);
	//all of the above is probably going away
	//end
}
void UOrbitCharacterMovementComponent::PhysMoonWalking(float deltaTime, int32 Iterations)
{

	if (deltaTime < MIN_TICK_TIME)
	{
		return;
	}

	if( (!CharacterOwner || !CharacterOwner->Controller) && !bRunPhysicsWithNoController && !HasRootMotion() )
	{
		Acceleration = FVector::ZeroVector;
		Velocity = FVector::ZeroVector;
		return;
	}

	if (!UpdatedComponent->IsCollisionEnabled())
	{
		SetMovementMode(MOVE_Custom, CUSTOM_MoonWalking);//fart
		return;
	}

	checkf(!Velocity.ContainsNaN(), TEXT("PhysMoonWalking: Velocity contains NaN before Iteration (%s: %s)\n%s"), *GetPathNameSafe(this), *GetPathNameSafe(GetOuter()), *Velocity.ToString());
	
	bJustTeleported = false;
	bool bCheckedFall = false;
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
		//RemoveVertical(Velocity);
		const FVector OldVelocity = Velocity;

		// Apply acceleration
		//Acceleration.Z = 0.f;
		//RemoveVertical(Acceleration);
		//gdg
		if( !HasRootMotion() )
		{
			CalcVelocity(timeTick, GroundFriction, false, BrakingDecelerationWalking);
		}
		checkf(!Velocity.ContainsNaN(), TEXT("PhysMoonWalking: Velocity contains NaN after CalcVelocity (%s: %s)\n%s"), *GetPathNameSafe(this), *GetPathNameSafe(GetOuter()), *Velocity.ToString());

		// Compute move parameters
		const FVector MoveVelocity = Velocity;
		const FVector Delta = timeTick * MoveVelocity;
		const bool bZeroDelta = Delta.IsNearlyZero();
		FStepDownResult StepDownResult;

		if ( bZeroDelta )
		{
			remainingTime = 0.f;
		}
		else
		{
			// try to move forward
			MoveAlongFloor(MoveVelocity, timeTick, &StepDownResult); // <-- hop/skate must differ in here

			if ( IsFalling() )
			{
				// pawn decided to jump up
				const float DesiredDist = Delta.Size();
				if (DesiredDist > KINDA_SMALL_NUMBER)
				{
					//@todo gdg
					const float ActualDist = (CharacterOwner->GetActorLocation() - OldLocation).Size2D();
					remainingTime += timeTick * (1.f - FMath::Min(1.f,ActualDist/DesiredDist));
				}
				StartNewPhysics(remainingTime, Iterations);
				return;
			}
			else if ( IsSwimming() ) //just entered water
			{
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
			CurrentFloor = StepDownResult.FloorResult;
		}
		else
		{
			
			//can walk w/o crow hopping to south if this is commented out!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
			//still have black holes at the poles.
			FindFloor(UpdatedComponent->GetComponentLocation(), CurrentFloor, bZeroDelta, NULL);
		}

		// check for ledges here
		const bool bCheckLedges = !CanWalkOffLedges();
		if ( bCheckLedges && !CurrentFloor.IsWalkableFloor() )
		{
			// calculate possible alternate movement
			//const FVector GravDir = FVector(0.f,0.f,-1.f);
			const FVector GravDir = GravityDirection;//gdg
			const FVector NewDelta = bTriedLedgeMove ? FVector::ZeroVector : GetLedgeMove(OldLocation, Delta, GravDir);
			if ( !NewDelta.IsZero() )
			{
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
				// see if it is OK to jump
				// @todo collision : only thing that can be problem is that oldbase has world collision on
				bool bMustJump = bZeroDelta || (OldBase == NULL || (!OldBase->IsCollisionEnabled() && MovementBaseUtility::IsDynamicBase(OldBase)));
				if ( (bMustJump || !bCheckedFall) && CheckFall(CurrentFloor.HitResult, Delta, OldLocation, remainingTime, timeTick, Iterations, bMustJump) )
				{
					return;
				}
				bCheckedFall = true;

				// revert this move
				RevertMove(OldLocation, OldBase, PreviousBaseLocation, OldFloor, true);
				remainingTime = 0.f;
				break;
			}

		}
		else
		{
			// Validate the floor check
			if (CurrentFloor.IsWalkableFloor())
			{
				if (ShouldCatchAir(OldFloor, CurrentFloor))
				{
					CharacterOwner->OnWalkingOffLedge();
					if (IsMovingOnGround())
					{
						// If still walking, then fall. If not, assume the user set a different mode they want to keep.
						StartFalling(Iterations, remainingTime, timeTick, Delta, OldLocation);//not problem
					}
					return;
				}

				AdjustFloorHeight();
				SetBase(CurrentFloor.HitResult.Component.Get(), CurrentFloor.HitResult.BoneName);
			}
			else if (CurrentFloor.HitResult.bStartPenetrating && remainingTime <= 0.f)
			{
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
				StartSwimming(OldLocation, Velocity, timeTick, remainingTime, Iterations);
				return;
			}
			// See if we need to start falling.
			if (!CurrentFloor.IsWalkableFloor() && !CurrentFloor.HitResult.bStartPenetrating)
			{
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__);//hopping sometimes
				const bool bMustJump = bJustTeleported || bZeroDelta || (OldBase == NULL || 
					(!OldBase->IsCollisionEnabled() && MovementBaseUtility::IsDynamicBase(OldBase)));
				if (
					(bMustJump || !bCheckedFall) 
					&& CheckFall(CurrentFloor.HitResult, Delta, OldLocation, remainingTime, timeTick, Iterations, bMustJump) 
					) {
			UE_LOG(LogTemp, Warning, TEXT("%d %s:"), __LINE__, __FUNCTIONW__); //hopping sometimes
					return;
				}
				bCheckedFall = true;
			}
		}

		// Allow overlap events and such to change physics state and velocity
		if (IsMovingOnGround())
		{
			// Make velocity reflect actual move
			if( !bJustTeleported && !HasRootMotion() && timeTick >= MIN_TICK_TIME)
			{
				Velocity = (CharacterOwner->GetActorLocation() - OldLocation) / timeTick;
			}
		}

		// If we didn't move at all this iteration then abort (since future iterations will also be stuck).
		if (CharacterOwner->GetActorLocation() == OldLocation)
		{
			remainingTime = 0.f;
			break;
		}	
	}

	if (IsMovingOnGround())
	{
		MaintainHorizontalGroundVelocity();
	}
}
//things that sweep
//ComputeFloorDist
void UOrbitCharacterMovementComponent::AdjustFloorHeight()
{

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
		UE_LOG(LogTemp, Warning, TEXT("Adjust floor height %.3f (Hit = %d)"), MoveDist, AdjustHit.bBlockingHit);

		if (!AdjustHit.IsValidBlockingHit())
		{
			CurrentFloor.FloorDist += MoveDist;
			//CurrentFloor.FloorDist -= MoveDist;
		}
		else if (MoveDist > 0.f)
		{
			//const float CurrentZ = UpdatedComponent->GetComponentLocation().Z;
			const FVector Current = UpdatedComponent->GetComponentLocation();//gdg
			//CurrentFloor.FloorDist += CurrentZ - InitialZ;
			CurrentFloor.FloorDist += FVector::Dist(Initial, Current );//gdg
		}
		else
		{
			checkSlow(MoveDist < 0.f);
			//const float CurrentZ = UpdatedComponent->GetComponentLocation().Z;
			const FVector Current = UpdatedComponent->GetComponentLocation();//gdg
			//CurrentFloor.FloorDist = CurrentZ - AdjustHit.Location.Z;
			CurrentFloor.FloorDist = FVector::Dist(AdjustHit.Location, Current );//gdg
			

			if (IsWalkable(AdjustHit))
			{
			CurrentFloor.SetFromSweep(AdjustHit, CurrentFloor.FloorDist, true);
			}
		}

		// Don't recalculate velocity based on this height adjustment, if considering vertical adjustments.
		// Also avoid it if we moved out of penetration
		bJustTeleported |= !bMaintainHorizontalGroundVelocity || (OldFloorDist < 0.f);

	}
}




















bool UOrbitCharacterMovementComponent::IsWalkable(const FHitResult& Hit) const
{
	if (!Hit.IsValidBlockingHit())
	{
		// No hit, or starting in penetration
		return false;
	}

	// Never walk up vertical surfaces. bah humbug
	//if (Hit.ImpactNormal.Z < KINDA_SMALL_NUMBER)
	if (FVector::DotProduct(Hit.ImpactNormal, -GravityVector) < KINDA_SMALL_NUMBER)//gdg
	{
		return false;
	}
	
	float TestWalkableZ = GetWalkableFloorZ();
	//FVector TestWalkable = -GravityDirection;//gdg

	// See if this component overrides the walkable floor z.
	const UPrimitiveComponent* HitComponent = Hit.Component.Get();
	

if (HitComponent)
	{
		const FWalkableSlopeOverride& SlopeOverride = HitComponent->GetWalkableSlopeOverride();
		TestWalkableZ = SlopeOverride.ModifyWalkableFloorZ(TestWalkableZ);
	}

	// Can't walk on this surface if it is too steep.
	//if (Hit.ImpactNormal.Z < TestWalkableZ)
	if ( FMath::Abs(FVector::DotProduct(Hit.ImpactNormal, GravityDirection)) < TestWalkableZ )
	{
			UE_LOG(LogTemp, Warning, TEXT("%d IsWalkable NOT %s"), __LINE__, *Hit.ImpactNormal.ToString() );
		return false;
	}

	return true;
}


void UOrbitCharacterMovementComponent::FindFloor(const FVector& CapsuleLocation, FFindFloorResult& OutFloorResult, bool bZeroDelta, const FHitResult* DownwardSweepResult) const
{
	// No collision, no floor...
	if (!UpdatedComponent->IsCollisionEnabled())
	{
		OutFloorResult.Clear();
		return;
	}

	// Increase height check slightly if walking, to prevent floor height adjustment from later invalidating the floor result.
	const float HeightCheckAdjust = (IsMovingOnGround() ? MAX_FLOOR_DIST + KINDA_SMALL_NUMBER : -MAX_FLOOR_DIST );

	float FloorSweepTraceDist = FMath::Max(MAX_FLOOR_DIST, MaxStepHeight + HeightCheckAdjust);
	float FloorLineTraceDist = FloorSweepTraceDist;
	bool bNeedToValidateFloor = true;
	
	// Sweep floor
	if (FloorLineTraceDist > 0.f || FloorSweepTraceDist > 0.f)
	{
		UCharacterMovementComponent* MutableThis = const_cast<UOrbitCharacterMovementComponent*>(this);

		if ( bAlwaysCheckFloor || !bZeroDelta || bForceNextFloorCheck || bJustTeleported )
		{
			//Normally end up here
			MutableThis->bForceNextFloorCheck = false;
			//get here a lot
			ComputeFloorDist(CapsuleLocation, FloorLineTraceDist, FloorSweepTraceDist, OutFloorResult, CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius(), DownwardSweepResult);
		}
		else
		{
			// Force floor check if base has collision disabled or if it does not block us.
			UPrimitiveComponent* MovementBase = CharacterOwner->GetMovementBase();
			const AActor* BaseActor = MovementBase ? MovementBase->GetOwner() : NULL;
			const ECollisionChannel CollisionChannel = UpdatedComponent->GetCollisionObjectType();

			if (MovementBase != NULL)
			{
				MutableThis->bForceNextFloorCheck = !MovementBase->IsCollisionEnabled()
				|| MovementBase->GetCollisionResponseToChannel(CollisionChannel) != ECR_Block
				|| (MovementBase->Mobility == EComponentMobility::Movable)
				|| MovementBaseUtility::IsDynamicBase(MovementBase)
				|| (Cast<const ADestructibleActor>(BaseActor) != NULL);
			}

			const bool IsActorBasePendingKill = BaseActor && BaseActor->IsPendingKill();

			if ( !bForceNextFloorCheck && !IsActorBasePendingKill && MovementBase )
			{
				OutFloorResult = CurrentFloor;
				bNeedToValidateFloor = false;
			}
			else
			{
				MutableThis->bForceNextFloorCheck = false;
				ComputeFloorDist(CapsuleLocation, FloorLineTraceDist, FloorSweepTraceDist, OutFloorResult, CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius(), DownwardSweepResult);
			}
		}
	}
		//The OutFloorResult.bBlockingHit goes from 1 to 0 when fall off and none of the code below executes

	// OutFloorResult.HitResult is now the result of the vertical floor check.
	// See if we should try to "perch" at this location.
	if (bNeedToValidateFloor && OutFloorResult.bBlockingHit && !OutFloorResult.bLineTrace)
	{
		//Normally end up here
		const bool bCheckRadius = true;
		if (ShouldComputePerchResult(OutFloorResult.HitResult, bCheckRadius))
		{
			float MaxPerchFloorDist = FMath::Max(MAX_FLOOR_DIST, MaxStepHeight + HeightCheckAdjust);
			if (IsMovingOnGround())
			{
				MaxPerchFloorDist += FMath::Max(0.f, PerchAdditionalHeight);
			}

			FFindFloorResult PerchFloorResult;
			if (ComputePerchResult(GetValidPerchRadius(), OutFloorResult.HitResult, MaxPerchFloorDist, PerchFloorResult))
			{
				// Don't allow the floor distance adjustment to push us up too high, or we will move beyond the perch distance and fall next time.
				const float AvgFloorDist = (MIN_FLOOR_DIST + MAX_FLOOR_DIST) * 0.5f;
				const float MoveUpDist = (AvgFloorDist - OutFloorResult.FloorDist);
				if (MoveUpDist + PerchFloorResult.FloorDist >= MaxPerchFloorDist)
				{
					OutFloorResult.FloorDist = AvgFloorDist;
				}

				// If the regular capsule is on an unwalkable surface but the perched one would allow us to stand, override the normal to be one that is walkable.
				if (!OutFloorResult.bWalkableFloor)
				{
					OutFloorResult.SetFromLineTrace(PerchFloorResult.HitResult, OutFloorResult.FloorDist, FMath::Min(PerchFloorResult.FloorDist, PerchFloorResult.LineDist), true);
				}
			}
			else
			{
				// We had no floor (or an invalid one because it was unwalkable), and couldn't perch here, so invalidate floor (which will cause us to start falling).
				OutFloorResult.bWalkableFloor = false;
			}
		} 
	}
}

void UOrbitCharacterMovementComponent::ComputeFloorDist(const FVector& CapsuleLocation, float LineDistance, 
	float SweepDistance, FFindFloorResult& OutFloorResult, float SweepRadius, const FHitResult* DownwardSweepResult) const
{
	OutFloorResult.Clear();

	// No collision, no floor...
	if (!UpdatedComponent->IsCollisionEnabled())
	{
		return;
	}

	float PawnRadius, PawnHalfHeight;
	CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(PawnRadius, PawnHalfHeight);

	bool bSkipSweep = false;
	if (DownwardSweepResult != NULL && DownwardSweepResult->IsValidBlockingHit())
	{
		// Only if the supplied sweep was vertical and downward.
		/*
		if ((DownwardSweepResult->TraceStart.Z > DownwardSweepResult->TraceEnd.Z) &&
			(DownwardSweepResult->TraceStart - DownwardSweepResult->TraceEnd).SizeSquared2D() <= KINDA_SMALL_NUMBER)
			*/
		if (( FVector::DotProduct(DownwardSweepResult->TraceStart, GravityDirection) > FVector::DotProduct(DownwardSweepResult->TraceEnd,GravityDirection)) &&
			(DownwardSweepResult->TraceStart - DownwardSweepResult->TraceEnd).SizeSquared() <= KINDA_SMALL_NUMBER)
		{
			// Reject hits that are barely on the cusp of the radius of the capsule
			if (IsWithinEdgeTolerance(DownwardSweepResult->Location, DownwardSweepResult->ImpactPoint, PawnRadius))
			{
				// Don't try a redundant sweep, regardless of whether this sweep is usable.
				bSkipSweep = true;

				const bool bIsWalkable = IsWalkable(*DownwardSweepResult);
				//const float FloorDist = (CapsuleLocation.Z - DownwardSweepResult->Location.Z);
				const float FloorDist = FVector::Dist(CapsuleLocation, DownwardSweepResult->Location);
				OutFloorResult.SetFromSweep(*DownwardSweepResult, FloorDist, bIsWalkable);
				
				if (bIsWalkable)
				{
					// Use the supplied downward sweep as the floor hit result.			
					return;
				}
			}
		}
	}

	// We require the sweep distance to be >= the line distance, otherwise the HitResult can't be interpreted as the sweep result.
	if (SweepDistance < LineDistance)
	{
		check(SweepDistance >= LineDistance);
		return;
	}

	bool bBlockingHit = false;
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
		//FCollisionShape CapsuleShape = FCollisionShape::MakeSphere(SweepRadius);//variations cause drippling/sliding

		FHitResult Hit(1.f);
		//bBlockingHit = FloorSweepTest(Hit, CapsuleLocation, CapsuleLocation + FVector(0.f,0.f,-TraceDist), CollisionChannel, CapsuleShape, QueryParams, ResponseParam);
			//the 2.0* seems to eliminate crow hopping, still flying off at equator though
			//FVector SecondPoint = CapsuleLocation + (2.0*TraceDist*GravityDirection);
			FVector SecondPoint = CapsuleLocation + (TraceDist*GravityDirection);
				bBlockingHit = FloorSweepTest(Hit, CapsuleLocation, SecondPoint, CollisionChannel, CapsuleShape, QueryParams, ResponseParam);

		if (bBlockingHit)
		{
			// Reject hits adjacent to us, we only care about hits on the bottom portion of our capsule.
			// Check 2D distance to impact point, reject if within a tolerance from radius.
			if (Hit.bStartPenetrating || !IsWithinEdgeTolerance(CapsuleLocation, Hit.ImpactPoint, CapsuleShape.Capsule.Radius))
			{
				// Use a capsule with a slightly smaller radius and shorter height to avoid the adjacent object.
				ShrinkHeight = (PawnHalfHeight - PawnRadius) * (1.f - ShrinkScaleOverlap);
				TraceDist = SweepDistance + ShrinkHeight;
				CapsuleShape.Capsule.Radius = FMath::Max(0.f, CapsuleShape.Capsule.Radius - SWEEP_EDGE_REJECT_DISTANCE - KINDA_SMALL_NUMBER);
				CapsuleShape.Capsule.HalfHeight = FMath::Max(PawnHalfHeight - ShrinkHeight, CapsuleShape.Capsule.Radius);
				Hit.Reset(1.f, false);

				//bBlockingHit = FloorSweepTest(Hit, CapsuleLocation, CapsuleLocation + FVector(0.f,0.f,-TraceDist), CollisionChannel, CapsuleShape, QueryParams, ResponseParam);
				bBlockingHit = FloorSweepTest(Hit, CapsuleLocation, CapsuleLocation + (GravityDirection * TraceDist), CollisionChannel, CapsuleShape, QueryParams, ResponseParam);
			}

			// Reduce hit distance by ShrinkHeight because we shrank the capsule for the trace.
			// We allow negative distances here, because this allows us to pull out of penetrations.
			const float MaxPenetrationAdjust = FMath::Max(MAX_FLOOR_DIST, PawnRadius);
			const float SweepResult = FMath::Max(-MaxPenetrationAdjust, Hit.Time * TraceDist - ShrinkHeight);

			OutFloorResult.SetFromSweep(Hit, SweepResult, false);
			if (Hit.IsValidBlockingHit() && IsWalkable(Hit))
			{		
				if (SweepResult <= SweepDistance)
				{
					// Hit within test distance.
					OutFloorResult.bWalkableFloor = true;
					return;
				}
			}
		}
	}

	// Since we require a longer sweep than line trace, we don't want to run the line trace if the sweep missed everything.
	// We do however want to try a line trace if the sweep was stuck in penetration.
	if (!OutFloorResult.bBlockingHit && !OutFloorResult.HitResult.bStartPenetrating)
	{
		OutFloorResult.FloorDist = SweepDistance;
		return;
	}

	// Line trace
	if (LineDistance > 0.f)
	{
		const float ShrinkHeight = PawnHalfHeight;
		const FVector LineTraceStart = CapsuleLocation;	
		const float TraceDist = LineDistance + ShrinkHeight;
		//const FVector Down = FVector(0.f, 0.f, -TraceDist);
		//const FVector Down = GravityDirection * -TraceDist;
		const FVector Down = GravityDirection * TraceDist;

		static const FName FloorLineTraceName = FName(TEXT("ComputeFloorDistLineTrace"));
		QueryParams.TraceTag = FloorLineTraceName;

		FHitResult Hit(1.f);
		bBlockingHit = GetWorld()->LineTraceSingle(Hit, LineTraceStart, LineTraceStart + Down, CollisionChannel, QueryParams, ResponseParam);

		if (bBlockingHit)
		{
			if (Hit.Time > 0.f)
			{
				// Reduce hit distance by ShrinkHeight because we started the trace higher than the base.
				// We allow negative distances here, because this allows us to pull out of penetrations.
				const float MaxPenetrationAdjust = FMath::Max(MAX_FLOOR_DIST, PawnRadius);
				const float LineResult = FMath::Max(-MaxPenetrationAdjust, Hit.Time * TraceDist - ShrinkHeight);

				OutFloorResult.bBlockingHit = true;
				if (LineResult <= LineDistance && IsWalkable(Hit))
				{
					OutFloorResult.SetFromLineTrace(Hit, OutFloorResult.FloorDist, LineResult, true);
					return;
				}
			}
		}
	}
	
	// No hits were acceptable.
			UE_LOG(LogTemp, Warning, TEXT("%d %s: No Hitter"), __LINE__, __FUNCTIONW__);
	OutFloorResult.bWalkableFloor = false;
	OutFloorResult.FloorDist = SweepDistance;
}
 void UOrbitCharacterMovementComponent::MaintainHorizontalGroundVelocity() { //if (Velocity.Z != 0.f)
	if (FVector::DotProduct( Velocity, GravityDirection) != 0.f)//gdg
	{
		if (bMaintainHorizontalGroundVelocity)
		{
			// Ramp movement already maintained the velocity, so we just want to remove the vertical component.
			RemoveVertical(Velocity, GravityVector);
		}
		else
		{
			/* This might have something to do with crow hopping but I'm not certain. 
			   In any case, it can make character move slowly and stiffly if not calculated right.
			*/
			// Rescale velocity to be horizontal but maintain magnitude of last update.
			//Velocity = Velocity.SafeNormal2D() * Velocity.Size(); //original
			//	Velocity = Velocity.SafeNormal() * (Velocity.ProjectOnTo(GravityDirection).Size());
			//Velocity = Velocity.SafeNormal() * FVector::DotProduct(Velocity, GravityDirection);
			RemoveVertical(Velocity);
		}
	}
}

bool UOrbitCharacterMovementComponent::DoJump(bool bReplayingMoves)
{
	if ( CharacterOwner && CharacterOwner->CanJump() )
	{
		FVector js = -GravityDirection * JumpZVelocity; //gdg shouldn't that be speed?
		// Don't jump if we can't move up/down.
		//if (!bConstrainToPlane || FMath::Abs(PlaneConstraintNormal.Z) != 1.f)
		if (!bConstrainToPlane || FMath::Abs(FVector::DotProduct(PlaneConstraintNormal, GravityDirection)) != 1.f)
		{
		//	Velocity.Z = JumpZVelocity;
			Velocity += js;//gdg
			SetMovementMode(MOVE_Falling);
			return true;
		}
	}
	
	return false;
}

bool UOrbitCharacterMovementComponent::IsMovingOnGround() const
{
	if (!CharacterOwner || !UpdatedComponent)
	{
		return false;
	}
	return (MovementMode == MOVE_Walking) || (MovementMode == MOVE_Custom && CustomMovementMode == CUSTOM_MoonWalking);
}

void UOrbitCharacterMovementComponent::SetPostLandedPhysics(const FHitResult& Hit)
{
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
	if (OldBase && CurrentFloor.IsWalkableFloor() && CurrentFloor.FloorDist <= MAX_FLOOR_DIST && FVector::DotProduct(Velocity, GravityDirection) <= KINDA_SMALL_NUMBER)//gdg
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
	Super::CalcAvoidanceVelocity(DeltaTime);
}

void UOrbitCharacterMovementComponent::SetDefaultMovementMode()
{
	// check for water volume
	if ( IsInWater() && CanEverSwim() )
	{
		SetMovementMode(DefaultWaterMovementMode);
	}
	else if ( !CharacterOwner || MovementMode != DefaultLandMovementMode )
	{
		SetMovementMode(DefaultLandMovementMode);

		if (DefaultLandMovementMode == MOVE_Custom) CustomMovementMode = CUSTOM_MoonWalking;
		// Avoid 1-frame delay if trying to walk but walking fails at this location.
		if ( (MovementMode == MOVE_Walking || (MovementMode==MOVE_Custom && CustomMovementMode==CUSTOM_MoonWalking) ) && GetMovementBase() == NULL)
		{
			SetMovementMode(MOVE_Falling);
		}
	}
}

float UOrbitCharacterMovementComponent::GetMaxJumpHeight() const
{
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
	// No acceleration in Z
	//FVector FallAcceleration = FVector(Acceleration.X, Acceleration.Y, 0.f);
	FVector FallAcceleration = Acceleration - GravityVector;//gdg

	// bound acceleration, falling object has minimal ability to impact acceleration
	//if (!HasRootMotion() && FallAcceleration.SizeSquared2D() > 0.f)
	if (!HasRootMotion() && FallAcceleration.SizeSquared() > 0.f)//gdg
	{
		FallAcceleration = GetAirControl(DeltaTime, AirControl, FallAcceleration);
#ifdef VERSION27
		FallAcceleration = FallAcceleration.GetClampedToMaxSize(GetMaxAcceleration());
#else
		FallAcceleration = FallAcceleration.ClampMaxSize(GetMaxAcceleration());
#endif
	}

	return FallAcceleration;
}

void UOrbitCharacterMovementComponent::PhysFalling(float deltaTime, int32 Iterations)
{
	if (deltaTime < MIN_TICK_TIME)
	{
		return;
	}

	FVector FallAcceleration = GetFallingLateralAcceleration(deltaTime);
	//FallAcceleration.Z = 0.f;
	RemoveVertical(FallAcceleration);
	//const bool bHasAirControl = (FallAcceleration.SizeSquared2D() > 0.f);
	const bool bHasAirControl = (FallAcceleration.SizeSquared() > 0.f);

	float remainingTime = deltaTime;
	while( (remainingTime >= MIN_TICK_TIME) && (Iterations < MaxSimulationIterations) )
	{
		Iterations++;
		const float timeTick = GetSimulationTimeStep(remainingTime, Iterations);
		remainingTime -= timeTick;
		
		const FVector OldLocation = CharacterOwner->GetActorLocation();
		
		const FRotator PawnRotation = CharacterOwner->GetActorRotation() + GravityDirection.Rotation();
		//GetActorFeetLocation();
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
				RemoveVertical(Velocity,GravityVector);
				CalcVelocity(timeTick, FallingLateralFriction, false, BrakingDecelerationFalling);
				//VelocityNoAirControl = FVector(Velocity.X, Velocity.Y, OldVelocity.Z);
				VelocityNoAirControl = Velocity - OldVelocity.ProjectOnTo(GravityVector);//gdg
			}

			// Compute Velocity
			{
				// Acceleration = FallAcceleration for CalcVelocity(), but we restore it after using it.
				TGuardValue<FVector> RestoreAcceleration(Acceleration, FallAcceleration);
				//Velocity.Z = 0.f;
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
		//const FVector Gravity=GravityVector;//gdg
		//const FVector Gravity=FVector(0,0,0);//gdg
		//Velocity = NewFallVelocity(Velocity, Gravity, timeTick);
		Velocity = NewFallVelocity(Velocity, GravityVector, timeTick);
		//VelocityNoAirControl = NewFallVelocity(VelocityNoAirControl, Gravity, timeTick);
		VelocityNoAirControl = NewFallVelocity(VelocityNoAirControl, GravityVector, timeTick);
		const FVector AirControlAccel = (Velocity - VelocityNoAirControl) / timeTick;

		//if( bNotifyApex && CharacterOwner->Controller && (Velocity.Z <= 0.f) )
		if( bNotifyApex && CharacterOwner->Controller && (FVector::DotProduct(GravityDirection, Velocity) <= 0.f) )
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
			if (IsValidLandingSpot(UpdatedComponent->GetComponentLocation(), Hit))
			{
				remainingTime += subTimeTickRemaining;
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
						bool bDitch = ( FVector::DotProduct(OldHitImpactNormal,GravityDirection) > 0.f) && 
							(FVector::DotProduct(Hit.ImpactNormal,GravityDirection) > 0.f) && 
							(FMath::Abs(FVector::DotProduct(Delta, GravityDirection) <= KINDA_SMALL_NUMBER) && 
							((Hit.ImpactNormal | OldHitImpactNormal) < 0.f) );
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
						//else if (GetPerchRadiusThreshold() > 0.f && Hit.Time == 1.f && OldHitImpactNormal.Z >= GetWalkableFloorZ())
						else if (GetPerchRadiusThreshold() > 0.f && Hit.Time == 1.f && FVector::DotProduct(OldHitImpactNormal,GravityDirection) >= GetWalkableFloorZ())
						{
							// We might be in a virtual 'ditch' within our perch radius. This is rare.
							const FVector PawnLocation = CharacterOwner->GetActorLocation();
							//const float ZMovedDist = FMath::Abs(PawnLocation.Z - OldLocation.Z);
							const float ZMovedDist = (PawnLocation - OldLocation).Size();
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
	if (UpdatedComponent == NULL)
	{
		OutHit.Reset(1.f);
		return false;
	}

	bool bMoveResult = MoveUpdatedComponent(Delta, NewRotation, bSweep, &OutHit);
	//always returns true

	// Handle initial penetrations
	if (OutHit.bStartPenetrating && UpdatedComponent)
	{
		const FVector RequestedAdjustment = GetPenetrationAdjustment(OutHit);
		if (ResolvePenetration(RequestedAdjustment, OutHit, NewRotation))
		{
			// Retry original move
			bMoveResult = MoveUpdatedComponent(Delta, NewRotation, bSweep, &OutHit);
		}
	}

	return bMoveResult;
}
void UOrbitCharacterMovementComponent::StartFalling(int32 Iterations, float remainingTime, float timeTick, const FVector& Delta, const FVector& subLoc)
{
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
	RemoveVertical(Velocity);
	if ( IsMovingOnGround() )
	{
		// This is to catch cases where the first frame of PIE is executed, and the
		// level is not yet visible. In those cases, the player will fall out of the
		// world... So, don't set MOVE_Falling straight away.
		if ( !GIsEditor || (GetWorld()->HasBegunPlay() && (GetWorld()->GetTimeSeconds() >= 1.f)) )
		{
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
	if( CharacterOwner && CharacterOwner->ShouldNotifyLanded(Hit) )
	{
		CharacterOwner->Landed(Hit);
	}
	if( IsFalling() )
	{
		SetPostLandedPhysics(Hit);
	}
/*	if (PathFollowingComp.IsValid())
	{
		//PathFollowingComp->OnLanded();
	}
	*/

	StartNewPhysics(remainingTime, Iterations);
}

bool UOrbitCharacterMovementComponent::IsValidLandingSpot(const FVector& CapsuleLocation, const FHitResult& Hit) const
{
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
		const float LowerHemisphere = FVector::DotProduct(Hit.Location, GravityDirection) - PawnHalfHeight + PawnRadius;
		if ( FVector::DotProduct(Hit.ImpactPoint, GravityDirection) < LowerHemisphere)
		{
			return false;
		}

		// Reject hits that are barely on the cusp of the radius of the capsule
		if (!IsWithinEdgeTolerance(Hit.Location, Hit.ImpactPoint, PawnRadius))
		{
			return false;
		}
	}

	FFindFloorResult FloorResult;
	FindFloor(CapsuleLocation, FloorResult, false, &Hit);

	if (!FloorResult.IsWalkableFloor())
	{
		return false;
	}

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
	bool bBlockingHit = false;

	if (!bUseFlatBaseForFloorChecks)
	{
		//YES!!!!!!!!!!!!! End/2.0 makes it work everywhere. Except the poles.
		//And unless I jump in southern hemisphere which will cause sliding. Didn't I somewhere
		//double sweep radius?
		bBlockingHit = GetWorld()->SweepSingle(OutHit, Start, End/2.f, GetOwner()->GetActorRotation().Quaternion(), TraceChannel, CollisionShape, Params, ResponseParam);//gdg
	}
	else
	{
		// Test with a box that is enclosed by the capsule.
		const float CapsuleRadius = CollisionShape.GetCapsuleRadius();
		const float CapsuleHeight = CollisionShape.GetCapsuleHalfHeight();
		const FCollisionShape BoxShape = FCollisionShape::MakeBox(FVector(CapsuleRadius * 0.707f, CapsuleRadius * 0.707f, CapsuleHeight));

		// First test with the box rotated so the corners are along the major axes (ie rotated 45 degrees).
		//bBlockingHit = GetWorld()->SweepSingle(OutHit, Start, End, FQuat(FVector(0.f, 0.f, -1.f), PI * 0.25f), TraceChannel, BoxShape, Params, ResponseParam);
		bBlockingHit = GetWorld()->SweepSingle(OutHit, Start, End, GetPawnOwner()->GetActorRotation().Add(0,45,0).Quaternion(), TraceChannel, BoxShape, Params, ResponseParam);

		if (!bBlockingHit)
		{
			// Test again with the same box, not rotated.
			OutHit.Reset(1.f, false);			
			bBlockingHit = GetWorld()->SweepSingle(OutHit, Start, End, FQuat::Identity, TraceChannel, BoxShape, Params, ResponseParam);
		}
	}

	return bBlockingHit;
}

bool UOrbitCharacterMovementComponent::IsWithinEdgeTolerance(const FVector& CapsuleLocation, const FVector& TestImpactPoint, const float CapsuleRadius) const
{
	//const float DistFromCenterSqo = (TestImpactPoint - CapsuleLocation).SizeSquared2D();
	const float DistFromCenterSqo = (TestImpactPoint - CapsuleLocation).SizeSquared();
	const float DistFromCenterSq = FVector::DistSquared(TestImpactPoint, CapsuleLocation);
	const float ReducedRadiusSq = FMath::Square(FMath::Max(KINDA_SMALL_NUMBER, CapsuleRadius - SWEEP_EDGE_REJECT_DISTANCE));
	//return DistFromCenterSq < ReducedRadiusSq;
	return DistFromCenterSq >= ReducedRadiusSq;
}

bool UOrbitCharacterMovementComponent::CanStepUp(const FHitResult& Hit) const
{
	if (!Hit.IsValidBlockingHit() || !HasValidData() || MovementMode == MOVE_Falling)
	{
		return false;
	}
	// No component for "fake" hits when we are on a known good base.
	const UPrimitiveComponent* HitComponent = Hit.Component.Get();
	if (!HitComponent)
	{
		return true;
	}

	if (!HitComponent->CanCharacterStepUp(CharacterOwner))
	{
		return false;
	}

	// No actor for "fake" hits when we are on a known good base.
	const AActor* HitActor = Hit.GetActor();
	if (!HitActor)
	{
		 return true;
	}

	if (!HitActor->CanBeBaseForCharacter(CharacterOwner))
	{
		return false;
	}

	return true;
}


bool UOrbitCharacterMovementComponent::StepUp(const FVector& InGravDir, const FVector& Delta, const FHitResult &InHit, FStepDownResult* OutStepDownResult)
{
	/*
	//gdg Crude and I forgot why I did it...
	FVector* StupidShit;
	StupidShit = (FVector*) (&InGravDir);
	*StupidShit = GravityDirection;
*/
//	SCOPE_CYCLE_COUNTER(super::STAT_CharStepUp);


	if (!CanStepUp(InHit))	  
	{
		return false;
	}

	if (MaxStepHeight <= 0.f)
	{
		return false;
	}

	const FVector OldLocation = UpdatedComponent->GetComponentLocation();
	float PawnRadius, PawnHalfHeight;
	CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(PawnRadius, PawnHalfHeight);

	// Don't bother stepping up if top of capsule is hitting something.
	//const float InitialImpactZ = InHit.ImpactPoint.Z;
	//gdg really uncertain here
	//if (InitialImpactZ > OldLocation.Z + (PawnHalfHeight - PawnRadius))
	if ( FVector::DotProduct(InHit.ImpactPoint,GravityDirection) > FVector::DotProduct(OldLocation,GravityDirection) + (PawnHalfHeight - PawnRadius))
	{
		return false;
	}

	// Don't step up if the impact is below us
	//if (InitialImpactZ <= OldLocation.Z - PawnHalfHeight)
	if ( FVector::DotProduct(InHit.ImpactPoint,GravityDirection) <= FVector::DotProduct(OldLocation, GravityDirection) - PawnHalfHeight)//gdg
	{
		return false;
	}

	const FVector GravDir = InGravDir.GetSafeNormal();
	if (GravDir.IsZero())
	{
		return false;
	}
	float StepTravelUpHeight = MaxStepHeight;
	float StepTravelDownHeight = StepTravelUpHeight;
	const float StepSideZ = -1.f * (InHit.ImpactNormal | GravDir);
	float PawnInitialFloorBase = FVector::DotProduct(OldLocation,GravityDirection)- PawnHalfHeight;//gdg
	float PawnFloorPoint = PawnInitialFloorBase;//gdg

	if (IsMovingOnGround() && CurrentFloor.IsWalkableFloor())
	{
		// Since we float a variable amount off the floor, we need to enforce max step height off the actual point of impact with the floor.
		const float FloorDist = FMath::Max(0.f, CurrentFloor.FloorDist);
		PawnInitialFloorBase -= FloorDist;//gdg
		StepTravelUpHeight = FMath::Max(StepTravelUpHeight - FloorDist, 0.f);
		StepTravelDownHeight = (MaxStepHeight + MAX_FLOOR_DIST*2.f);

		const bool bHitVerticalFace = !IsWithinEdgeTolerance(InHit.Location, InHit.ImpactPoint, PawnRadius);
		if (!CurrentFloor.bLineTrace && !bHitVerticalFace)
		{
			//PawnFloorPointZ = CurrentFloor.HitResult.ImpactPoint.Z;
			PawnFloorPoint = FVector::DotProduct(CurrentFloor.HitResult.ImpactPoint, GravityDirection);
		}
		else
		{
			// Base floor point is the base of the capsule moved down by how far we are hovering over the surface we are hitting.
			PawnFloorPoint -= CurrentFloor.FloorDist;//gdg
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
		//HandleImpact(SweepUpHit);
		HandleImpact(SweepUpHit, 0.f, FVector::ZeroVector); //gdg
	}

	// Check result of forward movement
	if (Hit.bBlockingHit)
	{
		if (Hit.bStartPenetrating)
		{
			// Undo movement
			ScopedStepUpMovement.RevertMove();
			return false;
		}

		// pawn ran into a wall
	//	HandleImpact(Hit);
		HandleImpact(Hit, 0.f, FVector::ZeroVector);//gdg
		if ( IsFalling() )
		{
			return true;
		}

		// adjust and try again
		const float ForwardHitTime = Hit.Time;
		const float ForwardSlideAmount = SlideAlongSurface(Delta, 1.f - Hit.Time, Hit.Normal, Hit, true);
		
		if (IsFalling())
		{
			ScopedStepUpMovement.RevertMove();
			return false;
		}

		// If both the forward hit and the deflection got us nowhere, there is no point in this step up.
		if (ForwardHitTime == 0.f && ForwardSlideAmount == 0.f)
		{
			ScopedStepUpMovement.RevertMove();
			return false;
		}
	}
	
	// Step down
	SafeMoveUpdatedComponent(GravDir * StepTravelDownHeight, CharacterOwner->GetActorRotation(), true, Hit);

	// If step down was initially penetrating abort the step up
	if (Hit.bStartPenetrating)
	{
		ScopedStepUpMovement.RevertMove();
		return false;
	}																				 

	FStepDownResult StepDownResult;
	if (Hit.IsValidBlockingHit())
	{	
		// See if this step sequence would have allowed us to travel higher than our max step height allows.
		//const float DeltaZ = Hit.ImpactPoint.Z - PawnFloorPointZ;
		const float DeltaZ = FVector::DotProduct(Hit.ImpactPoint, GravityDirection) - PawnFloorPoint;
		if (DeltaZ > MaxStepHeight)
		{
			ScopedStepUpMovement.RevertMove();
			return false;
		}

		// Reject unwalkable surface normals here.
		if (!IsWalkable(Hit))
		{
			// Reject if normal opposes movement direction
			const bool bNormalTowardsMe = (Delta | Hit.ImpactNormal) < 0.f;
			if (bNormalTowardsMe)
			{
				UE_LOG(LogCharacterMovement, Warning, TEXT("- Reject StepUp (unwalkable normal %s opposed to movement)"), *Hit.ImpactNormal.ToString());
				ScopedStepUpMovement.RevertMove();
				return false;
			}

			// Also reject if we would end up being higher than our starting location by stepping down.
			// It's fine to step down onto an unwalkable normal below us, we will just slide off. Rejecting those moves would prevent us from being able to walk off the edge.
			if ( FVector::DotProduct(Hit.Location, GravityDirection) > FVector::DotProduct(OldLocation, GravityDirection))//gdg
			{
				UE_LOG(LogCharacterMovement, Warning, TEXT("- Reject StepUp (unwalkable normal %s above old position)"), *Hit.ImpactNormal.ToString());
				ScopedStepUpMovement.RevertMove();
				return false;
			}
		}

		// Reject moves where the downward sweep hit something very close to the edge of the capsule. This maintains consistency with FindFloor as well.
		if (!IsWithinEdgeTolerance(Hit.Location, Hit.ImpactPoint, PawnRadius))
		{
			UE_LOG(LogCharacterMovement, Warning, TEXT("- Reject StepUp (outside edge tolerance)"));
			ScopedStepUpMovement.RevertMove();
			return false;
		}

		// Don't step up onto invalid surfaces if traveling higher.
		if (DeltaZ > 0.f && !CanStepUp(Hit))
		{
			UE_LOG(LogCharacterMovement, Warning, TEXT("- Reject StepUp (up onto surface with !CanStepUp())"));
			ScopedStepUpMovement.RevertMove();
	UE_LOG(LogTemp, Warning, TEXT("%d Can't step up"), __LINE__);
			return false;
		}

		// See if we can validate the floor as a result of this step down. In almost all cases this should succeed, and we can avoid computing the floor outside this method.
		if (OutStepDownResult != NULL)
		{
			FindFloor(UpdatedComponent->GetComponentLocation(), StepDownResult.FloorResult, false, &Hit);

			// Reject unwalkable normals if we end up higher than our initial height.
			// It's fine to walk down onto an unwalkable surface, don't reject those moves.
			//if (Hit.Location.Z > OldLocation.Z)
			if (FVector::DotProduct(Hit.Location,GravityDirection) > FVector::DotProduct(OldLocation,GravityDirection))
			{
				// We should reject the floor result if we are trying to step up an actual step where we are not able to perch (this is rare).
				// In those cases we should instead abort the step up and try to slide along the stair.
				if (!StepDownResult.FloorResult.bBlockingHit && StepSideZ < MAX_STEP_SIDE_Z)
				{
					ScopedStepUpMovement.RevertMove();
	UE_LOG(LogTemp, Warning, TEXT("%d Can't step up"), __LINE__);
					return false;
				}
			}

			StepDownResult.bComputedFloor = true;
		}
	}
	
	// Copy step down result.
	if (OutStepDownResult != NULL)
	{
		*OutStepDownResult = StepDownResult;
	}

	// Don't recalculate velocity based on this height adjustment, if considering vertical adjustments.
	bJustTeleported |= !bMaintainHorizontalGroundVelocity;

	return true;
}

bool UOrbitCharacterMovementComponent::ShouldCheckForValidLandingSpot(float DeltaTime, const FVector& Delta, const FHitResult& Hit) const
{
	// See if we hit an edge of a surface on the lower portion of the capsule.
	// In this case the normal will not equal the impact normal, and a downward sweep may find a walkable surface on top of the edge.
	//if (Hit.Normal.Z > KINDA_SMALL_NUMBER && !Hit.Normal.Equals(Hit.ImpactNormal))
	if (FVector::DotProduct(Hit.Normal, GravityDirection) > KINDA_SMALL_NUMBER && !Hit.Normal.Equals(Hit.ImpactNormal))
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
	if (InMaxFloorDist <= 0.f)
	{
		return 0.f;
	}

	// Sweep further than actual requested distance, because a reduced capsule radius means we could miss some hits that the normal radius would contact.
	float PawnRadius, PawnHalfHeight;
	CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(PawnRadius, PawnHalfHeight);

	//const float InHitAboveBase = FMath::Max(0.f, InHit.ImpactPoint.Z - (InHit.Location.Z - PawnHalfHeight));
	const float InHitAboveBase = FMath::Max(0.f, FVector::DotProduct(InHit.ImpactPoint,GravityDirection) - 
		(FVector::DotProduct(InHit.Location,GravityDirection) - PawnHalfHeight));

	const float PerchLineDist = FMath::Max(0.f, InMaxFloorDist - InHitAboveBase);
	const float PerchSweepDist = FMath::Max(0.f, InMaxFloorDist);

	const float ActualSweepDist = PerchSweepDist + PawnRadius;
	const FHitResult dummy(1.f);
	ComputeFloorDist(InHit.Location, PerchLineDist, ActualSweepDist, OutPerchFloorResult, TestRadius, &dummy);

	if (!OutPerchFloorResult.IsWalkableFloor())
	{
		return false;
	}
	else if (InHitAboveBase + OutPerchFloorResult.FloorDist > InMaxFloorDist)
	{
		// Hit something past max distance
		OutPerchFloorResult.bWalkableFloor = false;
		return false;
	}

	return true;
}

void UOrbitCharacterMovementComponent::HandleImpact(FHitResult const& Impact, float TimeSlice, const FVector& MoveDelta)
{
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
	FVector NewAccel = InputAcceleration;

	// walking or falling pawns ignore up/down sliding
	if (IsMovingOnGround() || IsFalling())
	{
		// This definately had something to do with sticking at equator
		//	NewAccel.Z = 0.f;
		NewAccel = NewAccel - NewAccel.ProjectOnTo(GravityVector);
//		NewAccel = NewAccel - NewAccel.VectorPlaneProject(GravityDirection, NewAccel);//seems smoother, but is it right?
	}

	return NewAccel;
}
void UOrbitCharacterMovementComponent::ApplyImpactPhysicsForces(const FHitResult& Impact, const FVector& ImpactAcceleration, const FVector& ImpactVelocity)
{
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
	float ret;
	// Don't allow negative values.
	ret=FMath::Max(0.f, PerchRadiusThreshold);
	return ret;
//	return PerchRadiusThreshold;//gdg
}


float UOrbitCharacterMovementComponent::GetValidPerchRadius() const
{
	if (CharacterOwner)
	{
		const float PawnRadius = CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius();
		return FMath::Clamp(PawnRadius - GetPerchRadiusThreshold(), 0.1f, PawnRadius);
	}
	return 0.f;
}


bool UOrbitCharacterMovementComponent::ShouldComputePerchResult(const FHitResult& InHit, bool bCheckRadius) const
{
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
	return;
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
		//This looks fishy to me
		//if ( Acceleration.Z > 0.f && ShouldJumpOutOfWater(JumpDir)
		if ( FVector::DotProduct(Acceleration, GravityDirection) > 0.f && ShouldJumpOutOfWater(JumpDir)
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
	AController* OwnerController = CharacterOwner->GetController();
	if (OwnerController)
	{
		const FRotator ControllerRot = OwnerController->GetControlRotation();
		//if ( (Velocity.Z > 0.0f) && (ControllerRot.Pitch > JumpOutOfWaterPitch) )
		if( (FVector::DotProduct(Velocity, GravityDirection) > 0.0f) && (ControllerRot.Pitch > JumpOutOfWaterPitch) )
			
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
					if (FMath::Abs( FVector::DotProduct(Hit.ImpactNormal, GravityDirection)) < 0.2f)
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
//	Super::MoveAlongFloor(InVelocity, DeltaSeconds, OutStepDownResult);

	//const FVector Delta = FVector(InVelocity.X, InVelocity.Y, 0.f) * DeltaSeconds;
	const FVector Delta =  InVelocity * DeltaSeconds;
	
	if (!CurrentFloor.IsWalkableFloor())
	{
		return;
	}

	// Move along the current floor
	FHitResult Hit(1.f);
	FVector RampVector = ComputeGroundMovementDelta(Delta, CurrentFloor.HitResult, CurrentFloor.bLineTrace);
	SafeMoveUpdatedComponent(RampVector, CharacterOwner->GetActorRotation(), true, Hit);

	float LastMoveTimeSlice = DeltaSeconds;
	
	if (Hit.bStartPenetrating)
	{
		OnCharacterStuckInGeometry();
	}

	if (Hit.IsValidBlockingHit())
	{
		// We impacted something (most likely another ramp, but possibly a barrier).
		float PercentTimeApplied = Hit.Time;
		//if ((Hit.Time > 0.f) && (Hit.Normal.Z > KINDA_SMALL_NUMBER) && IsWalkable(Hit))
		if ((Hit.Time > 0.f) && (FVector::DotProduct(Hit.Normal,GravityDirection) > KINDA_SMALL_NUMBER) && IsWalkable(Hit))
		{
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
			//CanStepUp is called from skippy but not smooth. However, skippy doesn't always get here when skipping
			//it seems to have something to with going forward.
			if (CanStepUp(Hit) || (CharacterOwner->GetMovementBase() != NULL && CharacterOwner->GetMovementBase()->GetOwner() == Hit.GetActor()))
			{
				// hit a barrier, try to step up
				//const FVector GravDir(0.f, 0.f, -1.f);
				//const FVector GravDir = GravityDirection;
				const FVector GravDir = GravityVector;
				if (!StepUp(GravDir, Delta * (1.f - PercentTimeApplied), Hit, OutStepDownResult))
				{
					UE_LOG(LogCharacterMovement, Warning, TEXT("- StepUp (ImpactNormal %s, Normal %s"), *Hit.ImpactNormal.ToString(), *Hit.Normal.ToString());
					HandleImpact(Hit, LastMoveTimeSlice, RampVector);
					//get blocked by my luna
					SlideAlongSurface(Delta, 1.f - PercentTimeApplied, Hit.Normal, Hit, true);
					//SlideAlongSurface(Delta, 1.f - PercentTimeApplied, Hit.Normal, Hit, false);//gdg
				}
				else
				{
					// Don't recalculate velocity based on this height adjustment, if considering vertical adjustments.
					UE_LOG(LogCharacterMovement, Warning, TEXT("+ StepUp (ImpactNormal %s, Normal %s"), *Hit.ImpactNormal.ToString(), *Hit.Normal.ToString());
					bJustTeleported |= !bMaintainHorizontalGroundVelocity;
				}
			}
			else if ( Hit.Component.IsValid() && !Hit.Component.Get()->CanCharacterStepUp(CharacterOwner) )
			{
				SlideAlongSurface(Delta, 1.f - PercentTimeApplied, Hit.Normal, Hit, true);
				//SlideAlongSurface(Delta, 1.f - PercentTimeApplied, Hit.Normal, Hit, false);//gdg
			}
		}
	}
}

FVector UOrbitCharacterMovementComponent::ComputeGroundMovementDelta(const FVector& Delta, const FHitResult& RampHit, const bool bHitFromLineTrace) const
{
	const FVector FloorNormal = RampHit.ImpactNormal;
	const FVector ContactNormal = RampHit.Normal;

	//if (FloorNormal.Z < (1.f - KINDA_SMALL_NUMBER) && FloorNormal.Z > KINDA_SMALL_NUMBER && ContactNormal.Z > KINDA_SMALL_NUMBER && !bHitFromLineTrace && IsWalkable(RampHit))
	if (FVector::DotProduct(FloorNormal, GravityDirection) < (1.f - KINDA_SMALL_NUMBER) && FVector::DotProduct(FloorNormal, GravityDirection) > KINDA_SMALL_NUMBER && FVector::DotProduct(ContactNormal, GravityDirection) > KINDA_SMALL_NUMBER && !bHitFromLineTrace && IsWalkable(RampHit))
	{
		// Compute a vector that moves parallel to the surface, by projecting the horizontal movement 
		// direction onto the ramp.
		const float FloorDotDelta = (FloorNormal | Delta);
		//FVector RampMovement(Delta.X, Delta.Y, -FloorDotDelta / FloorNormal.Z);
		//FVector RampMovement(Delta.X, Delta.Y, -FloorDotDelta / FloorNormal.ProjectOnTo(GravityDirection).Size() );//gdg
	//	FVector RampMovement = Delta;//gdg
		//FVector RampMovement = Delta / FloorNormal;
		FVector RampMovement =  Delta.ProjectOnTo( FloorNormal ) * -1.0/FloorDotDelta;
		
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
	if (!Hit.bBlockingHit)
	{
		return 0.f;
	}

	FVector Normal(InNormal);
	if (IsMovingOnGround())
	{
		// We don't want to be pushed up an unwalkable surface.
		//if (Normal.Z > 0.f)
		if (FVector::DotProduct(Normal, GravityDirection) > 0.f)
		{
			if (!IsWalkable(Hit))
			{
				//Normal = Normal.GetSafeNormal2D();
				Normal = Normal.GetSafeNormal();
			}
		}
		//else if (Normal.Z < -KINDA_SMALL_NUMBER)
		else if (FVector::DotProduct(Normal, GravityDirection) < -KINDA_SMALL_NUMBER)
		{
			// Don't push down into the floor when the impact is on the upper portion of the capsule.
			if (CurrentFloor.FloorDist < MIN_FLOOR_DIST && CurrentFloor.bBlockingHit)
			{
				const FVector FloorNormal = CurrentFloor.HitResult.Normal;
				//const bool bFloorOpposedToMovement = (Delta | FloorNormal) < 0.f && (FloorNormal.Z < 1.f - DELTA);
				const bool bFloorOpposedToMovement = (Delta | FloorNormal) < 0.f && (FVector::DotProduct(FloorNormal, GravityDirection) < 1.f - DELTA);
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
	bBlockingHit = InHit.IsValidBlockingHit(); // return bBlockingHit && !bStartPenetrating;
	bWalkableFloor = bIsWalkableFloor;
	bLineTrace = false;
	FloorDist = InSweepFloorDist;
	LineDist = 0.f;
	HitResult = InHit;
}

void FOrbitFindFloorResult::SetFromLineTrace(const FHitResult& InHit, const float InSweepFloorDist, const float InLineDist, const bool bIsWalkableFloor)
{
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
	}
}
bool UOrbitCharacterMovementComponent::MoveUpdatedComponent( const FVector& Delta, const FRotator& NewRotation, bool bSweep, FHitResult* OutHit)
{
	if (UpdatedComponent)
	{
		const FVector NewDelta = ConstrainDirectionToPlane(Delta);
		bool ret = UpdatedComponent->MoveComponent(NewDelta, NewRotation, bSweep, OutHit, MoveComponentFlags);
		return ret;
	}

	return false;
}

FVector UOrbitCharacterMovementComponent::ConstrainDirectionToPlane(FVector Direction) const
{

	if (bConstrainToPlane)
	{
		Direction = FVector::VectorPlaneProject(Direction, PlaneConstraintNormal);
	}

	return Direction;
}

FVector UOrbitCharacterMovementComponent::GetImpartedMovementBaseVelocity() const
{
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
	FVector InDelta = Delta;
	Super::TwoWallAdjust(Delta, Hit, OldHitNormal);

	if (IsMovingOnGround())
	{
		// Allow slides up walkable surfaces, but not unwalkable ones (treat those as vertical barriers).
		//if (Delta.Z > 0.f)
		if (FVector::DotProduct( Delta, GravityDirection ) > 0.f)
		{
			//if ((Hit.Normal.Z >= WalkableFloorZ || IsWalkable(Hit)) && Hit.Normal.Z > KINDA_SMALL_NUMBER)
			if ((FVector::DotProduct(Hit.Normal, GravityDirection) >= GetWalkableFloorZ() || IsWalkable(Hit)) && FVector::DotProduct(Hit.Normal, GravityDirection) > KINDA_SMALL_NUMBER)
				if ((FVector::DotProduct(Hit.Normal, GravityDirection) >= GetWalkableFloorZ() 
					|| IsWalkable(Hit)) && FVector::DotProduct(Hit.Normal,GravityDirection) > KINDA_SMALL_NUMBER)
			{
				// Maintain horizontal velocity
				const float Time = (1.f - Hit.Time);
				const FVector ScaledDelta = Delta.GetSafeNormal() * InDelta.Size();
				//Delta = FVector(InDelta.X, InDelta.Y, ScaledDelta.Z / Hit.Normal.Z) * Time;
				//LifIKnow
				Delta = InDelta.ProjectOnTo(GravityDirection) / Hit.Normal.ProjectOnTo(GravityDirection) * Time;
			}
			else
			{
				//Delta.Z = 0.f;
				Delta = Delta - Delta.ProjectOnTo(GravityDirection);
			}
		}
		//else if (Delta.Z < 0.f)
		//is this right? Can size be < 0? No, it isn't right, size IS ALWAYS POSITIVE
		//else if (Delta.ProjectOnTo(GravityDirection).Size() < 0.f)
		//same problem with distance. Vector magnitudes are always >=0
		//else if (FVector::Dist( FVector::ZeroVector, Delta.ProjectOnTo(GravityDirection)) < 0.f)
		else if (FVector::DotProduct( Delta, GravityDirection ) < 0.f)
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

bool UOrbitCharacterMovementComponent::IsFalling() const
{
	if (!CharacterOwner || !UpdatedComponent)
	{											   
		return false;
	}

	return MovementMode == MOVE_Falling;
}
void UOrbitCharacterMovementComponent::PerformMovement(float DeltaSeconds)
{
//	SCOPE_CYCLE_COUNTER(STAT_CharacterMovement);
//	SCOPE_CYCLE_COUNTER(STAT_CharacterMovementAuthority);

	if (!HasValidData())
	{
		return;
	}
	
	// no movement if we can't move, or if currently doing physical simulation on UpdatedComponent
	if (MovementMode == MOVE_None || UpdatedComponent->Mobility != EComponentMobility::Movable || UpdatedComponent->IsSimulatingPhysics())
	{
		return;
	}

	// Force floor update if we've moved outside of CharacterMovement since last update.
	bForceNextFloorCheck |= (IsMovingOnGround() && UpdatedComponent->GetComponentLocation() != LastUpdateLocation);

	FVector OldVelocity;
	FVector OldLocation;

	// Scoped updates can improve performance of multiple MoveComponent calls.
	{
		FScopedMovementUpdate ScopedMovementUpdate(UpdatedComponent, bEnableScopedMovementUpdates ? EScopedUpdate::DeferredUpdates : EScopedUpdate::ImmediateUpdates);

		MaybeUpdateBasedMovement(DeltaSeconds);

		OldVelocity = Velocity;
		OldLocation = CharacterOwner->GetActorLocation();

		ApplyAccumulatedForces(DeltaSeconds);

		// Check for a change in crouch state. Players toggle crouch by changing bWantsToCrouch.
		const bool bAllowedToCrouch = CanCrouchInCurrentState();
		if ((!bAllowedToCrouch || !bWantsToCrouch) && IsCrouching())
		{
			UnCrouch(false);
		}
		else if (bWantsToCrouch && bAllowedToCrouch && !IsCrouching()) 
		{
			Crouch(false);
		}
		/*
		if (MovementMode == MOVE_NavWalking && bWantsToLeaveNavWalking)
		{
			TryToLeaveNavWalking();
		}*/

		// Character::LaunchCharacter() has been deferred until now.
		HandlePendingLaunch();

		// If using RootMotion, tick animations before running physics.
		if( !CharacterOwner->bClientUpdating && CharacterOwner->IsPlayingRootMotion() && CharacterOwner->GetMesh() )
		{
			TickCharacterPose(DeltaSeconds);

			// Make sure animation didn't trigger an event that destroyed us
			if (!HasValidData())
			{
				return;
			}

			// For local human clients, save off root motion data so it can be used by movement networking code.
			if( CharacterOwner->IsLocallyControlled() && (CharacterOwner->Role == ROLE_AutonomousProxy) && CharacterOwner->IsPlayingNetworkedRootMotionMontage() )
			{
				CharacterOwner->ClientRootMotionParams = RootMotionParams;
			}
		}

		// if we're about to use root motion, convert it to world space first.
		if( HasRootMotion() )
		{
			USkeletalMeshComponent * SkelMeshComp = CharacterOwner->GetMesh();
			if( SkelMeshComp )
			{
				// Convert Local Space Root Motion to world space. Do it right before used by physics to make sure we use up to date transforms, as translation is relative to rotation.
				RootMotionParams.Set( SkelMeshComp->ConvertLocalRootMotionToWorld(RootMotionParams.RootMotionTransform) );
				UE_LOG(LogRootMotion, Log,  TEXT("PerformMovement WorldSpaceRootMotion Translation: %s, Rotation: %s, Actor Facing: %s"),
					*RootMotionParams.RootMotionTransform.GetTranslation().ToCompactString(), *RootMotionParams.RootMotionTransform.GetRotation().Rotator().ToCompactString(), *CharacterOwner->GetActorRotation().Vector().ToCompactString());
			}

			// Then turn root motion to velocity to be used by various physics modes.
			if( DeltaSeconds > 0.f )
			{
				const FVector RootMotionVelocity = RootMotionParams.RootMotionTransform.GetTranslation() / DeltaSeconds;
				// Do not override Velocity.Z if in falling physics, we want to keep the effect of gravity.
				/*
				Velocity = FVector(RootMotionVelocity.X, RootMotionVelocity.Y, 
					(MovementMode == MOVE_Falling ? Velocity.Z : RootMotionVelocity.Z));
*/
				if (MovementMode == MOVE_Falling){
					//Velocity = FVector(RootMotionVelocity.X, RootMotionVelocity.Y, Velocity.Z));
					//Velocity = Velocity - RootMotionVelocity.ProjectOnTo(GravityDirection);
					RemoveVertical(Velocity);
				}
				else{
					//Velocity = FVector(RootMotionVelocity.X, RootMotionVelocity.Y, RootMotionVelocity.Z));
					Velocity = RootMotionVelocity;
				}
			}
		}

		// NaN tracking
		checkf(!Velocity.ContainsNaN(), TEXT("UCharacterMovementComponent::PerformMovement: Velocity contains NaN (%s: %s)\n%s"), *GetPathNameSafe(this), *GetPathNameSafe(GetOuter()), *Velocity.ToString());

		// Clear jump input now, to allow movement events to trigger it for next update.
		CharacterOwner->ClearJumpInput();

		// change position
		StartNewPhysics(DeltaSeconds, 0);

		if (!HasValidData())
		{
			return;
		}

		// uncrouch if no longer allowed to be crouched
		if (IsCrouching() && !CanCrouchInCurrentState())
		{
			UnCrouch(false);
		}

		if (!HasRootMotion() && !CharacterOwner->IsMatineeControlled())
		{
			PhysicsRotation(DeltaSeconds);
		}

		// Apply Root Motion rotation after movement is complete.
		if( HasRootMotion() )
		{
			const FRotator OldActorRotation = CharacterOwner->GetActorRotation();
			const FRotator RootMotionRotation = RootMotionParams.RootMotionTransform.GetRotation().Rotator();
			if( !RootMotionRotation.IsNearlyZero() )
			{
				const FRotator NewActorRotation = (OldActorRotation + RootMotionRotation).GetNormalized();
				FHitResult fauxHit(1.0f);
				MoveUpdatedComponent(FVector::ZeroVector, NewActorRotation, true, &fauxHit);
			}

			// debug
			if( true )
			{
				const FVector ResultingLocation = CharacterOwner->GetActorLocation();
				const FRotator ResultingRotation = CharacterOwner->GetActorRotation();

				// Show current position
				DrawDebugCoordinateSystem(GetWorld(), CharacterOwner->GetMesh()->GetComponentLocation() + FVector(0,0,1), ResultingRotation, 50.f, false);

				// Show resulting delta move.
				DrawDebugLine(GetWorld(), OldLocation, ResultingLocation, FColor::Red, true, 10.f);

				// Log details.
				UE_LOG(LogRootMotion, Warning,  TEXT("PerformMovement Resulting DeltaMove Translation: %s, Rotation: %s, MovementBase: %s"),
					*(ResultingLocation - OldLocation).ToCompactString(), *(ResultingRotation - OldActorRotation).GetNormalized().ToCompactString(), *GetNameSafe(CharacterOwner->GetMovementBase()) );

				const FVector RMTranslation = RootMotionParams.RootMotionTransform.GetTranslation();
				const FRotator RMRotation = RootMotionParams.RootMotionTransform.GetRotation().Rotator();
				UE_LOG(LogRootMotion, Warning,  TEXT("PerformMovement Resulting DeltaError Translation: %s, Rotation: %s"),
					*(ResultingLocation - OldLocation - RMTranslation).ToCompactString(), *(ResultingRotation - OldActorRotation - RMRotation).GetNormalized().ToCompactString() );
			}

			// Root Motion has been used, clear
			RootMotionParams.Clear();
		}

		// consume path following requested velocity
		bHasRequestedVelocity = false;

		OnMovementUpdated(DeltaSeconds, OldLocation, OldVelocity);
	} // End scoped movement update

	// Call external post-movement events. These happen after the scoped movement completes in case the events want to use the current state of overlaps etc.
	CallMovementUpdateDelegate(DeltaSeconds, OldLocation, OldVelocity);

	SaveBaseLocation();
	UpdateComponentVelocity();

	LastUpdateLocation = UpdatedComponent ? UpdatedComponent->GetComponentLocation() : FVector::ZeroVector;
}


void UOrbitCharacterMovementComponent::RemoveVertical(FVector &OutVector){
	//should this be GravityDirection or GravityVector...school was so long ago and in a galaxy far, far away
	OutVector -= OutVector.ProjectOnTo(GravityVector);
}
void UOrbitCharacterMovementComponent::RemoveVertical(FVector &OutVector, FVector VerticalComponent){
	OutVector -= OutVector.ProjectOnTo(VerticalComponent);
}
