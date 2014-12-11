// Fill out your copyright notice in the Description page of Project Settings.

#include "Orbit.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "OrbitCharacterMovementComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogCharacterMovement, Log, All);

// Version that does not use inverse sqrt estimate, for higher precision.
FORCEINLINE FVector ClampMaxSizePrecise(const FVector& V, float MaxSize)
{
	if (MaxSize < KINDA_SMALL_NUMBER)
	{
		return FVector::ZeroVector;
	}

	const float VSq = V.SizeSquared();
	if (VSq > FMath::Square(MaxSize))
	{
		return V * (MaxSize / FMath::Sqrt(VSq));
	}
	else
	{
		return V;
	}
}

// gdg There is likely no need to override this since it doesn't appear to be used for anything but acceleration
// and accl.z is always 0 when it gets here.
// Version that does not use inverse sqrt estimate, for higher precision.
FORCEINLINE FVector SafeNormalPrecise(const FVector& V)
{
//	UE_LOG(LogTemp, Warning, TEXT("safe norm input:%s"), *V.ToString());
	//V.Z is always zero from what I have seen
	const float VSq = V.SizeSquared();
	if (VSq < SMALL_NUMBER)
	{
		return FVector::ZeroVector;
	}
	else
	{
		return V * (1.f / FMath::Sqrt(VSq));
	}
}

UOrbitCharacterMovementComponent::UOrbitCharacterMovementComponent(const class FObjectInitializer& PCIP)
	: Super(PCIP)
{
	RotationRate = FRotator(360.0f, 360.0f, 360.0f);
	bMaintainHorizontalGroundVelocity = false;
	//	SetWalkableFloorZ(11110.f); //doesn't seem to matter currently

	OldGravDir = FVector(0, 0, 0);

	//	UE_LOG(LogTemp, Warning, TEXT("!!!!!!!!!!!!!!!!!!!!!!!!! Super Happy happy constructor"));

}

// Magnitude of Gravity
float UOrbitCharacterMovementComponent::GetGravityZ() const
{
	float distance, magnitude, gravBodyMass = 9000000.f;
	distance = FVector::Dist(FVector(0, 0, 5000), GetActorLocation());
	magnitude = (Mass * gravBodyMass) / FMath::Square(distance);
	return magnitude;
}

// Normalized Gravity Direction Vector
FVector UOrbitCharacterMovementComponent::GetGravityDir() const
{
	//FIXME Need to iterate over massive things to get vector sum of direction and force
	//This may not even be the right class for it
	return -( GetActorLocation()-FVector(0.f, 0.f, 5000.f) ).UnsafeNormal();
}

// Magnitude and Direction Vector of Gravity
FVector UOrbitCharacterMovementComponent::GetGravityV() const {
	return GetGravityDir() * GetGravityZ();
}

void UOrbitCharacterMovementComponent::InitializeComponent()
{
	Super::InitializeComponent();
	//	UE_LOG(LogTemp, Warning, TEXT("!!!!!!!!!!!!!!!!!!!!!!!!! IN8itize Component"));

	//~~~~~~~~~~~~~~~~~
	//UE_LOG //comp Init!
}

void UOrbitCharacterMovementComponent::ProcessLanded(const FHitResult& Hit, float remainingTime, int32 Iterations)
{

	if (CharacterOwner && CharacterOwner->ShouldNotifyLanded(Hit))
	{
		CharacterOwner->Landed(Hit);
	}
	if (IsFalling())
	{
		SetPostLandedPhysics(Hit);
	}
	if (PathFollowingComp.IsValid())
	{
		//	PathFollowingComp->OnLanded();
	}

	StartNewPhysics(remainingTime, Iterations);
}
void UOrbitCharacterMovementComponent::SetPostLandedPhysics(const FHitResult& Hit)
{

	if (CharacterOwner)
	{

		if (GetPhysicsVolume()->bWaterVolume && CanEverSwim())
		{
			SetMovementMode(MOVE_Swimming);
		}
		else
		{

			const FVector PreImpactAccel = Acceleration + (IsFalling() ? GetGravityV() : FVector::ZeroVector);
			const FVector PreImpactVelocity = Velocity;
			SetMovementMode(MOVE_Walking);
			ApplyImpactPhysicsForces(Hit, PreImpactAccel, PreImpactVelocity);
		}
	}
}
bool UOrbitCharacterMovementComponent::DoJump(bool bReplayingMoves)
{
	UE_LOG(LogTemp, Warning, TEXT("Jump!!! %s"), *CharacterOwner->GetActorLocation().ToString());
	if (CharacterOwner && CharacterOwner->CanJump())
	{
	UE_LOG(LogTemp, Warning, TEXT("Jump!!! Can Jump"));
		// Don't jump if we can't move up/down.
		if (!bConstrainToPlane || FMath::Abs(PlaneConstraintNormal.Z) != 1.f)
		{
			const FVector GravityDir = GetGravityV();
	UE_LOG(LogTemp, Warning, TEXT("Jump!!! No up down constraint"));
		//orig	Velocity.Z = JumpZVelocity;
	UE_LOG(LogTemp, Warning, TEXT("Jump!!! vel1:%s"), *Velocity.ToString());
			Velocity -= GetGravityDir() * JumpZVelocity;//subtract b/c jump force opposes grav force
	UE_LOG(LogTemp, Warning, TEXT("Jump!!! GravityVec:%s"), *GravityDir.ToString());
			SetMovementMode(MOVE_Falling);
			// this->GetCharacterOwner()->ClientSetRotation( GetGravityDir().Rotation() );//gdg attempt sorta but not quite

			return true;
		}
	}

	return false;
}

