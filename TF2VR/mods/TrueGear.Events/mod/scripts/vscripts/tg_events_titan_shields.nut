// SERVER && SP. Confirmed enemy impacts and successful vortex refires on the
// occupied Titan produce feedback. Empty releases and shield lifecycle stay silent.
global function TrueGear_OnTitanShieldImpact
global function TrueGear_OnTitanVortexReflected
global function TrueGear_OnTitanParticleWallCreated
global function TrueGear_OnTitanParticleWallImpact

struct TGParticleWallState
{
	entity owner = null
	entity wall = null
}

struct
{
	array<TGParticleWallState> walls
} file

entity function TrueGear_TitanShieldLocalPlayer()
{
	foreach ( entity player in GetPlayerArray() )
	{
		if ( IsValid( player ) )
			return player
	}
	return null
}

bool function TrueGear_TitanShieldOwnerEligible( entity owner )
{
	return IsValid( owner ) && owner.IsPlayer() && IsAlive( owner )
		&& owner.IsTitan() && owner == TrueGear_TitanShieldLocalPlayer()
}

bool function TrueGear_TitanShieldAttackerEligible( entity owner, entity attacker )
{
	if ( !IsValid( attacker ) || attacker == owner )
		return false
	int ownerTeam = owner.GetTeam()
	int attackerTeam = attacker.GetTeam()
	// World, neutral, spectator, and both-team entities are not identified enemies.
	if ( ownerTeam == TEAM_INVALID || ownerTeam == TEAM_UNASSIGNED || ownerTeam == TEAM_SPECTATOR || ownerTeam == TEAM_BOTH
		|| attackerTeam == TEAM_INVALID || attackerTeam == TEAM_UNASSIGNED || attackerTeam == TEAM_SPECTATOR || attackerTeam == TEAM_BOTH )
		return false
	return IsEnemyTeam( ownerTeam, attackerTeam )
}

int function TrueGear_ParticleWallIndex( entity wall )
{
	for ( int i = 0; i < file.walls.len(); i++ )
	{
		if ( file.walls[i].wall == wall )
			return i
	}
	return -1
}

string function TrueGear_TitanShieldWeaponClass( string kind )
{
	switch ( kind )
	{
		case "vortex": return "mp_titanweapon_vortex_shield"
		case "heat": return "mp_titanweapon_heat_shield"
		case "sword": return "mp_titanability_basic_block"
		case "gun": return "mp_titanweapon_predator_cannon"
		case "particle": return "mp_titanability_particle_wall"
	}
	return ""
}

string function TrueGear_TitanShieldImpactName( string kind )
{
	switch ( kind )
	{
		case "vortex": return "titan_vortex_shield_impact"
		case "heat": return "titan_heat_shield_impact"
		case "sword": return "titan_sword_block_impact"
		case "gun": return "titan_gun_shield_impact"
		case "particle": return "titan_particle_wall_impact"
	}
	return ""
}

void function TrueGear_TitanShieldEmitImpact( entity owner, string kind, entity shieldWeapon, entity attacker, vector contactPos, string extraFields = "" )
{
	if ( !TrueGear_Enabled( "titan" ) || !TrueGear_TitanShieldOwnerEligible( owner )
		|| !TrueGear_TitanShieldAttackerEligible( owner, attacker ) )
		return
	string name = TrueGear_TitanShieldImpactName( kind )
	string weaponClass = TrueGear_TitanShieldWeaponClass( kind )
	if ( name == "" )
		return
	if ( kind != "particle" )
	{
		if ( !IsValid( shieldWeapon ) || shieldWeapon.GetWeaponOwner() != owner )
			return
		string actualClass = shieldWeapon.GetWeaponClassName()
		if ( actualClass != weaponClass && !( kind == "vortex" && actualClass == "mp_titanweapon_vortex_shield_ion" ) )
			return
		weaponClass = actualClass
	}

	TrueGearPlay( TrueGear_RecordEvent( name, TrueGear_PlayerFields( owner )
		+ TrueGear_String( "weapon", weaponClass )
		+ TrueGear_String( "shield_kind", kind )
		+ TrueGear_EntityFields( shieldWeapon, "shield_weapon_" )
		+ TrueGear_EntityFields( attacker, "attacker_" )
		+ TrueGear_Vector( "impact_position", contactPos )
		+ TrueGear_String( "detection", "confirmed_enemy_shield_impact" )
		+ extraFields ) )
}

