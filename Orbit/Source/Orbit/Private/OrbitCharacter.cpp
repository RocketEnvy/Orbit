// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.
#include "Orbit.h"
#include "OrbitCharacter.h"
#include "OrbitProjectile.h"
#include "Animation/AnimInstance.h"
#include "OrbitCharacterMovementComponent.h"


//////////////////////////////////////////////////////////////////////////
// AOrbitCharacter

AOrbitCharacter::AOrbitCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UOrbitCharacterMovementComponent>(ACharacter::CharacterMovementComponentName))
{
	bDoFreeLook = false;

	// Set size for collision capsule
	GetCapsuleComponent()->InitCapsuleSize(42.f, 96.0f);

	// set our turn rates for input
	BaseTurnRate = 45.f;
	BaseLookUpRate = 45.f;

	// Create a CameraComponent	
	FirstPersonCameraComponent = ObjectInitializer.CreateDefaultSubobject<UCameraComponent>(this, TEXT("FirstPersonCamera"));
	FirstPersonCameraComponent->AttachParent = GetCapsuleComponent();
	//	FirstPersonCameraComponent->RelativeLocation = FVector(0, 0, 64.f); // Position the camera
	FirstPersonCameraComponent->bUsePawnControlRotation = true;

	// Default offset from the character location for projectiles to spawn
	GunOffset = FVector(100.0f, 30.0f, 10.0f);

	//Mesh = ObjectInitializer.CreateDefaultSubobject<USkeletalMeshComponent>(this, TEXT("CharacterMesh0"));
	// Create a mesh component that will be used when being viewed from a '1st person' view (when controlling this pawn)
	Mesh1P = ObjectInitializer.CreateDefaultSubobject<USkeletalMeshComponent>(this, TEXT("CharacterMesh1P"));
	Mesh1P->SetOnlyOwnerSee(false);			// only the owning player will see this mesh
	Mesh1P->AttachParent = FirstPersonCameraComponent;//doesn't seem to work
	//	Mesh1P->RelativeLocation = FVector(0.f, 0.f, -150.f);
	Mesh1P->bCastDynamicShadow = true;
	Mesh1P->CastShadow = true;
	//	Mesh1P->AttachParent = GetCapsuleComponent();
}

//////////////////////////////////////////////////////////////////////////
// Input

void AOrbitCharacter::SetupPlayerInputComponent(class UInputComponent* InputComponent)
{
	// set up gameplay key bindings
	check(InputComponent);

	InputComponent->BindAction("Jump", IE_Pressed, this, &ACharacter::Jump);
	InputComponent->BindAction("Jump", IE_Released, this, &ACharacter::StopJumping);

	InputComponent->BindAction("Fire", IE_Pressed, this, &AOrbitCharacter::OnFire);
	InputComponent->BindTouch(EInputEvent::IE_Pressed, this, &AOrbitCharacter::TouchStarted);

	InputComponent->BindAxis("MoveForward", this, &AOrbitCharacter::MoveForward);
	InputComponent->BindAxis("MoveRight", this, &AOrbitCharacter::MoveRight);

	// We have 2 versions of the rotation bindings to handle different kinds of devices differently
	// "turn" handles devices that provide an absolute delta, such as a mouse.
	// "turnrate" is for devices that we choose to treat as a rate of change, such as an analog joystick

	//That explanation makes no sense even though I know approximately what it is trying to say.

	//InputComponent->BindAxis("Turn", this, &AOrbitCharacter::AddControllerYawInput);
	InputComponent->BindAxis("Turn", this, &AOrbitCharacter::Turn);
	InputComponent->BindAxis("TurnRate", this, &AOrbitCharacter::TurnAtRate);//keyboard

	//InputComponent->BindAxis("LookUp", this, &APawn::AddControllerPitchInput);//mouse 

	InputComponent->BindAxis("LookUp", this, &AOrbitCharacter::LookUp);
	InputComponent->BindAxis("LookUpRate", this, &AOrbitCharacter::LookUpAtRate);//keyboard?
}