void UOrbitCharacterMovementComponent::PhysWalking(float deltaTime, int32 Iterations)
{

	if (deltaTime < MIN_TICK_TIME)
	{
		return;
	}

	if ((!CharacterOwner || !CharacterOwner->Controller) && !bRunPhysicsWithNoController && !HasRootMotion())
	{
		Acceleration = FVector::ZeroVector;
		Velocity = FVector::ZeroVector;
		return;
	}

	if (!UpdatedComponent->IsCollisionEnabled())
	{
		SetMovementMode(MOVE_Walking);
		return;
	}
	if (Velocity.ContainsNaN()) Velocity = FVector(0, 0, -10);//gdg added to prevent exception, no idea what I did to need this
	checkf(!Velocity.ContainsNaN(), TEXT("PhysWalking: Velocity contains NaN before Iteration (%s: %s)\n%s"), *GetPathNameSafe(this), *GetPathNameSafe(GetOuter()), *Velocity.ToString());

	bJustTeleported = false;
	bool bCheckedFall = false;
	bool bTriedLedgeMove = false;
	float remainingTime = deltaTime;

	// Perform the move
	while ((remainingTime >= MIN_TICK_TIME) && (Iterations < MaxSimulationIterations) && (CharacterOwner->Controller || bRunPhysicsWithNoController || HasRootMotion()))
	{
	//UE_LOG(LogTemp, Warning, TEXT("%d Performing Movement Velocity:%s"),__LINE__, *Velocity.ToString()); //Z non-zero when jumping
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
		 MaintainHorizontalGroundVelocity(); //don't comment out b/c won't land properly
		//gdg	Velocity.Z = 0.f;
		const FVector OldVelocity = Velocity;

		// Apply acceleration
		//gdg	Acceleration.Z = 0.f;
		if (!HasRootMotion())
		{
			CalcVelocity(timeTick, GroundFriction, false, BrakingDecelerationWalking);
		}
		checkf(!Velocity.ContainsNaN(), TEXT("PhysWalking: Velocity contains NaN after CalcVelocity (%s: %s)\n%s"), *GetPathNameSafe(this), *GetPathNameSafe(GetOuter()), *Velocity.ToString());

		// Compute move parameters
		const FVector MoveVelocity = Velocity;
		const FVector Delta = timeTick * MoveVelocity;
		const bool bZeroDelta = Delta.IsNearlyZero();
		FStepDownResult StepDownResult;

		if (bZeroDelta)
		{
			remainingTime = 0.f;
		}
		else
		{
			// try to move forward
			MoveAlongFloor(MoveVelocity, timeTick, &StepDownResult); //Kitty was here

			if (IsFalling())
			{
				UE_LOG(LogTemp, Warning, TEXT("Transition from walk to fall"));

				// pawn decided to jump up
				const float DesiredDist = Delta.Size();
				if (DesiredDist > KINDA_SMALL_NUMBER)
				{
					const float ActualDist = (CharacterOwner->GetActorLocation() - OldLocation).Size2D();
					remainingTime += timeTick * (1.f - FMath::Min(1.f, ActualDist / DesiredDist));
				}
				StartNewPhysics(remainingTime, Iterations);
				return;
			}
			else if (IsSwimming()) //just entered water
			{
				StartSwimming(OldLocation, OldVelocity, timeTick, remainingTime, Iterations);
				return;
			}
		}

		// Update floor.
		// StepUp might have already done it for us.
		if (StepDownResult.bComputedFloor)
		{
			CurrentFloor = StepDownResult.FloorResult;
		}
		else
		{
			FindFloor(UpdatedComponent->GetComponentLocation(), CurrentFloor, bZeroDelta, NULL);
		}
// !!!!!!!!!!!!!!!!! Z is zero again by here. Haven't bailed out at this point when stuck at equator
		// check for ledges here
		const bool bCheckLedges = !CanWalkOffLedges();
		if (bCheckLedges && !CurrentFloor.IsWalkableFloor())
		{

			// calculate possible alternate movement
			const FVector GravDir = GetGravityDir(); //gdg FVector(0.f, 0.f, -1.f);
			const FVector NewDelta = bTriedLedgeMove ? FVector::ZeroVector : GetLedgeMove(OldLocation, Delta, GravDir);
			if (!NewDelta.IsZero())
			{

				// first revert this move
				RevertMove(OldLocation, OldBase, PreviousBaseLocation, OldFloor, false);

				// avoid repeated ledge moves if the first one fails
				bTriedLedgeMove = true;

				// Try new movement direction
				Velocity = NewDelta / timeTick;
				remainingTime += timeTick;
				continue;
			}
			else
			{

				// see if it is OK to jump
				// @todo collision : only thing that can be problem is that oldbase has world collision on
				bool bMustJump = bZeroDelta || (OldBase == NULL || (!OldBase->IsCollisionEnabled() && MovementBaseUtility::IsDynamicBase(OldBase)));
				if ((bMustJump || !bCheckedFall) && CheckFall(CurrentFloor.HitResult, Delta, OldLocation, remainingTime, timeTick, Iterations, bMustJump))
				{
					UE_LOG(LogTemp, Warning, TEXT("phys 1d"));

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
					UE_LOG(LogTemp, Warning, TEXT("phys 1g"));
					CharacterOwner->OnWalkingOffLedge();
					if (IsMovingOnGround())
					{
						UE_LOG(LogTemp, Warning, TEXT("phys 1h"));
						// If still walking, then fall. If not, assume the user set a different mode they want to keep.
						StartFalling(Iterations, remainingTime, timeTick, Delta, OldLocation);
					}
					return;
				}

				AdjustFloorHeight();
				SetBase(CurrentFloor.HitResult.Component.Get(), CurrentFloor.HitResult.BoneName);
			}
			else if (CurrentFloor.HitResult.bStartPenetrating && remainingTime <= 0.f)
			{
				UE_LOG(LogTemp, Warning, TEXT("phys 1i"));
				// The floor check failed because it started in penetration
				// We do not want to try to move downward because the downward sweep failed, rather we'd like to try to pop out of the floor.
				FHitResult Hit(CurrentFloor.HitResult);
				Hit.TraceEnd = Hit.TraceStart + FVector(0.f, 0.f, MAX_FLOOR_DIST);
				const FVector RequestedAdjustment = GetPenetrationAdjustment(Hit);
				ResolvePenetration(RequestedAdjustment, Hit, CharacterOwner->GetActorRotation());
			}

			// check if just entered water
			if (IsSwimming())
			{
				UE_LOG(LogTemp, Warning, TEXT("phys 1j"));
				StartSwimming(OldLocation, Velocity, timeTick, remainingTime, Iterations);
				return;
			}

			// See if we need to start falling.
			if (!CurrentFloor.IsWalkableFloor() && !CurrentFloor.HitResult.bStartPenetrating)
			{
				const bool bMustJump = bJustTeleported || bZeroDelta || (OldBase == NULL || (!OldBase->IsCollisionEnabled() && MovementBaseUtility::IsDynamicBase(OldBase)));
				if ((bMustJump || !bCheckedFall) && CheckFall(CurrentFloor.HitResult, Delta, OldLocation, remainingTime, timeTick, Iterations, bMustJump))
				{
					return;
				}
				bCheckedFall = true;
			}
		}

	//UE_LOG(LogTemp, Warning, TEXT("%d Performing Movement Velocity:%s"),__LINE__, *Velocity.ToString()); //Z non-zero when jumping

		// Allow overlap events and such to change physics state and velocity
		if (IsMovingOnGround())
		{
			//gets here			// Make velocity reflect actual move
			if (!bJustTeleported && !HasRootMotion() && timeTick >= MIN_TICK_TIME)
			{
				Velocity = (CharacterOwner->GetActorLocation() - OldLocation) / timeTick;
			}
		}

		// If we didn't move at all this iteration then abort (since future iterations will also be stuck).
		if (CharacterOwner->GetActorLocation() == OldLocation)
		{
			//gets here		UE_LOG(LogTemp, Warning, TEXT("phys 1m"));
			remainingTime = 0.f;
			break;
		}
	}
	/*
//	This zeroes out Z-axis movement
	if (IsMovingOnGround())
	{
		MaintainHorizontalGroundVelocity();
	}*/
}

void UOrbitCharacterMovementComponent::CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration)
{
	// UE_LOG(LogTemp, Warning, TEXT("ReqCC:%s"), *Acceleration.ToString());//hmmm I thought Acceleration.Z wasn't 0 here but it is 0....
	//	UE_LOG(LogTemp, Warning, TEXT(" CalcVelocity: DeltaTime:%f, Friction:%f, Braking:%f"), DeltaTime, Friction, BrakingDeceleration);
	Friction = 0;
	// BrakingDeceleration = 0;
	// Do not update velocity when using root motion
	if (!HasValidData() || HasRootMotion() || DeltaTime < MIN_TICK_TIME)
	{
		return;
	}

	Friction = FMath::Max(0.f, Friction);
	const float MaxAccel = GetMaxAcceleration();
	float MaxSpeed = GetMaxSpeed();

	// Check if path following requested movement
	bool bZeroRequestedAcceleration = true;
	FVector RequestedAcceleration = FVector::ZeroVector;
	float RequestedSpeed = 0.0f;

	if (ApplyRequestedMove(DeltaTime, MaxAccel, MaxSpeed, Friction, BrakingDeceleration, RequestedAcceleration, RequestedSpeed))
	{
		RequestedAcceleration.ClampMaxSize(MaxAccel);
		bZeroRequestedAcceleration = false;
	}

	if (bForceMaxAccel)
	{
		// Force acceleration at full speed.
		// In consideration order for direction: Acceleration, then Velocity, then Pawn's rotation.
		if (Acceleration.SizeSquared() > SMALL_NUMBER)
		{
			Acceleration = SafeNormalPrecise(Acceleration) * MaxAccel;
		}
		else
		{
			Acceleration = MaxAccel * (Velocity.SizeSquared() < SMALL_NUMBER ? CharacterOwner->GetActorRotation().Vector() : SafeNormalPrecise(Velocity));
		}

		AnalogInputModifier = 1.f;
	}

	// Path following above didn't care about the analog modifier, but we do for everything else below, so get the fully modified value.
	// Use max of requested speed and max speed if we modified the speed in ApplyRequestedMove above.
	MaxSpeed = FMath::Max(RequestedSpeed, MaxSpeed * AnalogInputModifier);

	// Apply braking or deceleration
	const bool bZeroAcceleration = Acceleration.IsZero();
	const bool bVelocityOverMax = IsExceedingMaxSpeed(MaxSpeed);
	// Only apply braking if there is no acceleration, or we are over our max speed and need to slow down to it.
	if ((bZeroAcceleration && bZeroRequestedAcceleration) || bVelocityOverMax)
	{
		const FVector OldVelocity = Velocity;
		ApplyVelocityBraking(DeltaTime, Friction, BrakingDeceleration);

		// Don't allow braking to lower us below max speed if we started above it.
		if (bVelocityOverMax && Velocity.SizeSquared() < FMath::Square(MaxSpeed) && FVector::DotProduct(Acceleration, OldVelocity) > 0.0f)
		{
			Velocity = SafeNormalPrecise(OldVelocity) * MaxSpeed;
		}
	}
	else if (!bZeroAcceleration)
	{
		// Friction affects our ability to change direction. This is only done for input acceleration, not path following.
		//gdg This seems to be the only place SafeNormalPr is really being called
		const FVector AccelDir = SafeNormalPrecise(Acceleration);
		const float VelSize = Velocity.Size();
		Velocity = Velocity - (Velocity - AccelDir * VelSize) * FMath::Min(DeltaTime * Friction, 1.f);
	}

	// Apply fluid friction
	if (bFluid)
	{
		Velocity = Velocity * (1.f - FMath::Min(Friction * DeltaTime, 1.f));
	}

	// Apply acceleration
	const float NewMaxSpeed = (IsExceedingMaxSpeed(MaxSpeed)) ? Velocity.Size() : MaxSpeed;
	Velocity += Acceleration * DeltaTime;
	Velocity += RequestedAcceleration * DeltaTime;
	Velocity = ClampMaxSizePrecise(Velocity, NewMaxSpeed);

	if (bUseRVOAvoidance)
	{
		CalcAvoidanceVelocity(DeltaTime);
	}
}

