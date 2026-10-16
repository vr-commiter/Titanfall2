// SERVER && SP. The native main-frame callback invokes this after scripts load.
// Loading an existing save does not run Northstar's MapSpawn mod callbacks.
global function TrueGear_ServerBootstrap

void function TrueGear_ServerBootstrap()
{
	// Keep the failing module visible in SCRIPT SV instead of reporting a
	// successful bootstrap when only the function lookup succeeded.
	TrueGear_CoreInit()
	TrueGear_MovementInit()
	TrueGear_HandsInit()
	TrueGear_TitanInit()
}
