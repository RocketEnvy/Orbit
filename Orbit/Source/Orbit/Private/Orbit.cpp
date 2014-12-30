// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

#include "Orbit.h"


IMPLEMENT_PRIMARY_GAME_MODULE( FDefaultGameModuleImpl, Orbit, "Orbit" );

/*
FVector ApplyGravity(){
	UE_LOG(LogTemp, Warning, TEXT("%d %s: In Orbit Main"), __LINE__, __FUNCTIONW__ ); 
	return FVector(1, 2, 3);
}

void Orbit::CalculateGravity()
{
	//FIXME Need to iterate over massive things to get vector sum of direction and force
	const float gravBodyMass = 90000000.f;
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
	AddImpulse(GravityVector);
	
	UE_LOG(LogTemp, Warning, TEXT("CalGrav:%d GDir:%s GM:%f GV:%s"), __LINE__,*GravityDirection.ToString(), GravityMagnitude, *GravityVector.ToString());
}*/
