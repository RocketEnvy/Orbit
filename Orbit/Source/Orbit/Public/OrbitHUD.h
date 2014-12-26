// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.
#pragma once 
#include "Orbit.h"
#include "GameFramework/HUD.h"
#include "OrbitHUD.generated.h"

UCLASS()
class AOrbitHUD : public AHUD
{
	GENERATED_BODY()

public:
	AOrbitHUD(const FObjectInitializer& ObjectInitializer);

	/** Primary draw call for the HUD */
	virtual void DrawHUD() override;

private:
	/** Crosshair asset pointer */
	class UTexture2D* CrosshairTex;

};

