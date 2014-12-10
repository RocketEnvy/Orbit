// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

#include "Orbit.h"
#include "OrbitGameMode.h"
#include "OrbitHUD.h"
#include "OrbitCharacter.h"

AOrbitGameMode::AOrbitGameMode(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// set default pawn class to our Blueprinted character
	static ConstructorHelpers::FClassFinder<APawn> PlayerPawnClassFinder(TEXT("/Game/Blueprints/MyCharacter"));
	DefaultPawnClass = PlayerPawnClassFinder.Class;

	// use our custom HUD class
	HUDClass = AOrbitHUD::StaticClass();
}
