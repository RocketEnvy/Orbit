// Fill out your copyright notice in the Description page of Project Settings.

#include "Orbit.h"
#include "GravityManager.h"
TMap<FString, FString> Fuckers;
//TMap<FString, FMyActorWrapper> Actors;
TMap<FString, AStaticMeshActor *> GravBods;//bodies that create gravity
TMap<FString, AStaticMeshActor *> GravActiveBods;//bodies that are attracted to gravity
TMap<FString, APlayerStart *> Players;
TMap<FString, FGravityBody> GravityBodies;//bodies that are attracted to gravity

UGravityManager::UGravityManager(const class FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UGravityManager::Start(){
	for (TObjectIterator<AStaticMeshActor> Itr; Itr; ++Itr)
	{
		if (Itr->ActorHasTag(TEXT("GravitationalBody"))){
			GravBods.Add(Itr->GetName(), *Itr);//is this storing the whole object? Hope not.
		}
		if (Itr->ActorHasTag(TEXT("GratitationallyActive"))){
			GravActiveBods.Add(Itr->GetName(), *Itr);//is this storing the whole object? Hope not.
		}
	}
	for (TObjectIterator<APlayerStart> Itr; Itr; ++Itr)
	{
		if (Itr->ActorHasTag(TEXT("GratitationallyActive"))){
			Players.Add(Itr->GetName(), *Itr);//is this storing the whole object? Hope not.
		}
	}
}


void UGravityManager::ApplyGravity(){
	FVector GravityDistanceVector = FVector::ZeroVector;
	float GravBodyMass = 0.f;
	APlayerStart* Player;
	AStaticMeshActor* ActiveBody;
	FGravityBody BodyStats, PlayerStats;

	for (auto GB : GravityBodies){
		GB.Value.GravityVector = FVector::ZeroVector;
		SetGravityBody(GB.Key, GB.Value);
	}

	for (auto Bod : GravBods)
	{
		auto GravitationalBody = Bod.Value;
		GravBodyMass = GravitationalBody->GetStaticMeshComponent()->GetBodyInstance()->MassInKg;
		for (auto ActiveBod : GravActiveBods){
			ActiveBody = ActiveBod.Value;
			BodyStats = GetGravityBody(ActiveBod.Key);
			if (ActiveBody->IsValidLowLevel() && GravitationalBody->IsValidLowLevel()){
				GravityDistanceVector = GravitationalBody->GetActorLocation() - ActiveBody->GetActorLocation();
				BodyStats.Magnitude = (ActiveBody->GetStaticMeshComponent()->GetBodyInstance()->MassInKg * GravBodyMass) / FMath::Square(GravityDistanceVector.Size());
				BodyStats.GravityVector += (GravityDistanceVector* BodyStats.Magnitude);
				SetGravityBody(ActiveBod.Key, BodyStats);
				//ActiveBody->GetStaticMeshComponent()->AddForce(BodyStats.GravityVector);
				//probably need to wait until finished b/f adding force for smoothness
			}
		}


		for (auto PlayerPair : Players){
			Player = PlayerPair.Value;
			PlayerStats = GetGravityBody(PlayerPair.Key);
			GetGravityBody(PlayerPair.Key);
			GravityDistanceVector = GravitationalBody->GetActorLocation() - Player->GetActorLocation();
			PlayerStats.SetMagnitude((1.f * GravBodyMass) / FMath::Square(GravityDistanceVector.Size()));
			PlayerStats.GravityVector += (GravityDistanceVector* PlayerStats.Magnitude);
			SetGravityBody(PlayerPair.Key, PlayerStats);
		}
	}
	for (auto ActiveBod : GravActiveBods){
		if (ActiveBod.Value->IsValidLowLevel()){
			ActiveBod.Value->GetStaticMeshComponent()->AddForce(GravityBodies[ActiveBod.Key].GravityVector);
		}
	}
	for (auto PlayerPair : Players){
		if (PlayerPair.Value->IsValidLowLevel()){
			PlayerPair.Value->GetCapsuleComponent()->AddForce(GravityBodies[PlayerPair.Key].GravityVector);
		}
	}
}

//Adds new GravityBody if it doesn't already exist, then returns reference to it.
FGravityBody UGravityManager::GetGravityBody(FString Name){
	if (!GravityBodies.Contains(Name)){
		UE_LOG(LogTemp, Warning, TEXT("Added Gravity Body:%s"), *Name);
		GravityBodies.Add(Name, FGravityBody());
	}
	return GravityBodies[Name];
}

bool UGravityManager::SetGravityBody(FString Name, FGravityBody GB){
	if (!GravityBodies.Contains(Name)){
		UE_LOG(LogTemp, Warning, TEXT("Missing Gravity Body:%s"), *Name);
		return 0;
	}
	GravityBodies[Name] = GB;
	return 1;
}

