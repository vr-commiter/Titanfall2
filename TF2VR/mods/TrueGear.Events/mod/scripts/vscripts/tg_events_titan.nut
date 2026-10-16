// TrueGear.Events: server-side, singleplayer Titan and confirmed NPC kill hooks.
// This module observes events only. It does not change damage or game state.
global function TrueGear_TitanInit

struct
{
	bool initialized = false
	array<entity> shieldAttached
	entity localPlayer = null
	entity petTitan = null
	entity ownedSoul = null
} file

void function TrueGear_TitanInit()
{
	if ( file.initialized )
		return
	file.initialized = true

	AddCallback_OnPilotBecomesTitan( TrueGear_TitanOnEmbark )
	AddCallback_OnTitanBecomesPilot( TrueGear_TitanOnDisembark )
	AddCallback_OnTitanDoomed( TrueGear_TitanOnDoomed )
	AddSoulDeathCallback( TrueGear_TitanOnSoulDeath )
	AddCallback_OnNPCKilled( TrueGear_TitanOnNPCKilled )
	thread TrueGear_TitanAssociationWatch()
}

entity function TrueGear_TitanLocalPlayer()
{
	foreach ( entity player in GetPlayerArray() )
	{
		if ( IsValid( player ) )
			return player
	}
	return null
}

// In SP the player's playable Titan is still the player entity. When exited,
// GetPetTitan() identifies BT/the owned NPC Titan. The cached soul survives the
// brief entity transfer between those two forms and is needed for soul death.
bool function TrueGear_TitanIsOwned( entity titan, entity player )
{
	if ( !IsValid( titan ) || !IsValid( player ) || !titan.IsTitan() )
		return false
	if ( titan == player || titan == player.GetPetTitan() )
		return true
	entity soul = titan.GetTitanSoul()
	return IsValid( soul ) && soul == file.ownedSoul && player == file.localPlayer
}

void function TrueGear_TitanRememberSoul( entity titan )
{
	if ( !IsValid( titan ) || !titan.IsTitan() )
		return
	entity soul = titan.GetTitanSoul()
	if ( IsValid( soul ) )
		file.ownedSoul = soul
}

string function TrueGear_TitanStateFields( entity titan )
{
	string fields = TrueGear_EntityFields( titan, "titan_" )
	if ( !IsValid( titan ) || !titan.IsTitan() )
		return fields
	fields += TrueGear_Int( "titan_health", titan.GetHealth() )
	fields += TrueGear_Int( "titan_max_health", titan.GetMaxHealth() )
	entity soul = titan.GetTitanSoul()
	if ( IsValid( soul ) )
	{
		fields += TrueGear_Int( "soul_handle", soul.GetEncodedEHandle() )
		fields += TrueGear_Float( "shield", float( soul.GetShieldHealth() ) )
		fields += TrueGear_Float( "shield_max", float( soul.GetShieldHealthMax() ) )
	}
	return fields
}

string function TrueGear_TitanDamageFields( var damageInfo )
{
	return TrueGear_Float( "damage", DamageInfo_GetDamage( damageInfo ) )
		+ TrueGear_Int( "damage_type", DamageInfo_GetDamageType( damageInfo ) )
		+ TrueGear_Int( "custom_damage_type", DamageInfo_GetCustomDamageType( damageInfo ) )
		+ TrueGear_Int( "damage_source_id", DamageInfo_GetDamageSourceIdentifier( damageInfo ) )
		+ TrueGear_Int( "hit_group", DamageInfo_GetHitGroup( damageInfo ) )
		+ TrueGear_Int( "hit_box", DamageInfo_GetHitBox( damageInfo ) )
		+ TrueGear_Vector( "damage_position", DamageInfo_GetDamagePosition( damageInfo ) )
		+ TrueGear_EntityFields( DamageInfo_GetAttacker( damageInfo ), "attacker_" )
		+ TrueGear_EntityFields( DamageInfo_GetInflictor( damageInfo ), "inflictor_" )
		+ TrueGear_EntityFields( DamageInfo_GetWeapon( damageInfo ), "damage_weapon_" )
}

void function TrueGear_TitanAttachShield( entity titan )
{
	if ( !IsValid( titan ) || file.shieldAttached.contains( titan ) )
		return
	AddEntityCallback_OnPostShieldDamage( titan, TrueGear_TitanOnShieldDamage )
	file.shieldAttached.append( titan )
}