void AOrbitCharacter::OnFire()
{
	/*
			UE_LOG(LogTemp, Warning, TEXT("Sporting boner"));
			TArray<FName> boners;
			GetMesh()->GetBoneNames(boners);
			if (boners.Num() > 0){
			for (auto &c : boners){
			UE_LOG(LogTemp, Warning, TEXT("name:%s"), *c.ToString());
			}
			}

			const USkeletalMeshSocket* HeadSocket = GetMesh()->GetSocketByName("headSocket");
			if (HeadSocket){
			UE_LOG(LogTemp, Warning, TEXT("HEAD socket: %s"), *HeadSocket->GetName());

			HeadSocket->AttachActor(FirstPersonCameraComponent->GetAttachmentRootActor(), Boner);// LogTemp : Warning : HEAD socket : SkeletalMeshSocket_0
			UE_LOG(LogTemp, Warning, TEXT("Still Alive"));
			}
			else{
			UE_LOG(LogTemp, Warning, TEXT("Lost my HEAD"));
			}
			*/
	// try and fire a projectile
	if (ProjectileClass != NULL)
	{
		const FRotator SpawnRotation = GetControlRotation();
		// MuzzleOffset is in camera space, so transform it to world space before offsetting from the character location to find the final muzzle position
		const FVector SpawnLocation = GetActorLocation() + SpawnRotation.RotateVector(GunOffset);

		UWorld* const World = GetWorld();
		if (World != NULL)
		{
			// spawn the projectile at the muzzle
			World->SpawnActor<AOrbitProjectile>(ProjectileClass, SpawnLocation, SpawnRotation);
		}
	}

	// try and play the sound if specified
	if (FireSound != NULL)
	{
		UGameplayStatics::PlaySoundAtLocation(this, FireSound, GetActorLocation());
	}

	// try and play a firing animation if specified
	if (FireAnimation != NULL)
	{
		// Get the animation object for the arms mesh
#ifdef VERSION27
		UAnimInstance* AnimInstance = Mesh1P->GetAnimInstance();
		if (AnimInstance != NULL)
		{
			AnimInstance->Montage_Play(FireAnimation, 1.f);
		}
#endif
	}

}

void AOrbitCharacter::TouchStarted(const ETouchIndex::Type FingerIndex, const FVector Location)
{
	// only fire for first finger down
	if (FingerIndex == 0)
	{
		OnFire();
	}
}

void AOrbitCharacter::MoveForward(float Value)
{
	if (Value != 0.0f)
	{
		// add movement in that direction
		AddMovementInput(GetActorForwardVector(), Value);
	}
}

void AOrbitCharacter::MoveRight(float Value)
{
	if (Value != 0.0f)
	{
		// add movement in that direction
		AddMovementInput(GetActorRightVector(), Value);
	}
}

void AOrbitCharacter::TurnAtRate(float Rate) //keyboard
{
	UOrbitCharacterMovementComponent* OrbitMovementComponent = Cast<UOrbitCharacterMovementComponent>(GetCharacterMovement());
	if (OrbitMovementComponent)
	{
		OrbitMovementComponent->SumYaw(Rate);
	}
}

void AOrbitCharacter::Turn(float Rate)
{
	if (bDoFreeLook)
	{
		FirstPersonCameraComponent->AddLocalRotation(FRotator(0, -45.f * Rate * GetWorld()->GetDeltaSeconds(), 0));
	}
	else
	{
		UOrbitCharacterMovementComponent* OrbitMovementComponent = Cast<UOrbitCharacterMovementComponent>(GetCharacterMovement());
		if (OrbitMovementComponent)
		{
			OrbitMovementComponent->SumYaw(Rate);
		}
	}
}

void AOrbitCharacter::LookUpAtRate(float Rate)//don't know
{
	// calculate delta for this frame from the rate information
	AddControllerPitchInput(Rate * BaseLookUpRate * GetWorld()->GetDeltaSeconds());
}


void AOrbitCharacter::LookUp(float Rate)
{
	FirstPersonCameraComponent->AddLocalRotation(FRotator(-45.f * Rate * GetWorld()->GetDeltaSeconds(), 0, 0));
	AddControllerPitchInput(Rate);
}
