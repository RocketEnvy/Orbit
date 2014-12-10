Orbit
=====

Attempt to add real gravity physics and complex 3D motion to Unreal Engine 4 Game

Notes:
1. After adding OrbitCharacterMovementComponent there were errors about physics and log stuff being undefined. The fix is to
   include Engine.h instead of EngineMinimal.h in Orbit.h.
2. Not sure map is being saved, if not, add big BSP sphere at 0,0,5000 ; tesselation:6, radius: 1000.