void function TrueGear_TitanAssociationWatch()
{
	while ( true )
	{
		entity player = TrueGear_TitanLocalPlayer()
		if ( player != file.localPlayer )
		{
			file.localPlayer = player
			file.petTitan = null
			file.ownedSoul = null
		}

		// Drop destroyed handles so a new entity never inherits a stale dedupe.
		for ( int i = file.shieldAttached.len() - 1; i >= 0; i-- )
		{
			if ( !IsValid( file.shieldAttached[i] ) )
				file.shieldAttached.remove( i )
		}

		if ( IsValid( player ) )
		{
			// Attaching to the persistent player also covers future embarkations.
			TrueGear_TitanAttachShield( player )
			TrueGear_TitanRememberSoul( player )
			entity pet = player.GetPetTitan()
			if ( IsValid( pet ) )
			{
				TrueGear_TitanAttachShield( pet )
				TrueGear_TitanRememberSoul( pet )
			}

			if ( pet != file.petTitan )
			{
				if ( TrueGear_Enabled( "titan" ) )
				{
					// This is an ownership association change, not a death event.
					if ( file.petTitan != null )
						TrueGearPlay( TrueGear_RecordEvent( "titan_auto_unassigned", TrueGear_PlayerFields( player )
							+ TrueGear_TitanStateFields( file.petTitan ) + TrueGear_String( "detection", "poll" ) ) )
					if ( IsValid( pet ) )
						TrueGearPlay( TrueGear_RecordEvent( "titan_auto_assigned", TrueGear_PlayerFields( player )
							+ TrueGear_TitanStateFields( pet ) + TrueGear_String( "detection", "poll" ) ) )
				}
				file.petTitan = pet
			}
		}
		wait TrueGear_PollInterval()
	}
}

void function TrueGear_TitanOnEmbark( entity pilot, entity npcTitan )
{
	if ( pilot != TrueGear_TitanLocalPlayer() )
		return
	file.localPlayer = pilot
	TrueGear_TitanRememberSoul( pilot )
	TrueGear_TitanRememberSoul( npcTitan )
	TrueGear_TitanAttachShield( pilot )
	if ( !TrueGear_Enabled( "titan" ) )
		return
	TrueGearPlay( TrueGear_RecordEvent( "titan_embark_complete", TrueGear_PlayerFields( pilot )
		+ TrueGear_EntityFields( npcTitan, "previous_titan_" ) ) )
}

void function TrueGear_TitanOnDisembark( entity pilot, entity npcTitan )
{
	if ( pilot != TrueGear_TitanLocalPlayer() )
		return
	file.localPlayer = pilot
	TrueGear_TitanRememberSoul( npcTitan )
	TrueGear_TitanAttachShield( npcTitan )
	if ( !TrueGear_Enabled( "titan" ) )
		return
	TrueGearPlay( TrueGear_RecordEvent( "titan_disembark_complete", TrueGear_PlayerFields( pilot )
		+ TrueGear_TitanStateFields( npcTitan ) ) )
}

void function TrueGear_TitanOnDoomed( entity victim, var damageInfo )
{
	entity player = TrueGear_TitanLocalPlayer()
	if ( !TrueGear_Enabled( "titan" ) || !TrueGear_TitanIsOwned( victim, player ) )
		return
	TrueGearPlay( TrueGear_RecordEvent( "titan_doomed", TrueGear_PlayerFields( player )
		+ TrueGear_TitanStateFields( victim ) + TrueGear_TitanDamageFields( damageInfo )
		+ TrueGear_Bool( "occupied", victim == player ) ) )
}

void function TrueGear_TitanOnSoulDeath( entity soul, var damageInfo )
{
	entity player = TrueGear_TitanLocalPlayer()
	if ( !TrueGear_Enabled( "titan" ) || !IsValid( player ) || !IsValid( soul ) )
		return
	entity titan = soul.GetTitan()
	if ( soul != file.ownedSoul && !TrueGear_TitanIsOwned( titan, player ) )
		return
	TrueGearPlay( TrueGear_RecordEvent( "titan_destroyed", TrueGear_PlayerFields( player )
		+ TrueGear_TitanStateFields( titan ) + TrueGear_Int( "destroyed_soul_handle", soul.GetEncodedEHandle() )
		+ TrueGear_TitanDamageFields( damageInfo ) ) )
}

void function TrueGear_TitanOnShieldDamage( entity titan, var damageInfo, float actualShieldDamage )
{
	entity player = TrueGear_TitanLocalPlayer()
	if ( !TrueGear_Enabled( "titan" ) || !TrueGear_TitanIsOwned( titan, player ) || actualShieldDamage <= 0 )
		return
	string fields = TrueGear_PlayerFields( player ) + TrueGear_TitanStateFields( titan )
		+ TrueGear_TitanDamageFields( damageInfo ) + TrueGear_Float( "shield_damage", actualShieldDamage )
		+ TrueGear_Bool( "occupied", titan == player )
	TrueGearPlay( TrueGear_RecordEvent( "titan_shield_damage", fields ) )
}

void function TrueGear_TitanOnNPCKilled( entity victim, entity attacker, var damageInfo )
{
	entity player = TrueGear_TitanLocalPlayer()
	if ( !TrueGear_Enabled( "combat" ) || !IsValid( player ) || !IsValid( victim ) || !IsValid( attacker ) )
		return
	if ( attacker != player && !TrueGear_TitanIsOwned( attacker, player ) )
		return
	if ( victim.GetTeam() == player.GetTeam() )
		return
	TrueGearPlay( TrueGear_RecordEvent( "combat_enemy_killed", TrueGear_PlayerFields( player )
		+ TrueGear_EntityFields( victim, "victim_" ) + TrueGear_TitanDamageFields( damageInfo )
		+ TrueGear_Bool( "by_owned_titan", attacker.IsTitan() )
		+ TrueGear_Float( "distance", Distance( player.GetOrigin(), victim.GetOrigin() ) ) ) )
}