// Vortex/heat absorption and the sword's successfully reduced-damage branch
// call this only after their original acceptance checks have succeeded.
void function TrueGear_OnTitanShieldImpact( entity owner, string kind, entity shieldWeapon, entity attacker, vector contactPos )
{
	TrueGear_TitanShieldEmitImpact( owner, kind, shieldWeapon, attacker, contactPos )
}

// Original server fire helpers call this only after firing bullets or creating a
// reflected projectile. Empty releases and queued bullet signals emit nothing.
void function TrueGear_OnTitanVortexReflected( entity weapon, int bullets, int projectiles )
{
	if ( !TrueGear_Enabled( "titan" ) || !IsValid( weapon )
		|| bullets < 0 || projectiles < 0 || ( bullets == 0 && projectiles == 0 ) )
		return
	string weaponClass = weapon.GetWeaponClassName()
	if ( weaponClass != "mp_titanweapon_vortex_shield" && weaponClass != "mp_titanweapon_vortex_shield_ion" )
		return
	entity owner = weapon.GetWeaponOwner()
	if ( !TrueGear_TitanShieldOwnerEligible( owner ) )
		return
	TrueGearPlay( TrueGear_RecordEvent( "titan_vortex_shield_reflect", TrueGear_PlayerFields( owner )
		+ TrueGear_String( "weapon", weaponClass )
		+ TrueGear_String( "shield_kind", "vortex" )
		+ TrueGear_EntityFields( weapon, "shield_weapon_" )
		+ TrueGear_Int( "reflected_bullets", bullets )
		+ TrueGear_Int( "reflected_projectiles", projectiles )
		+ TrueGear_String( "detection", "confirmed_vortex_refire" ) ) )
}

// CreateParticleWallForOwnerFromDirection establishes ownership. Generic sphere
// creation cannot distinguish player particle walls from unrelated defenses.
// Registration emits nothing and needs no polling, duration, or stop packets.
void function TrueGear_OnTitanParticleWallCreated( entity owner, entity wall )
{
	for ( int i = file.walls.len() - 1; i >= 0; i-- )
	{
		if ( !IsValid( file.walls[i].wall ) || !IsValid( file.walls[i].owner ) )
			file.walls.remove( i )
	}
	if ( !TrueGear_TitanShieldOwnerEligible( owner ) || !IsValid( wall )
		|| wall.GetClassName() != "vortex_sphere" || TrueGear_ParticleWallIndex( wall ) >= 0 )
		return
	TGParticleWallState state
	state.owner = owner
	state.wall = wall
	file.walls.append( state )
}

// The two accepted wall callbacks call this BEFORE draining the sphere's health,
// so the hit that destroys a wall is identified without a lifetime watcher.
void function TrueGear_OnTitanParticleWallImpact( entity wall, string impactType, entity attacker, vector contactPos )
{
	if ( !IsValid( wall ) || wall.GetClassName() != "vortex_sphere" )
		return
	string fields = TrueGear_EntityFields( wall, "wall_" )
		+ TrueGear_String( "impact_type", impactType )
		+ TrueGear_String( "impact_detection", "accepted_vortex_wall_hit_before_damage" )
	int index = TrueGear_ParticleWallIndex( wall )
	if ( index >= 0 )
	{
		TrueGear_TitanShieldEmitImpact( file.walls[index].owner, "particle", null, attacker, contactPos, fields )
		return
	}

	// Gun shields are created outside the particle-wall factory. Require the
	// original server sphere's exact target name, weapon, owner, and attachment.
	if ( wall.GetTargetName() != GUN_SHIELD_WALL )
		return
	entity owner = wall.GetOwner()
	entity weapon = wall.GetOwnerWeapon()
	if ( !IsValid( weapon ) || wall.GetParent() != weapon
		|| weapon.GetWeaponUtilityEntity() != wall )
		return
	TrueGear_TitanShieldEmitImpact( owner, "gun", weapon, attacker, contactPos, fields )
}
