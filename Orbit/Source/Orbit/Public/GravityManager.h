// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "Orbit.h"
#include <map>
#include "GameFramework/Actor.h"
#include "GravityManager.generated.h"

USTRUCT()
struct ORBIT_API FGravityBody
{
	GENERATED_USTRUCT_BODY()
public:
	int foo=0.f;
	float Magnitude=0;
	FVector GravityVector = FVector::ZeroVector;

	void SetVector(FVector GV){ GravityVector = GV; }
	void AddVector(FVector GV){ GravityVector += GV; }
	FVector GetVector(void){ return GravityVector; }
	float GetMagnitude(void){ return Magnitude; }
	void SetMagnitude(float mag){ Magnitude=mag; }
	FVector GetDirection(void){ return GravityVector.GetSafeNormal(); }
};

/**
 * Coordinates Gravitational forces between actors 
 */
UCLASS()
class ORBIT_API UGravityManager : public UObject
{
	GENERATED_BODY()
public:
	UGravityManager(){}
	UGravityManager::UGravityManager(const class FObjectInitializer& ObjectInitializer);
	//void RegisterActor(AActor& InActor, FVector &GravityVector);
	void ApplyGravity(void);
	FVector ApplyGravityTo(FString Name);
	FGravityBody GetGravityBody(FString Name);
	bool SetGravityBody(FString Name, FGravityBody GB);
	void Start();
};