void UOrbitCharacterMovementComponent::PhysFalling(float deltaTime, int32 Iterations)
{
	if (deltaTime < MIN_TICK_TIME)
	{
		return;
	}

	FVector FallAcceleration = GetFallingLateralAcceleration(deltaTime);
	FallAcceleration.Z = 0.f;
	const bool bHasAirControl = (FallAcceleration.SizeSquared2D() > 0.f);

	float remainingTime = deltaTime;
	while ((remainingTime >= MIN_TICK_TIME) && (Iterations < MaxSimulationIterations))
	{
		Iterations++;
		const float timeTick = GetSimulationTimeStep(remainingTime, Iterations);
		remainingTime -= timeTick;

		const FVector OldLocation = CharacterOwner->GetActorLocation();
		const FRotator PawnRotation = CharacterOwner->GetActorRotation();
		bJustTeleported = false;

		FVector OldVelocity = Velocity;
		FVector VelocityNoAirControl = Velocity;
		// this->GetCharacterOwner()->ClientSetRotation(-GetGravityDir().Rotation() * deltaTime);//gdg attempt sorta but not quite

		// Apply input
		if (!HasRootMotion())
		{
			// Compute VelocityNoAirControl
			if (bHasAirControl)
			{
				// Find velocity *without* acceleration.
				TGuardValue<FVector> RestoreAcceleration(Acceleration, FVector::ZeroVector);
				TGuardValue<FVector> RestoreVelocity(Velocity, Velocity);
				Velocity.Z = 0.f;
				CalcVelocity(timeTick, FallingLateralFriction, false, BrakingDecelerationFalling);
				VelocityNoAirControl = FVector(Velocity.X, Velocity.Y, OldVelocity.Z);
			}

			// Compute Velocity
			{
				// Acceleration = FallAcceleration for CalcVelocity(), but we restore it after using it.
				TGuardValue<FVector> RestoreAcceleration(Acceleration, FallAcceleration);
				Velocity.Z = 0.f;
				CalcVelocity(timeTick, FallingLateralFriction, false, BrakingDecelerationFalling);
				Velocity.Z = OldVelocity.Z;
			}

			// Just copy Velocity to VelocityNoAirControl if they are the same (ie no acceleration).
			if (!bHasAirControl)
			{
				VelocityNoAirControl = Velocity;
			}
		}

		// Apply gravity
		const FVector Gravity = GetGravityV();
		//	this->GetCharacterOwner()->ClientSetRotation( Gravity.Rotation());//gdg attempt sorta but not quite

		Velocity = NewFallVelocity(Velocity, Gravity, timeTick);
		VelocityNoAirControl = NewFallVelocity(VelocityNoAirControl, Gravity, timeTick);
		const FVector AirControlAccel = (Velocity - VelocityNoAirControl) / timeTick;

		if (bNotifyApex && CharacterOwner->Controller && (Velocity.Z <= 0.f))
		{
			// Just passed jump apex since now going down
			bNotifyApex = false;
			NotifyJumpApex();
		}

		// Not really necessary to limit air control since the first deflection uses non air control velocity for deflection!
		/*
		if (bHasAirControl)
		{
		const float LookAheadTime = 0.02f;
		FHitResult TestAirResult(1.f);
		if (FindAirControlImpact(timeTick, LookAheadTime, Velocity, FallAcceleration, Gravity, TestAirResult))
		{
		const FVector LimitedAirAccel = LimitAirControl(timeTick, AirControlAccel, TestAirResult, true);
		Velocity = VelocityNoAirControl + (LimitedAirAccel * timeTick);
		}
		}
		*/


		// Move
		FHitResult Hit(1.f);
		FVector Adjusted = 0.5f*(OldVelocity + Velocity) * timeTick;
		SafeMoveUpdatedComponent(Adjusted, PawnRotation, true, Hit);

		if (!HasValidData())
		{
			return;
		}

		float LastMoveTimeSlice = timeTick;
		float subTimeTickRemaining = timeTick * (1.f - Hit.Time);
		if (IsSwimming()) //just entered water
		{
			remainingTime += subTimeTickRemaining;
			StartSwimming(OldLocation, OldVelocity, timeTick, remainingTime, Iterations);
			return;
		}
		else if (Hit.bBlockingHit)
		{

			if (IsValidLandingSpot(UpdatedComponent->GetComponentLocation(), Hit))
			{
				//		UE_LOG(LogTemp, Warning, TEXT("Got to here3"));

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
					FindFloor(PawnLocation, FloorResult, false);
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
					Velocity = HasRootMotion() ? FVector(Velocity.X, Velocity.Y, NewVelocity.Z) : NewVelocity;
				}

				if (subTimeTickRemaining > KINDA_SMALL_NUMBER && (Delta | Adjusted) > 0.f)
				{
					// Move in deflected direction.
					SafeMoveUpdatedComponent(Delta, PawnRotation, true, Hit);

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
						//gdg no fucking clue where vsnz is defined if (bHasAirControl && Hit.Normal.Z > VERTICAL_SLOPE_NORMAL_Z)
						//{
						const FVector LastMoveNoAirControl = VelocityNoAirControl * LastMoveTimeSlice;
						Delta = ComputeSlideVector(LastMoveNoAirControl, 1.f, OldHitNormal, Hit);
						//}

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
							Velocity = HasRootMotion() ? FVector(Velocity.X, Velocity.Y, NewVelocity.Z) : NewVelocity;
						}

						// bDitch=true means that pawn is straddling two slopes, neither of which he can stand on
						bool bDitch = ((OldHitImpactNormal.Z > 0.f) && (Hit.ImpactNormal.Z > 0.f) && (FMath::Abs(Delta.Z) <= KINDA_SMALL_NUMBER) && ((Hit.ImpactNormal | OldHitImpactNormal) < 0.f));
						SafeMoveUpdatedComponent(Delta, PawnRotation, true, Hit);
						if (Hit.Time == 0)
						{
							// if we are stuck then try to side step
							FVector SideDelta = (OldHitNormal + Hit.ImpactNormal).SafeNormal2D();
							if (SideDelta.IsNearlyZero())
							{
								SideDelta = FVector(OldHitNormal.Y, -OldHitNormal.X, 0).SafeNormal();
							}
							SafeMoveUpdatedComponent(SideDelta, PawnRotation, true, Hit);
						}

						if (bDitch || IsValidLandingSpot(UpdatedComponent->GetComponentLocation(), Hit) || Hit.Time == 0)
						{
							remainingTime = 0.f;
							ProcessLanded(Hit, remainingTime, Iterations);
							return;
						}
						else if (GetPerchRadiusThreshold() > 0.f && Hit.Time == 1.f && OldHitImpactNormal.Z >= GetWalkableFloorZ())
						{
							UE_LOG(LogTemp, Warning, TEXT("Perch Stuff"));
							// We might be in a virtual 'ditch' within our perch radius. This is rare.
							const FVector PawnLocation = CharacterOwner->GetActorLocation();
							const float ZMovedDist = FMath::Abs(PawnLocation.Z - OldLocation.Z);
							const float MovedDist2DSq = (PawnLocation - OldLocation).SizeSquared2D();
							if (ZMovedDist <= 0.2f * timeTick && MovedDist2DSq <= 4.f * timeTick)
							{
								Velocity.X += 0.25f * GetMaxSpeed() * (FMath::FRand() - 0.5f);
								Velocity.Y += 0.25f * GetMaxSpeed() * (FMath::FRand() - 0.5f);
								Velocity.Z = FMath::Max<float>(JumpZVelocity * 0.25f, 1.f);
								Delta = Velocity * timeTick;
								SafeMoveUpdatedComponent(Delta, PawnRotation, true, Hit);
							}
						}
					}
				}
			}
		}

		if (Velocity.SizeSquared2D() <= KINDA_SMALL_NUMBER * 10.f)
		{
			Velocity.X = 0.f;
			Velocity.Y = 0.f;
		}
	}
}

bool UOrbitCharacterMovementComponent::IsValidLandingSpot(const FVector& CapsuleLocation, const FHitResult& Hit) const
{
	return true;//FIXME this is temporary
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
		if (Hit.ImpactPoint.Z >= LowerHemisphereZ)
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
void UOrbitCharacterMovementComponent::PhysSwimming(float deltaTime, int32 Iterations)
{
	if (deltaTime < MIN_TICK_TIME)
	{
		return;
	}

	float NetFluidFriction = 0.f;
	float Depth = ImmersionDepth();
	float NetBuoyancy = Buoyancy * Depth;
	if (!HasRootMotion() && (Velocity.Z > 0.5f*GetMaxSpeed()) && (NetBuoyancy != 0.f))
	{
		//damp positive Z out of water
		Velocity.Z = Velocity.Z * Depth;
	}
	Iterations++;
	FVector OldLocation = CharacterOwner->GetActorLocation();
	bJustTeleported = false;
	if (!HasRootMotion())
	{
		const float Friction = 0.5f * GetPhysicsVolume()->FluidFriction * Depth;
		CalcVelocity(deltaTime, Friction, true, BrakingDecelerationSwimming);
		// orig		Velocity.Z += GetGravityZ() * deltaTime * (1.f - NetBuoyancy);
		Velocity += GetGravityV() * deltaTime * (1.f - NetBuoyancy);
	}

	FVector Adjusted = Velocity * deltaTime;
	FHitResult Hit(1.f);
	float remainingTime = deltaTime * Swim(Adjusted, Hit);

	//may have left water - if so, script might have set new physics mode
	if (!IsSwimming())
	{
		StartNewPhysics(remainingTime, Iterations);
		return;
	}

	if (Hit.Time < 1.f && CharacterOwner)
	{
		const FVector GravDir = GetGravityDir(); // FVector(0.f, 0.f, -1.f);
		const FVector VelDir = Velocity.SafeNormal();
		const float UpDown = GravDir | VelDir;

		bool bSteppedUp = false;
		if ((FMath::Abs(Hit.ImpactNormal.Z) < 0.2f) && (UpDown < 0.5f) && (UpDown > -0.2f) && CanStepUp(Hit))
		{
			float stepZ = CharacterOwner->GetActorLocation().Z;
			const FVector RealVelocity = Velocity;
			Velocity.Z = 1.f;	// HACK: since will be moving up, in case pawn leaves the water
			bSteppedUp = StepUp(GravDir, Adjusted * (1.f - Hit.Time), Hit);
			if (bSteppedUp)
			{
				//may have left water - if so, script might have set new physics mode
				if (!IsSwimming())
				{
					StartNewPhysics(remainingTime, Iterations);
					return;
				}
				OldLocation.Z = CharacterOwner->GetActorLocation().Z + (OldLocation.Z - stepZ);
			}
			Velocity = RealVelocity;
		}

		if (!bSteppedUp)
		{
			//adjust and try again
			HandleImpact(Hit, deltaTime, Adjusted);
			SlideAlongSurface(Adjusted, (1.f - Hit.Time), Hit.Normal, Hit, true);
		}
	}

	if (!HasRootMotion() && !bJustTeleported && ((deltaTime - remainingTime) > KINDA_SMALL_NUMBER) && CharacterOwner)
	{
		bool bWaterJump = !GetPhysicsVolume()->bWaterVolume;
		float velZ = Velocity.Z;
		Velocity = (CharacterOwner->GetActorLocation() - OldLocation) / (deltaTime - remainingTime);
		if (bWaterJump)
		{
			Velocity.Z = velZ;
		}
	}

	if (!GetPhysicsVolume()->bWaterVolume && IsSwimming())
	{
		SetMovementMode(MOVE_Falling); //in case script didn't change it (w/ zone change)
	}

	//may have left water - if so, script might have set new physics mode
	if (!IsSwimming())
	{
		StartNewPhysics(remainingTime, Iterations);
	}
}

//Tick Comp
void UOrbitCharacterMovementComponent::TickComponent(
	float DeltaTime,
enum ELevelTick TickType,
	FActorComponentTickFunction *ThisTickFunction
	){
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	//	GEngine->AddOnScreenDebugMessage(-1, 2.f, FColor::Green, (TEXT("HIT ")));

	//UE_LOG //custom comp is ticking!!!
	//UE_LOG(LogTemp, Warning, TEXT("!!!!!!!!!!!!!!!!!!!!!!!!! TICK COMPONENT"));

	//begin

	/* Get some sort of gimble lock at north pole
	if (OldGravDir == FVector(0, 0, 0)) OldGravDir = GetGravityDir();
	FVector CurrGravDir = GetGravityDir();
	FQuat q1 = CurrGravDir.Rotation().Quaternion();
	FQuat q2 = OldGravDir.Rotation().Quaternion();
	//FQuat NewQuat = q2 * q1 * q2.Inverse();
	FQuat NewQuat = q1 - q2 ;
		GetCharacterOwner()->AddActorLocalRotation(NewQuat.Rotator());
	OldGravDir = CurrGravDir;
	*/
	if (OldGravDir == FVector(0, 0, 0)) OldGravDir = GetGravityDir();
	FVector CurrGravDir = GetGravityDir();
	FRotator RealRot = GetCharacterOwner()->GetControlRotation() - OldGravDir.Rotation();
	FRotator NewRot = CurrGravDir.Rotation() - OldGravDir.Rotation();
	if (NewRot.ContainsNaN()){
		UE_LOG(LogTemp, Warning, TEXT("SetRotTo NaN Yo Business"));
	} else{
		if (NewRot.Pitch < 0.0) NewRot.Pitch += 360.0;
		if (NewRot.Yaw < 0.0) NewRot.Yaw += 360.0;
		if (NewRot.Roll < 0.0) NewRot.Roll += 360.0;
		//GetCharacterOwner()->AddActorLocalRotation(NewRot);
		GetCharacterOwner()->AddActorWorldRotation(NewRot);
	}
	OldGravDir = CurrGravDir;
	/*
	*/
	//this->GetCharacterOwner()->ClientSetRotation(-GetGravityDir().Rotation() * DeltaTime);//gdg attempt sorta but not quite
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
		if (CharacterOwner->IsLocallyControlled() || bRunPhysicsWithNoController || (!CharacterOwner->Controller && CharacterOwner->IsPlayingRootMotion()))
		{
			// We need to check the jump state before adjusting input acceleration, to minimize latency
			// and to make sure acceleration respects our potentially new falling state.
			CharacterOwner->CheckJumpInput(DeltaTime);

			// apply input to acceleration
			Acceleration = ScaleInputAcceleration(ConstrainInputAcceleration(InputVector));
			AnalogInputModifier = ComputeAnalogInputModifier();

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
		if (CurrentFloor.HitResult.IsValidBlockingHit())
		{
			// Apply downwards force when walking on top of physics objects
			if (UPrimitiveComponent* BaseComp = CurrentFloor.HitResult.GetComponent())
			{
				if (StandingDownwardForceScale != 0.f && BaseComp->IsAnySimulatingPhysics())
				{
					UE_LOG(LogTemp, Warning, TEXT("Applying Downforce"));
					const FVector ForceLocation = CurrentFloor.HitResult.ImpactPoint;
					BaseComp->AddForceAtLocation(GetGravityV() * Mass * StandingDownwardForceScale, ForceLocation, CurrentFloor.HitResult.BoneName);
					//BaseComp->AddForceAtLocation(GetGravityV(), ForceLocation, CurrentFloor.HitResult.BoneName);

				}
			}
		}

		ApplyRepulsionForce(DeltaTime);
	}

}

bool UOrbitCharacterMovementComponent::FindAirControlImpact(float DeltaTime, float AdditionalTime, const FVector& FallVelocity, const FVector& FallAcceleration, const FVector& Gravity, FHitResult& OutHitResult)
{
	// Test for slope to avoid using air control to climb walls.
	FVector TestWalk = Velocity * DeltaTime;
	if (AdditionalTime > 0.f)
	{
		const FVector PostGravityVelocity = NewFallVelocity(FallVelocity, Gravity, AdditionalTime);
		TestWalk += ((FallAcceleration * AdditionalTime) + PostGravityVelocity) * AdditionalTime;
	}

	if (!TestWalk.IsZero())
	{
		static const FName FallingTraceParamsTag = FName(TEXT("PhysFalling"));
		FCollisionQueryParams CapsuleQuery(FallingTraceParamsTag, false, CharacterOwner);
		FCollisionResponseParams ResponseParam;
		InitCollisionParams(CapsuleQuery, ResponseParam);
		const FVector CapsuleLocation = UpdatedComponent->GetComponentLocation();
		const FCollisionShape CapsuleShape = GetPawnCapsuleCollisionShape(SHRINK_None);

		if (GetWorld()->SweepSingle(OutHitResult, CapsuleLocation, CapsuleLocation + TestWalk, FQuat::Identity, UpdatedComponent->GetCollisionObjectType(), CapsuleShape, CapsuleQuery, ResponseParam))
		{
			return true;
		}
	}

	return false;
}

bool UOrbitCharacterMovementComponent::CheckLedgeDirection(const FVector& OldLocation, const FVector& SideStep, const FVector& GravDir)
{
	FVector SideDest = OldLocation + SideStep;
	static const FName CheckLedgeDirectionName(TEXT("CheckLedgeDirection"));
	FCollisionQueryParams CapsuleParams(CheckLedgeDirectionName, false, CharacterOwner);
	FCollisionResponseParams ResponseParam;
	InitCollisionParams(CapsuleParams, ResponseParam);
	FCollisionShape CapsuleShape = GetPawnCapsuleCollisionShape(SHRINK_None);
	const ECollisionChannel CollisionChannel = UpdatedComponent->GetCollisionObjectType();
	FHitResult Result(1.f);
	GetWorld()->SweepSingle(Result, OldLocation, SideDest, FQuat::Identity, CollisionChannel, CapsuleShape, CapsuleParams, ResponseParam);

	if (!Result.bBlockingHit || IsWalkable(Result))
	{
		if (!Result.bBlockingHit)
		{
			GetWorld()->SweepSingle(Result, SideDest, SideDest + GravDir * (MaxStepHeight + LedgeCheckThreshold), FQuat::Identity, CollisionChannel, CapsuleShape, CapsuleParams, ResponseParam);
		}
		if ((Result.Time < 1.f) && IsWalkable(Result))
		{
			return true;
		}
	}
	return false;
}

FVector UOrbitCharacterMovementComponent::GetLedgeMove(const FVector& OldLocation, const FVector& Delta, const FVector& GravDir)
{
	// We have a ledge!
	if (!HasValidData())
	{
		return FVector::ZeroVector;
	}

	// check which direction ledge goes
	float DesiredDistSq = Delta.SizeSquared();

	if (DesiredDistSq > 0.f)
	{
		//orig		FVector SideDir(Delta.Y, -1.f * Delta.X, 0.f);
		FVector SideDir(Delta.Y, -1.f * Delta.X, Delta.Z);
		//TODO probably need to check the other directions

		// try left
		if (CheckLedgeDirection(OldLocation, SideDir, GravDir))
		{
			return SideDir;
		}

		// try right
		SideDir *= -1.f;
		if (CheckLedgeDirection(OldLocation, SideDir, GravDir))
		{
			return SideDir;
		}
	}
	return FVector::ZeroVector;
}

FVector UOrbitCharacterMovementComponent::ComputeGroundMovementDelta(const FVector& Delta, const FHitResult& RampHit, const bool bHitFromLineTrace) const
{
	//return FVector(1, 1, 1);//will head in 1 directon no matter what key pressed
	const FVector FloorNormal = RampHit.ImpactNormal;
	//const FVector FloorNormal = FQuat::FindBetween(RampHit.ImpactNormal, -GetGravityDir()).GetRotationAxis() ;
	//seems i'll need soemthing likke that for stair/ramp climbing
	const FVector ContactNormal = RampHit.Normal;

	if (
		FloorNormal.Z < (1.f - KINDA_SMALL_NUMBER) 
		&& FloorNormal.Z > KINDA_SMALL_NUMBER 
		&& ContactNormal.Z > KINDA_SMALL_NUMBER 
		&& !bHitFromLineTrace && IsWalkable(RampHit))
	{
	UE_LOG(LogTemp, Warning, TEXT(" @ Floor shit @"));
	//get here until hit weirdness slope
	//UE_LOG(LogTemp, Warning, TEXT(" @ Floor shit @"));
	// Compute a vector that moves parallel to the surface, by projecting the horizontal movement direction onto the ramp.
	const float FloorDotDelta = (FloorNormal | Delta);
	FVector RampMovement(Delta.X, Delta.Y, -FloorDotDelta / FloorNormal.Z);
	//FVector RampMovement(11.f,11.f, -11.f);//can very clumsily walk around world through poles with this

	//UE_LOG(LogTemp, Warning, TEXT(" @ Ramp Vec %s"), *RampMovement.ToString());

	if (bMaintainHorizontalGroundVelocity)
	{
		//UE_LOG(LogTemp, Warning, TEXT(" @ floor b1 @"));
		return RampMovement;
	}
	else
	{
		//get here UE_LOG(LogTemp, Warning, TEXT(" @ floor b2 @"));
		return RampMovement.SafeNormal() * Delta.Size();
	}
		}

	return Delta;
}

bool UOrbitCharacterMovementComponent::IsWalkable(const FHitResult& Hit) const
{
	return true; //temporary hack gdg
	if (!Hit.IsValidBlockingHit())
	{
		// No hit, or starting in penetration
		return false;
	}
	/*
	// Never walk up vertical surfaces.
	//bah humbug
	if (Hit.ImpactNormal.Z < KINDA_SMALL_NUMBER)
	{
	return false;
	}*/

	float TestWalkableZ = WalkableFloorZ;

	// See if this component overrides the walkable floor z.
	const UPrimitiveComponent* HitComponent = Hit.Component.Get();
	if (HitComponent)
	{
		const FWalkableSlopeOverride& SlopeOverride = HitComponent->GetWalkableSlopeOverride();
		TestWalkableZ = SlopeOverride.ModifyWalkableFloorZ(TestWalkableZ);
	}
	
	// Can't walk on this surface if it is too steep.
	if (Hit.ImpactNormal.Z < TestWalkableZ)
	{
	return false;
	}
	return true;
}

void UOrbitCharacterMovementComponent::SetWalkableFloorAngle(float InWalkableFloorAngle)
{
//	WalkableFloorAngle = FMath::Clamp(InWalkableFloorAngle, -90.f, 90.0f);
	WalkableFloorAngle = FMath::Clamp(InWalkableFloorAngle, 0.f, 90.0f);
	//WalkableFloorAngle = InWalkableFloorAngle; //may be dangerous
	WalkableFloorZ = FMath::Cos(FMath::DegreesToRadians(WalkableFloorAngle));
}

void UOrbitCharacterMovementComponent::SetWalkableFloorZ(float InWalkableFloorZ)
{
	//no nipple clamps 
	//	WalkableFloorZ = InWalkableFloorZ;
	//WalkableFloorZ = FMath::Clamp(InWalkableFloorZ, -1.f, 1.f);
	WalkableFloorZ = FMath::Clamp(InWalkableFloorZ, 0.f, 1.f);
	WalkableFloorAngle = FMath::RadiansToDegrees(FMath::Acos(WalkableFloorZ));
}

void UOrbitCharacterMovementComponent::MoveAlongFloor(const FVector& InVelocity, float DeltaSeconds, FStepDownResult* OutStepDownResult)
{
	//const FVector Delta = FVector(InVelocity.X, InVelocity.Y, 0.f) * DeltaSeconds;
	// const FVector Delta = FVector(InVelocity.X, InVelocity.Y, InVelocity.Z) * DeltaSeconds;
	const FVector Delta = InVelocity * DeltaSeconds;//gdg Delta.Z is always 0
//	 UE_LOG(LogTemp, Warning, TEXT(" @ MOVE ALONG FLOOR Delta:%s"), *Delta.ToString());

	/* CurrentFloor is a stuct with data and methods, a class, partially defined in this file and the header
	AdjustFloorHeight , UpdateFloorFromAdjustment, StepUp, (bForceNextFloorCheck), ComputeFloorDist

	if (!CurrentFloor.IsWalkableFloor())
	{
	UE_LOG(LogTemp, Warning, TEXT("Floor Became unwalkable"));
	return;
	}
	*/

	// Move along the current floor
	FHitResult Hit(1.f);

	FVector RampVector = ComputeGroundMovementDelta(Delta, CurrentFloor.HitResult, CurrentFloor.bLineTrace);//RampVector.Z can be non zero
	 //UE_LOG(LogTemp, Warning, TEXT(" @ MOVE ALONG FLOOR RampVec:%s"), *RampVector.ToString());
	// RampVector=FVector(360, 360, 360); has intersting effect of forward motion causing a flying leap
	//	RampVector = FVector(3, 3, -3); //more reasonable speed
	SafeMoveUpdatedComponent(RampVector, CharacterOwner->GetActorRotation(), true, Hit);
	
	//looks like ., MovementComponent::SafeMoveUpdatedComponent => MoveUpdatedComponent, PrimativeComponent::MoveComponent, 
	//SceneComponent::SetInternalLocationAndRot. are using Quaternions... Where the fuck is gimble lock occuring?
		/* This would go in the Pawn Class... should it override AddActorRotation?
	//CharacterOwner->GetTra
		see:https://answers.unrealengine.com/questions/36110/rotate-a-pawn-in-full-360-degrees.html
		FORCEINLINE void AddToActorRotation(AActor* TheActor, const FRotator& AddRot) const
	{
		if (!TheActor) return;

		FTransform TheTransform = TheActor->GetTransform();
		TheTransform.ConcatenateRotation(AddRot.Quaternion());
		TheTransform.NormalizeRotation();
		TheActor->SetActorTransform(TheTransform);
	}*/
	float LastMoveTimeSlice = DeltaSeconds;

	if (Hit.bStartPenetrating)
	{
		UE_LOG(LogTemp, Warning, TEXT(" @ Penetration @"));

		OnCharacterStuckInGeometry();
	}
	// Commenting out this entire chunck doesn't prevent getting glued in
	if (Hit.IsValidBlockingHit())
	{
		UE_LOG(LogTemp, Warning, TEXT(" Blocking Hit"));

		// We impacted something (most likely another ramp, but possibly a barrier).
		float PercentTimeApplied = Hit.Time;
		if ((Hit.Time > 0.f) && (Hit.Normal.Z > KINDA_SMALL_NUMBER) && IsWalkable(Hit))
		{
			UE_LOG(LogTemp, Warning, TEXT(" Another walkable ramp"));

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
			UE_LOG(LogTemp, Warning, TEXT(" Blocking Hit Again "));

			if (CanStepUp(Hit) || (CharacterOwner->GetMovementBase() != NULL && CharacterOwner->GetMovementBase()->GetOwner() == Hit.GetActor()))
			{
				UE_LOG(LogTemp, Warning, TEXT(" Hit Barrier "));

				// hit a barrier, try to step up
				// const FVector GravDir(0.f, 0.f, -1.f);
				//denegated 2014-12-10
				const FVector GravDir = GetGravityDir(); //negative will make player bounce to safety rather than get stuck

				if (!StepUp(GravDir, Delta * (1.f - PercentTimeApplied), Hit, OutStepDownResult))
				{
					UE_LOG(LogTemp, Warning, TEXT("- StepUp (ImpactNormal %s, Normal %s"), *Hit.ImpactNormal.ToString(), *Hit.Normal.ToString());
					HandleImpact(Hit, LastMoveTimeSlice, RampVector);
					SlideAlongSurface(Delta, 1.f - PercentTimeApplied, Hit.Normal, Hit, true);
				}
				else
				{
					// Don't recalculate velocity based on this height adjustment, if considering vertical adjustments.
					UE_LOG(LogTemp, Warning, TEXT("+ StepUp (ImpactNormal %s, Normal %s"), *Hit.ImpactNormal.ToString(), *Hit.Normal.ToString());
					bJustTeleported |= !bMaintainHorizontalGroundVelocity;
					// Got here walking down sphere
					//LogTemp:Warning: + StepUp (ImpactNormal X=0.856 Y=-0.122 Z=0.502, Normal X=0.856 Y=-0.122 Z=0.502

				}

			}
			else if (Hit.Component.IsValid() && !Hit.Component.Get()->CanCharacterStepUp(CharacterOwner))
			{
				UE_LOG(LogTemp, Warning, TEXT(" Uhm, not sure"));

				HandleImpact(Hit, LastMoveTimeSlice, RampVector);
				SlideAlongSurface(Delta, 1.f - PercentTimeApplied, Hit.Normal, Hit, true);
			}
		}
	}

}

void UOrbitCharacterMovementComponent::SimulateMovement(float DeltaSeconds)
{
	if (!HasValidData() || UpdatedComponent->Mobility != EComponentMobility::Movable || UpdatedComponent->IsSimulatingPhysics())
	{
		return;
	}

	const bool bIsSimulatedProxy = (CharacterOwner->Role == ROLE_SimulatedProxy);

	// Workaround for replication not being updated initially
	if (bIsSimulatedProxy &&
		CharacterOwner->ReplicatedMovement.Location.IsZero() &&
		CharacterOwner->ReplicatedMovement.Rotation.IsZero() &&
		CharacterOwner->ReplicatedMovement.LinearVelocity.IsZero())
	{
		return;
	}

	// If base is not resolved on the client, we should not try to simulate at all
	if (CharacterOwner->GetBasedMovement().IsBaseUnresolved())
	{
		//UE_LOG(LogTemp, Verbose, TEXT("Base for simulated character '%s' is not resolved on client, skipping SimulateMovement"), *CharacterOwner->GetName());
		return;
	}

	FVector OldVelocity;
	FVector OldLocation;

	// Scoped updates can improve performance of multiple MoveComponent calls.
	{
		FScopedMovementUpdate ScopedMovementUpdate(UpdatedComponent, bEnableScopedMovementUpdates ? EScopedUpdate::DeferredUpdates : EScopedUpdate::ImmediateUpdates);

		if (bIsSimulatedProxy)
		{
			// Handle network changes
			if (bNetworkUpdateReceived)
			{
				bNetworkUpdateReceived = false;
				if (bNetworkMovementModeChanged)
				{
					bNetworkMovementModeChanged = false;
					ApplyNetworkMovementMode(CharacterOwner->GetReplicatedMovementMode());
				}
				else if (bJustTeleported)
				{
					// Make sure floor is current. We will continue using the replicated base, if there was one.
					bJustTeleported = false;
					UpdateFloorFromAdjustment();
				}
			}

			HandlePendingLaunch();
		}

		if (MovementMode == MOVE_None)
		{
			return;
		}

		Acceleration = Velocity.SafeNormal();	// Not currently used for simulated movement
		AnalogInputModifier = 1.0f;				// Not currently used for simulated movement

		MaybeUpdateBasedMovement(DeltaSeconds);

		// simulated pawns predict location
		OldVelocity = Velocity;
		OldLocation = UpdatedComponent->GetComponentLocation();
		FStepDownResult StepDownResult;
		MoveSmooth(Velocity, DeltaSeconds, &StepDownResult);

		// consume path following requested velocity
		bHasRequestedVelocity = false;

		// if simulated gravity, find floor and check if falling
		const bool bEnableFloorCheck = (!CharacterOwner->bSimGravityDisabled || !bIsSimulatedProxy);
		if (bEnableFloorCheck && (MovementMode == MOVE_Walking || MovementMode == MOVE_Falling))
		{
			const FVector CollisionCenter = UpdatedComponent->GetComponentLocation();
			if (StepDownResult.bComputedFloor)
			{
				CurrentFloor = StepDownResult.FloorResult;
			}
			else if (Velocity.Z <= 0.f)
			{
				FindFloor(CollisionCenter, CurrentFloor, Velocity.IsZero(), NULL);
			}
			else
			{
				CurrentFloor.Clear();
			}

			if (!CurrentFloor.IsWalkableFloor())
			{
				// No floor, must fall.
				Velocity = NewFallVelocity(Velocity, GetGravityV(), DeltaSeconds);
				SetMovementMode(MOVE_Falling);
			}
			else
			{
				// Walkable floor
				if (MovementMode == MOVE_Walking)
				{
					AdjustFloorHeight();
					SetBase(CurrentFloor.HitResult.Component.Get(), CurrentFloor.HitResult.BoneName);
				}
				else if (MovementMode == MOVE_Falling)
				{
					if (CurrentFloor.FloorDist <= MIN_FLOOR_DIST)
					{
						// Landed
						SetMovementMode(MOVE_Walking);
					}
					else
					{
						// Continue falling.
						Velocity = NewFallVelocity(Velocity, GetGravityV(), DeltaSeconds);
						CurrentFloor.Clear();
					}
				}
			}
		}

		OnMovementUpdated(DeltaSeconds, OldLocation, OldVelocity);
	} // End scoped movement update

	// Call custom post-movement events. These happen after the scoped movement completes in case the events want to use the current state of overlaps etc.
	CallMovementUpdateDelegate(DeltaSeconds, OldLocation, OldVelocity);

	SaveBaseLocation();
	UpdateComponentVelocity();
	bJustTeleported = false;

	LastUpdateLocation = UpdatedComponent ? UpdatedComponent->GetComponentLocation() : FVector::ZeroVector;
}

//haven't gotten here yet
void UOrbitCharacterMovementComponent::MoveSmooth(const FVector& InVelocity, const float DeltaSeconds, FStepDownResult* OutStepDownResult)
{
	UE_LOG(LogTemp, Warning, TEXT(" @ Smooth Move Exlax @"));

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
					if (FMath::Abs(Hit.ImpactNormal.Z) < 0.2f)
					{
						const FVector GravDir = GetGravityDir();
						const FVector DesiredDir = Delta.SafeNormal();
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

void UOrbitCharacterMovementComponent::PerformAirControlForPathFollowing(FVector Direction, float ZDiff)
{
	// use air control if low grav or above destination and falling towards it
	if (CharacterOwner && Velocity.Z < 0.f && (ZDiff < 0.f || GetGravityZ() > 0.9f * GetWorld()->GetDefaultGravityZ()))
	{
		if (ZDiff > 0.f)
		{
			if (ZDiff > 2.f * GetMaxJumpHeight())
			{
				if (PathFollowingComp.IsValid())
				{
					// PathFollowingComp->AbortMove(TEXT("missed jump"));
				}
			}
		}
		else
		{
			if ((Velocity.X == 0.f) && (Velocity.Y == 0.f))
			{
				Acceleration = FVector::ZeroVector;
			}
			else
			{
				float Dist2D = Direction.Size2D();
				//Direction.Z = 0.f;
				Acceleration = Direction.SafeNormal() * GetMaxAcceleration();

				if ((Dist2D < 0.5f * FMath::Abs(Direction.Z)) && ((Velocity | Direction) > 0.5f*FMath::Square(Dist2D)))
				{
					Acceleration *= -1.f;
				}

				if (Dist2D < 1.5f*CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius())
				{
					Velocity.X = 0.f;
					Velocity.Y = 0.f;
					Acceleration = FVector::ZeroVector;
				}
				else if ((Velocity | Direction) < 0.f)
				{
					float M = FMath::Max(0.f, 0.2f - GetWorld()->DeltaTimeSeconds);
					Velocity.X *= M;
					Velocity.Y *= M;
				}
			}
		}
	}
}
FVector UOrbitCharacterMovementComponent::NewFallVelocity(const FVector& InitialVelocity, const FVector& Gravity, float DeltaTime) const
{
	FVector Result = InitialVelocity;

	if (!Gravity.IsZero())
	{
		// Apply gravity.
		Result += Gravity * DeltaTime;

		const FVector GravityDir = Gravity.SafeNormal();
		const float TerminalLimit = FMath::Abs(GetPhysicsVolume()->TerminalVelocity);

		// Don't exceed terminal velocity.
		if ((Result | GravityDir) > TerminalLimit)
		{
			Result = FVector::PointPlaneProject(Result, FVector::ZeroVector, GravityDir) + GravityDir * TerminalLimit;
		}
	}

	return Result;
}

void UOrbitCharacterMovementComponent::HandleImpact(FHitResult const& Impact, float TimeSlice, const FVector& MoveDelta)
{
	if (CharacterOwner)
	{
		CharacterOwner->MoveBlockedBy(Impact);
	}

	if (PathFollowingComp.IsValid())
	{	// Also notify path following!
		//		PathFollowingComp->OnMoveBlockedBy(Impact);
	}

	APawn* OtherPawn = Cast<APawn>(Impact.GetActor());
	if (OtherPawn)
	{
		NotifyBumpedPawn(OtherPawn);
	}

	if (bEnablePhysicsInteraction)
	{
		const FVector ForceAccel = Acceleration + (IsFalling() ? GetGravityV() : FVector::ZeroVector);
		ApplyImpactPhysicsForces(Impact, ForceAccel, Velocity);
	}
}

void UOrbitCharacterMovementComponent::PerformMovement(float DeltaSeconds)
{
	//won't compile with next line. something about threads
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

		// Character::LaunchCharacter() has been deferred until now.
		HandlePendingLaunch();

		// If using RootMotion, tick animations before running physics.
		if (!CharacterOwner->bClientUpdating && CharacterOwner->IsPlayingRootMotion() && CharacterOwner->GetMesh())
		{
			TickCharacterPose(DeltaSeconds);

			// Make sure animation didn't trigger an event that destroyed us
			if (!HasValidData())
			{
				return;
			}

			// For local human clients, save off root motion data so it can be used by movement networking code.
			if (CharacterOwner->IsLocallyControlled() && (CharacterOwner->Role == ROLE_AutonomousProxy) && CharacterOwner->IsPlayingNetworkedRootMotionMontage())
			{
				CharacterOwner->ClientRootMotionParams = RootMotionParams;
			}
		}

		// if we're about to use root motion, convert it to world space first.
		if (HasRootMotion())
		{
			USkeletalMeshComponent * SkelMeshComp = CharacterOwner->GetMesh();
			if (SkelMeshComp)
			{
				// Convert Local Space Root Motion to world space. Do it right before used by physics to make sure we use up to date transforms, as translation is relative to rotation.
				RootMotionParams.Set(SkelMeshComp->ConvertLocalRootMotionToWorld(RootMotionParams.RootMotionTransform));
				UE_LOG(LogRootMotion, Log, TEXT("PerformMovement WorldSpaceRootMotion Translation: %s, Rotation: %s, Actor Facing: %s"),
					*RootMotionParams.RootMotionTransform.GetTranslation().ToCompactString(), *RootMotionParams.RootMotionTransform.GetRotation().Rotator().ToCompactString(), *CharacterOwner->GetActorRotation().Vector().ToCompactString());
			}

			// Then turn root motion to velocity to be used by various physics modes.
			if (DeltaSeconds > 0.f)
			{
				const FVector RootMotionVelocity = RootMotionParams.RootMotionTransform.GetTranslation() / DeltaSeconds;
				// Do not override Velocity.Z if in falling physics, we want to keep the effect of gravity.
				//Velocity = FVector(RootMotionVelocity.X, RootMotionVelocity.Y, (MovementMode == MOVE_Falling ? Velocity.Z : RootMotionVelocity.Z));
				Velocity = RootMotionVelocity;
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
	//do get here
		if (HasRootMotion())
		{
				//currently not getting here
			const FRotator OldActorRotation = CharacterOwner->GetActorRotation();
			const FRotator RootMotionRotation = RootMotionParams.RootMotionTransform.GetRotation().Rotator();
			if (!RootMotionRotation.IsNearlyZero())
			{
				//currently not getting here
				const FRotator NewActorRotation = (OldActorRotation + RootMotionRotation).GetNormalized();
				MoveUpdatedComponent(FVector::ZeroVector, NewActorRotation, true);
			}

			// debug
			if (false)
			{
				const FVector ResultingLocation = CharacterOwner->GetActorLocation();
				const FRotator ResultingRotation = CharacterOwner->GetActorRotation();

				// Show current position
				DrawDebugCoordinateSystem(GetWorld(), CharacterOwner->GetMesh()->GetComponentLocation() + FVector(0, 0, 1), ResultingRotation, 50.f, false);

				// Show resulting delta move.
				DrawDebugLine(GetWorld(), OldLocation, ResultingLocation, FColor::Red, true, 10.f);

				// Log details.
				UE_LOG(LogRootMotion, Warning, TEXT("PerformMovement Resulting DeltaMove Translation: %s, Rotation: %s, MovementBase: %s"),
					*(ResultingLocation - OldLocation).ToCompactString(), *(ResultingRotation - OldActorRotation).GetNormalized().ToCompactString(), *GetNameSafe(CharacterOwner->GetMovementBase()));

				const FVector RMTranslation = RootMotionParams.RootMotionTransform.GetTranslation();
				const FRotator RMRotation = RootMotionParams.RootMotionTransform.GetRotation().Rotator();
				UE_LOG(LogRootMotion, Warning, TEXT("PerformMovement Resulting DeltaError Translation: %s, Rotation: %s"),
					*(ResultingLocation - OldLocation - RMTranslation).ToCompactString(), *(ResultingRotation - OldActorRotation - RMRotation).GetNormalized().ToCompactString());
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

void UOrbitCharacterMovementComponent::ApplyAccumulatedForces(float DeltaSeconds)
{
	/*
	It looks like this function isn't relevent ATM
	Commenting out next chunk had no apparent effect
*/
	if (PendingImpulseToApply.Z != 0.f || PendingForceToApply.Z != 0.f)
	{
		// check to see if applied momentum is enough to overcome gravity
		if ( IsMovingOnGround() && (PendingImpulseToApply.Z + (PendingForceToApply.Z * DeltaSeconds) + (GetGravityZ() * DeltaSeconds) > SMALL_NUMBER))
		{
			SetMovementMode(MOVE_Falling);
		}
	}
	Velocity += PendingImpulseToApply + (PendingForceToApply * DeltaSeconds);
	//UE_LOG(LogTemp, Warning, TEXT("Pending Impulse : %s, PendingForce:%s"), *PendingImpulseToApply.ToString(), *PendingForceToApply.ToString());
	//UE_LOG(LogTemp, Warning, TEXT("Vel plus Pending Impulse  PendingForce:%s"), *Velocity.ToString()); Z non-zero when jumping

	PendingImpulseToApply = FVector::ZeroVector;
	PendingForceToApply = FVector::ZeroVector;
}

void UOrbitCharacterMovementComponent::MaintainHorizontalGroundVelocity()
{
	//The original version of this basically zeroed out Z-axis motion. This version almost zeros out
	//motion along the line between the player and the center of mass.
	//G can be >80 when landing, about 4 when walking. Will need to tweak the constant later.
	// Confirmed this works. If G >= 0.01 then when G is recaled will be about 0.001 after adjustment
	// There can be some weird bouncing after landing, but, I think it is from StepUp() firing
	FVector G = Velocity.ProjectOnTo(GetGravityDir());
	if (G.Size() >= 0.01f) {
		if (bMaintainHorizontalGroundVelocity)
		{
			Velocity -= G;
		}
		else
		{
			Velocity -= G;
			Velocity += (Velocity.UnsafeNormal() * G.Size() * 0.1f); //this may be sort of like what was here, but not sure
		}
	}
	/*
	FVector F = Velocity.ProjectOnTo(GetGravityDir());
	UE_LOG(LogTemp, Warning, TEXT("%d F size:%f"),__LINE__, F.Size()); //Z non-zero when jumping
	would fuck up stuff when running on sphere
	if (Velocity.Z != 0.f)
	{
		if (bMaintainHorizontalGroundVelocity)
		{
			// Ramp movement already maintained the velocity, so we just want to remove the vertical component.
			Velocity.Z = 0.f;
		}
		else
		{
			// Rescale velocity to be horizontal but maintain magnitude of last update.
			Velocity = Velocity.SafeNormal2D() * Velocity.Size();
		}
	}*/
}

//this isn't used with default settings
// couldn't get any settings that seemed likely to lead to fixing gimble lock
void UOrbitCharacterMovementComponent::PhysicsRotation(float DeltaTime)
{
	if (!HasValidData() || (!CharacterOwner->Controller && !bRunPhysicsWithNoController))
	{
	UE_LOG(LogTemp, Warning, TEXT("PhysRot inval or noCtrl and noPhyNoCtrl"));
		return;
	}

	if (!(bOrientRotationToMovement || bUseControllerDesiredRotation))
	{
		//bailing out here gdgd
	// UE_LOG(LogTemp, Warning, TEXT("PhysRot neither toMovement nor Controller"));
		return;
	}

	const FRotator CurrentRotation = CharacterOwner->GetActorRotation();
	FRotator DeltaRot = GetDeltaRotation(DeltaTime);
	FRotator DesiredRotation = CurrentRotation;

	if (bOrientRotationToMovement)
	{
		//no apparent difference
	UE_LOG(LogTemp, Warning, TEXT("PhysRot Orin2move"));
		DesiredRotation = ComputeOrientToMovementRotation(CurrentRotation, DeltaTime, DeltaRot);
	}
	else if (CharacterOwner->Controller && bUseControllerDesiredRotation)
	{
		//can set character to get here but causes fight over movement if ctrler pitch and roll
		// look down min pitch 270. force to 260 doesn't fix gimble lock. sees to be a camera only thing.
		// roll is always 0.0
		DesiredRotation = CharacterOwner->Controller->GetDesiredRotation();
	UE_LOG(LogTemp, Warning, TEXT("PhysRot orienController DesiredRot:%s"), *DesiredRotation.ToString());
	}
	else
	{
	UE_LOG(LogTemp, Warning, TEXT("PhysRot orin??"));
		return;
	}

	// Always remain vertical when walking or falling.
	if( IsMovingOnGround() || IsFalling() )
	{
		DesiredRotation.Pitch = 0;
		DesiredRotation.Roll = 0;
	}

	if( CurrentRotation.GetDenormalized().Equals(DesiredRotation.GetDenormalized(), 0.01f) )
	{
		return;
	}

	// Accumulate a desired new rotation.
	FRotator NewRotation = CurrentRotation;	

	//YAW
	if( DesiredRotation.Yaw != CurrentRotation.Yaw )
	{
		NewRotation.Yaw = FMath::FixedTurn(CurrentRotation.Yaw, DesiredRotation.Yaw, DeltaRot.Yaw);
	}

	// PITCH
	if( DesiredRotation.Pitch != CurrentRotation.Pitch )
	{
		NewRotation.Pitch = FMath::FixedTurn(CurrentRotation.Pitch, DesiredRotation.Pitch, DeltaRot.Pitch);
	}

	// ROLL
	if( DesiredRotation.Roll != CurrentRotation.Roll )
	{
		NewRotation.Roll = FMath::FixedTurn(CurrentRotation.Roll, DesiredRotation.Roll, DeltaRot.Roll);
	}

	//UpdatedComponent->AngularVelocity = CharAngularVelocity( CurrentRotation, NewRotation, deltaTime );

	// Set the new rotation.
	if( !NewRotation.Equals(CurrentRotation.GetDenormalized(), 0.01f) )
	{
		MoveUpdatedComponent( FVector::ZeroVector, NewRotation, true );
	}
}


// appears not to actually do anything
void UOrbitCharacterMovementComponent::ApplyImpactPhysicsForces(const FHitResult& Impact, const FVector& ImpactAcceleration, const FVector& ImpactVelocity)
{
	//UE_LOG(LogTemp, Warning, TEXT("ApplyImpactphyForc  ImpAccel:%s"), *ImpactAcceleration.ToString());
	if (bEnablePhysicsInteraction && Impact.bBlockingHit)
	{
		UPrimitiveComponent* ImpactComponent = Impact.GetComponent();
		// Impact Comp is usually not null
		//turning on Simulate physics to char collision capsule just fucked up everything and didn't get here
		if (ImpactComponent != NULL && ImpactComponent->IsAnySimulatingPhysics())
		{
	UE_LOG(LogTemp, Warning, TEXT("%d ApplyImpactphyForc  ImpAccel:%s"),__LINE__, *ImpactAcceleration.ToString());
			FVector ForcePoint = Impact.ImpactPoint;

			FBodyInstance* BI = ImpactComponent->GetBodyInstance(Impact.BoneName);

			float BodyMass = 1.0f;

			if (BI != NULL)
			{
	UE_LOG(LogTemp, Warning, TEXT("%d ApplyImpactphyForc  ImpAccel:%s"),__LINE__, *ImpactAcceleration.ToString());
				BodyMass = FMath::Max(BI->GetBodyMass(), 1.0f);

				FBox Bounds = BI->GetBodyBounds();

				FVector Center, Extents;
				Bounds.GetCenterAndExtents(Center, Extents);

				if (!Extents.IsNearlyZero())
				{
					ForcePoint.Z = Center.Z + Extents.Z * PushForcePointZOffsetFactor;
				}
			}

			FVector Force = Impact.ImpactNormal * -1.0f;

			float PushForceModificator = 1.0f;

			const FVector ComponentVelocity = ImpactComponent->GetPhysicsLinearVelocity();
			const FVector VirtualVelocity = ImpactAcceleration.IsZero() ? ImpactVelocity : ImpactAcceleration.SafeNormal() * GetMaxSpeed();

			float Dot = 0.0f;

			if (bScalePushForceToVelocity && !ComponentVelocity.IsNearlyZero())
			{			
	UE_LOG(LogTemp, Warning, TEXT("%d ApplyImpactphyForc  ImpAccel:%s"),__LINE__, *ImpactAcceleration.ToString());
				Dot = ComponentVelocity | VirtualVelocity;

				if (Dot > 0.0f && Dot < 1.0f)
				{
					PushForceModificator *= Dot;
				}
			}

			if (bPushForceScaledToMass)
			{
	UE_LOG(LogTemp, Warning, TEXT("%d ApplyImpactphyForc  ImpAccel:%s"),__LINE__, *ImpactAcceleration.ToString());
				PushForceModificator *= BodyMass;
			}

			Force *= PushForceModificator;

			if (ComponentVelocity.IsNearlyZero())
			{
	UE_LOG(LogTemp, Warning, TEXT("%d ApplyImpactphyForc  ImpAccel:%s"),__LINE__, *ImpactAcceleration.ToString());
				Force *= InitialPushForceFactor;
				ImpactComponent->AddImpulseAtLocation(Force, ForcePoint, Impact.BoneName);
			}
			else
			{
	UE_LOG(LogTemp, Warning, TEXT("%d ApplyImpactphyForc  ImpAccel:%s"),__LINE__, *ImpactAcceleration.ToString());
				Force *= PushForceFactor;
				ImpactComponent->AddForceAtLocation(Force, ForcePoint, Impact.BoneName);
			}
		}
	}
}

//does not get here
void UOrbitCharacterMovementComponent::SmoothClientPosition(float DeltaSeconds)
{
	UE_LOG(LogTemp, Warning, TEXT("%d SmoothClientPos"),__LINE__);
	if (!HasValidData() || GetNetMode() != NM_Client)
	{
		return;
	}

	FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character();
	if (ClientData && ClientData->bSmoothNetUpdates)
	{
		// smooth interpolation of mesh translation to avoid popping of other client pawns, unless driving or ragdoll or low tick rate
		if ((DeltaSeconds < ClientData->SmoothNetUpdateTime) && CharacterOwner->GetMesh() && !CharacterOwner->GetMesh()->IsSimulatingPhysics())
		{
			ClientData->MeshTranslationOffset = (ClientData->MeshTranslationOffset * (1.f - DeltaSeconds / ClientData->SmoothNetUpdateTime));
		}
		else
		{
			ClientData->MeshTranslationOffset = FVector::ZeroVector;
		}

		if (IsMovingOnGround())
		{
			// don't smooth Z position if walking on ground
			ClientData->MeshTranslationOffset.Z = 0;
		}

		if (CharacterOwner->GetMesh())
		{
			const FVector NewRelTranslation = CharacterOwner->ActorToWorld().InverseTransformVectorNoScale(ClientData->MeshTranslationOffset + CharacterOwner->GetBaseTranslationOffset());
			CharacterOwner->GetMesh()->SetRelativeLocation(NewRelTranslation);
		}
	}
}



//Does get here
bool UOrbitCharacterMovementComponent::ApplyRequestedMove(float DeltaTime, float MaxAccel, float MaxSpeed, float Friction, float BrakingDeceleration, FVector& OutAcceleration, float& OutRequestedSpeed)
{
		//Doesn't seem to get past here
	if (bHasRequestedVelocity)
	{
	UE_LOG(LogTemp, Warning, TEXT("%d RequestedMove"),__LINE__);
		const float RequestedSpeedSquared = RequestedVelocity.SizeSquared();
		if (RequestedSpeedSquared < KINDA_SMALL_NUMBER)
		{
			return false;
		}
	UE_LOG(LogTemp, Warning, TEXT("%d RequestedMove"),__LINE__);

		// Compute requested speed from path following
		float RequestedSpeed = FMath::Sqrt(RequestedSpeedSquared);
		const FVector RequestedMoveDir = RequestedVelocity / RequestedSpeed;
		RequestedSpeed = (bRequestedMoveWithMaxSpeed ? MaxSpeed : FMath::Min(MaxSpeed, RequestedSpeed));
		
		// Compute actual requested velocity
		const FVector MoveVelocity = RequestedMoveDir * RequestedSpeed;
		
		// Compute acceleration. Use MaxAccel to limit speed increase, 1% buffer.
		FVector NewAcceleration = FVector::ZeroVector;
		const float CurrentSpeedSq = Velocity.SizeSquared();
		if (bRequestedMoveUseAcceleration && CurrentSpeedSq < FMath::Square(RequestedSpeed * 1.01f))
		{
			// Turn in the same manner as with input acceleration.
			const float VelSize = FMath::Sqrt(CurrentSpeedSq);
			Velocity = Velocity - (Velocity - RequestedMoveDir * VelSize) * FMath::Min(DeltaTime * Friction, 1.f);

			// How much do we need to accelerate to get to the new velocity?
			NewAcceleration = ((MoveVelocity - Velocity) / DeltaTime);
			NewAcceleration = NewAcceleration.ClampMaxSize(MaxAccel);
		}
		else
		{
			// Just set velocity directly.
			// If decelerating we do so instantly, so we don't slide through the destination if we can't brake fast enough.
			Velocity = MoveVelocity;
		}

		// Copy to out params
		OutRequestedSpeed = RequestedSpeed;
		OutAcceleration = NewAcceleration;
		return true;
	}

	return false;
}

// Doesn't Get To Here
void UOrbitCharacterMovementComponent::RequestDirectMove(const FVector& MoveVelocity, bool bForceMaxSpeed)
{
	UE_LOG(LogTemp, Warning, TEXT("%d RequestedDirectMove"),__LINE__);
	if (MoveVelocity.SizeSquared() < KINDA_SMALL_NUMBER)
	{
		return;
	}

	if (IsFalling())
	{
		const FVector FallVelocity = MoveVelocity.ClampMaxSize(GetMaxSpeed());
		PerformAirControlForPathFollowing(FallVelocity, FallVelocity.Z);
		return;
	}

	RequestedVelocity = MoveVelocity;
	bHasRequestedVelocity = true;
	bRequestedMoveWithMaxSpeed = bForceMaxSpeed;

	if (IsMovingOnGround())
	{
		RequestedVelocity.Z = 0.0f;
	}
}

void UOrbitCharacterMovementComponent::OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode)
{
	//UE_LOG(LogTemp, Warning, TEXT("%d Movement Mode Changed "),__LINE__);
	if (!HasValidData())
	{
		return;
	}

	// React to changes in the movement mode.
	if (MovementMode == MOVE_Walking)
	{	
		// Walking uses only XY velocity, and must be on a walkable floor, with a Base.
		// not anymore since running on curves, stop vel along gravity direction, not z
		//Velocity.Z = 0.f;
		MaintainHorizontalGroundVelocity(); // Perfect, stops raven hopping upon landing. But gimble lock causes crash!
		//when get home, Try overrideing player controller with that quat code snippet... just zero out unwanted when adding yaw,roll,pitch
		bCrouchMaintainsBaseLocation = true;

		// make sure we update our new floor/base on initial entry of the walking physics
		FindFloor(UpdatedComponent->GetComponentLocation(), CurrentFloor, false);
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
			StopMovementKeepPathing();
			CharacterOwner->ClearJumpInput();
		}
	}

	CharacterOwner->OnMovementModeChanged(PreviousMovementMode, PreviousCustomMode);
}
