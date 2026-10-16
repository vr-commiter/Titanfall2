untyped

global function TrueGear_CombatInit

// Original event collector. CLIENT && SP only. Native game/VR state is read;
// no weapon script, damage function, projectile, or input behavior is replaced.
struct
{
	bool initialized = false
	table<int, string> sourceNames
	entity player = null
	entity soul = null
	bool titan = false
	bool enabled = false
	int health = 0
	int maxHealth = 0
	int shield = 0
	int maxShield = 0
	bool lowHealth = false
	bool healing = false
	float lastHealTime = 0.0
	bool shieldRegen = false
	float lastShieldRegenTime = 0.0
	entity weapon = null
	string weaponName = ""
	string weaponType = "unknown"
	int weaponHand = -1
	int weaponHandRaw = -1
	int clip = 0
	int reserve = 0
	bool reloading = false
	int reloadStartClip = 0
	bool reloadRefilled = false
	bool aiming = false
	bool melee = false
	float charge = 0.0
	int chargeBucket = 0
	bool chargeRising = false
	float lastChargeRiseTime = 0.0
	int vrEjections = 0
	int vrRackedRounds = 0
	bool vrMagazineOut = false
} file

void function TrueGear_CombatInit()
{
	if ( file.initialized )
		return
	file.initialized = true

	foreach ( name, id in eDamageSourceId )
		file.sourceNames[ id ] <- name

	// Register the actual source IDs present in this game's enum, including
	// mod-added IDs, instead of assuming that all integers in a range are valid.
	foreach ( id, name in file.sourceNames )
		AddLocalPlayerTookDamageCallback( id, TrueGear_CombatTookDamage )

	// The installed SP cl_sp_player.gnut calls this existing array with these
	// four arguments. Do not overwrite ClientCodeCallback_PlayerDidDamage.
	clGlobal.onLocalPlayerDidDamageCallback.append( TrueGear_CombatDidDamage )
	thread TrueGear_CombatWatch()
}

string function TrueGear_CombatDamageFlags( int flags )
{
	return TrueGear_Int( "damage_type", flags )
		+ TrueGear_Bool( "bullet", ( flags & DF_BULLET ) != 0 )
		+ TrueGear_Bool( "explosion", ( flags & DF_EXPLOSION ) != 0 )
		+ TrueGear_Bool( "melee", ( flags & DF_MELEE ) != 0 )
		+ TrueGear_Bool( "electric", ( flags & DF_ELECTRICAL ) != 0 )
		+ TrueGear_Bool( "shield_hit", ( flags & DF_SHIELD_DAMAGE ) != 0 )
		+ TrueGear_Bool( "headshot", ( flags & DF_HEADSHOT ) != 0 )
		+ TrueGear_Bool( "critical", ( flags & DF_CRITICAL ) != 0 )
		+ TrueGear_Bool( "killshot", ( flags & DF_KILLSHOT ) != 0 )
		+ TrueGear_Bool( "titan_step", ( flags & DF_TITAN_STEP ) != 0 )
		+ TrueGear_Bool( "shotgun", ( flags & DF_SHOTGUN ) != 0 )
}

// A valid position alone does not establish a direction: environmental ticks
// can use the victim, a trigger center, or a synthetic/randomized origin.
bool function TrueGear_CombatDamageHasDirection( int flags, int sourceID )
{
	if ( flags & ( DF_NO_INDICATOR | DF_DOOMED_HEALTH_LOSS ) )
		return false

	switch ( sourceID )
	{
		case eDamageSourceId.fall:
		case eDamageSourceId.submerged:
		case eDamageSourceId.outOfBounds:
		case eDamageSourceId.toxic_sludge:
		case eDamageSourceId.deadly_fog:
		case eDamageSourceId.floor_is_lava:
		case eDamageSourceId.burn:
		case eDamageSourceId.indoor_inferno:
		case eDamageSourceId.electric_conduit:
		case eDamageSourceId.titanEmpField:
		case eDamageSourceId.lasergrid:
		case eDamageSourceId.turbine:
		case eDamageSourceId.stuck:
		case eDamageSourceId.switchback_trap:
		case eDamageSourceId.mp_titanability_smoke:
		case eDamageSourceId.mp_weapon_grenade_electric_smoke:
		case eDamageSourceId.damagedef_suicide:
		case eDamageSourceId.damagedef_despawn:
		case eDamageSourceId.team_switch:
		case eDamageSourceId.round_end:
		case eDamageSourceId.bleedout:
			return false
	}
	// Weapons, explosions (including self-damage), and unknown sources may
	// retain a real origin. TrueGearPlayAngle still checks source/camera data.
	return true
}

void function TrueGear_CombatTookDamage( float damage, vector origin, int flags, int sourceID, entity attacker )
{
	if ( !TrueGear_Enabled( "combat" ) )
		return
	entity player = GetLocalClientPlayer()
	if ( !IsValid( player ) || player != GetLocalViewPlayer() || damage <= 0.0 )
		return

	string sourceName = sourceID in file.sourceNames ? file.sourceNames[ sourceID ] : "unknown"
	string fields = TrueGear_PlayerFields( player ) + TrueGear_Float( "amount", damage )
		+ TrueGear_Int( "damage_source_id", sourceID ) + TrueGear_String( "damage_source", sourceName )
		+ TrueGear_Vector( "damage_origin", origin ) + TrueGear_CombatDamageFlags( flags )
		+ TrueGear_EntityFields( attacker, "attacker_" )

	bool directional = TrueGear_CombatDamageHasDirection( flags, sourceID )
	TrueGear_CombatDamagePlay( directional ? "combat_damage_taken" : "combat_damage_taken_nondirectional", fields, origin, directional )
	TrueGear_CombatDamageSubevents( fields, flags, sourceID, origin, directional )
}

void function TrueGear_CombatDamagePlay( string name, string fields, vector damageOrigin, bool directional )
{
	string playbackName = TrueGear_RecordEvent( name, fields )
	if ( directional )
		TrueGearPlayAngle( playbackName, damageOrigin, name == "combat_damage_taken" ? "combat_damage_taken_nondirectional" : "" )
	else
		TrueGearPlay( playbackName )
}

void function TrueGear_CombatDamageSubevents( string fields, int flags, int sourceID, vector damageOrigin, bool directional )
{
	// These are derived views of the same callback. Consumers should select
	// either damage_taken or a subtype, to avoid playing the same hit twice.
	if ( flags & DF_BULLET )
		TrueGear_CombatDamagePlay( "combat_damage_bullet", fields, damageOrigin, directional )
	if ( flags & DF_EXPLOSION )
		TrueGear_CombatDamagePlay( "combat_damage_explosion", fields, damageOrigin, directional )
	if ( flags & DF_MELEE )
		TrueGear_CombatDamagePlay( "combat_damage_melee", fields, damageOrigin, directional )
	if ( ( flags & DF_ELECTRICAL ) || sourceID == eDamageSourceId.electric_conduit || sourceID == eDamageSourceId.titanEmpField )
		TrueGear_CombatDamagePlay( "combat_damage_electric", fields, damageOrigin, directional )
	if ( flags & DF_SHIELD_DAMAGE )
		TrueGear_CombatDamagePlay( "combat_damage_shield", fields, damageOrigin, directional )
	if ( flags & DF_HEADSHOT )
		TrueGear_CombatDamagePlay( "combat_damage_headshot", fields, damageOrigin, directional )
	if ( flags & DF_CRITICAL )
		TrueGear_CombatDamagePlay( "combat_damage_critical", fields, damageOrigin, directional )

	switch ( sourceID )
	{
		case eDamageSourceId.burn:
		case eDamageSourceId.molotov:
		case eDamageSourceId.indoor_inferno:
		case eDamageSourceId.mp_weapon_thermite_grenade:
		case eDamageSourceId.mp_titanweapon_meteor_thermite:
		case eDamageSourceId.mp_titanweapon_meteor_thermite_charged:
			TrueGear_CombatDamagePlay( "combat_damage_fire", fields, damageOrigin, directional )
			break
		case eDamageSourceId.fall:
			TrueGear_CombatDamagePlay( "combat_damage_fall", fields, damageOrigin, directional )
			break
		case eDamageSourceId.crushed:
		case eDamageSourceId.splat:
		case eDamageSourceId.damagedef_crush:
			TrueGear_CombatDamagePlay( "combat_damage_crush", fields, damageOrigin, directional )
			break
		case eDamageSourceId.submerged:
		case eDamageSourceId.toxic_sludge:
		case eDamageSourceId.deadly_fog:
		case eDamageSourceId.outOfBounds:
		case eDamageSourceId.lasergrid:
		case eDamageSourceId.turbine:
		case eDamageSourceId.floor_is_lava:
			TrueGear_CombatDamagePlay( "combat_damage_environment", fields, damageOrigin, directional )
			break
	}
}

void function TrueGear_CombatDidDamage( entity attacker, entity victim, vector position, int flags )
{
	if ( !TrueGear_Enabled( "combat" ) || !IsValid( attacker ) || attacker != GetLocalClientPlayer() || !IsValid( victim ) )
		return
	string fields = TrueGear_PlayerFields( attacker ) + TrueGear_EntityFields( victim, "victim_" )
		+ TrueGear_Vector( "hit_position", position ) + TrueGear_CombatDamageFlags( flags )
		+ TrueGear_Bool( "damage_amount_available", false )
	TrueGearPlay( TrueGear_RecordEvent( "combat_damage_dealt", fields ) )
	if ( flags & DF_HEADSHOT )
		TrueGearPlay( TrueGear_RecordEvent( "combat_hit_headshot", fields ) )
	if ( flags & DF_CRITICAL )
		TrueGearPlay( TrueGear_RecordEvent( "combat_hit_critical", fields ) )
	if ( flags & DF_MELEE )
		TrueGearPlay( TrueGear_RecordEvent( "combat_hit_melee", fields ) )
	if ( flags & DF_KILLSHOT )
		TrueGearPlay( TrueGear_RecordEvent( "combat_kill_confirmed", fields ) )
}

void function TrueGear_CombatWatch()
{
	while ( true )
	{
		try
		{
			TrueGear_CombatPoll()
		}
		catch ( error )
		{
			// A transient destroyed entity must not permanently stop the watcher.
			TrueGear_CombatReset( "watcher_resync" )
			wait 1.0
		}
		wait TrueGear_PollInterval()
	}
}

void function TrueGear_CombatReset( string reason )
{
	string fields = TrueGear_String( "reason", reason )
	if ( IsValid( file.player ) )
		fields += TrueGear_PlayerFields( file.player )
	if ( file.enabled )
	{
		string weaponFields = fields + TrueGear_String( "weapon", file.weaponName )
			+ TrueGear_CombatWeaponHandFields( file.weaponHand, file.weaponHandRaw, "weapon_hand_at_previous_sample", "previous_sample" )
		if ( file.shieldRegen )
			TrueGearPlay( TrueGear_RecordEvent( "shield_regen_end", fields ) )
		if ( file.reloading )
			TrueGearPlay( TrueGear_RecordEvent( "weapon_reload_end", weaponFields ) )
		if ( file.aiming )
			TrueGearPlay( TrueGear_RecordEvent( "weapon_aim_end", weaponFields ) )
		if ( file.melee )
			TrueGearPlay( TrueGear_RecordEvent( "combat_melee_end", fields ) )
		if ( file.chargeRising )
			TrueGearPlay( TrueGear_RecordEvent( "weapon_charge_rise_end", weaponFields + TrueGear_Bool( "inferred", true ) ) )
	}
	file.player = null
	file.soul = null
	file.weapon = null
	file.weaponName = ""
	file.weaponType = "unknown"
	file.weaponHand = -1
	file.weaponHandRaw = -1
	file.healing = false
	file.lastHealTime = 0.0
	file.shieldRegen = false
	file.reloading = false
	file.aiming = false
	file.melee = false
	file.chargeRising = false
}

void function TrueGear_CombatPoll()
{
	entity player = GetLocalClientPlayer()
	bool enabled = TrueGear_Enabled( "combat" )
	if ( !IsValid( player ) || !IsAlive( player ) || player != GetLocalViewPlayer() )
	{
		if ( file.player != null )
			TrueGear_CombatReset( "player_unavailable" )
		file.enabled = enabled
		return
	}

	bool titan = player.IsTitan()
	entity soul = titan ? player.GetTitanSoul() : null
	if ( file.player != player || file.titan != titan || file.soul != soul || enabled != file.enabled )
	{
		TrueGear_CombatReset( "context_changed" )
		file.player = player
		file.soul = soul
		file.titan = titan
		file.enabled = enabled
		TrueGear_CombatBaseline( player )
		return
	}

	TrueGear_CombatHealthPoll( player )
	TrueGear_CombatWeaponPoll( player )
	bool melee = player.PlayerMelee_IsAttackActive()
	if ( melee != file.melee && enabled )
		TrueGearPlay( TrueGear_RecordEvent( melee ? "combat_melee_start" : "combat_melee_end", TrueGear_PlayerFields( player ) ) )
	file.melee = melee
}

void function TrueGear_CombatBaseline( entity player )
{
	file.health = player.GetHealth()
	file.maxHealth = player.GetMaxHealth()
	file.lowHealth = TrueGear_CombatIsLowHealth( file.health, file.maxHealth )
	file.healing = false
	file.lastHealTime = 0.0
	entity shieldEntity = IsValid( file.soul ) ? file.soul : player
	file.shield = shieldEntity.GetShieldHealth()
	file.maxShield = shieldEntity.GetShieldHealthMax()
	file.melee = player.PlayerMelee_IsAttackActive()
	file.vrEjections = GetConVarInt( "tf2vr_reload_ejections" )
	file.vrRackedRounds = GetConVarInt( "tf2vr_reload_rounds" )
	file.vrMagazineOut = GetConVarBool( "tf2vr_reload_magazine_out" )
	if ( file.enabled )
	{
		TrueGearPlay( TrueGear_RecordEvent( "health_snapshot", TrueGear_PlayerFields( player ) + TrueGear_Int( "health", file.health )
			+ TrueGear_Int( "max_health", file.maxHealth ) + TrueGear_Bool( "low_health", file.lowHealth ) + TrueGear_Bool( "baseline", true ) ) )
		TrueGearPlay( TrueGear_RecordEvent( "shield_snapshot", TrueGear_PlayerFields( player ) + TrueGear_Int( "shield", file.shield )
			+ TrueGear_Int( "max_shield", file.maxShield ) + TrueGear_Bool( "baseline", true ) ) )
	}
	TrueGear_CombatWeaponPoll( player )
}

bool function TrueGear_CombatIsLowHealth( int health, int maximum )
{
	float threshold = GetConVarFloat( "tg_events_low_health" )
	threshold = max( 0.01, min( threshold, 0.95 ) )
	return maximum > 0 && health > 0 && float( health ) / float( maximum ) <= threshold
}

void function TrueGear_CombatHealthPoll( entity player )
{
	int health = player.GetHealth()
	int maximum = player.GetMaxHealth()
	int delta = health - file.health
	string fields = TrueGear_PlayerFields( player ) + TrueGear_Int( "health", health )
		+ TrueGear_Int( "max_health", maximum ) + TrueGear_Int( "previous", file.health ) + TrueGear_Int( "delta", delta )
	bool low = TrueGear_CombatIsLowHealth( health, maximum )
	if ( file.enabled )
	{
		if ( delta != 0 || maximum != file.maxHealth )
			TrueGearPlay( TrueGear_RecordEvent( "health_changed", fields ) )
		if ( low != file.lowHealth )
			TrueGearPlay( TrueGear_RecordEvent( low ? "health_low" : "health_recovered", fields ) )
		if ( delta > 0 && maximum == file.maxHealth )
		{
			if ( !file.healing )
				TrueGearPlay( TrueGear_RecordEvent( "health_regen_start", fields + TrueGear_Bool( "inferred", true ) ) )
			file.healing = true
			file.lastHealTime = Time()
		}
		// Track the end silently so a later recovery can emit a fresh start.
		if ( file.healing && ( delta < 0 || maximum != file.maxHealth || health >= maximum || Time() - file.lastHealTime >= 0.25 ) )
		{
			file.healing = false
			file.lastHealTime = 0.0
		}
		if ( file.health < file.maxHealth && health >= maximum && maximum > 0 )
			TrueGearPlay( TrueGear_RecordEvent( "health_full", fields ) )
	}
	file.health = health
	file.maxHealth = maximum
	file.lowHealth = low

	entity shieldEntity = IsValid( file.soul ) ? file.soul : player
	int shield = shieldEntity.GetShieldHealth()
	int maxShield = shieldEntity.GetShieldHealthMax()
	int shieldDelta = shield - file.shield
	string shieldFields = TrueGear_PlayerFields( player ) + TrueGear_Int( "shield", shield ) + TrueGear_Int( "max_shield", maxShield )
		+ TrueGear_Int( "previous", file.shield ) + TrueGear_Int( "delta", shieldDelta )
	if ( file.enabled )
	{
		if ( shieldDelta != 0 || maxShield != file.maxShield )
			TrueGearPlay( TrueGear_RecordEvent( "shield_changed", shieldFields ) )
		if ( file.shield > 0 && shield <= 0 )
			TrueGearPlay( TrueGear_RecordEvent( "shield_broken", shieldFields ) )
		if ( file.shield <= 0 && shield > 0 )
			TrueGearPlay( TrueGear_RecordEvent( "shield_restored", shieldFields ) )
		if ( shieldDelta > 0 && maxShield == file.maxShield )
		{
			if ( !file.shieldRegen )
				TrueGearPlay( TrueGear_RecordEvent( "shield_regen_start", shieldFields + TrueGear_Bool( "inferred", true ) ) )
			file.shieldRegen = true
			file.lastShieldRegenTime = Time()
		}
		if ( file.shieldRegen && ( shieldDelta < 0 || maxShield != file.maxShield || shield >= maxShield || Time() - file.lastShieldRegenTime >= 0.25 ) )
		{
			TrueGearPlay( TrueGear_RecordEvent( "shield_regen_end", shieldFields + TrueGear_Bool( "inferred", true ) ) )
			file.shieldRegen = false
		}
		if ( file.shield < file.maxShield && shield >= maxShield && maxShield > 0 )
			TrueGearPlay( TrueGear_RecordEvent( "shield_full", shieldFields ) )
	}
	file.shield = shield
	file.maxShield = maxShield
}

string function TrueGear_CombatWeaponFields( entity player, entity weapon )
{
	return TrueGear_PlayerFields( player ) + TrueGear_EntityFields( weapon, "weapon_entity_" )
		+ TrueGear_String( "weapon", weapon.GetWeaponClassName() )
		+ TrueGear_Int( "clip", weapon.GetWeaponPrimaryClipCount() ) + TrueGear_Int( "clip_max", weapon.GetWeaponPrimaryClipCountMax() )
		+ TrueGear_Int( "reserve", player.GetWeaponAmmoStockpile( weapon ) )
}

// This identifies the weapon hand sampled by the collector. Reload events do
// not identify which hand operated the magazine, bolt, or reload button.
string function TrueGear_CombatWeaponHandFields( int side, int raw, string basis, string timing = "sampled", bool inferred = false )
{
	return TrueGear_HandFields( side, basis, inferred ) + TrueGear_Int( "weapon_hand", raw )
		+ TrueGear_String( "hand_role", "weapon" ) + TrueGear_String( "hand_timing", timing )
}

string function TrueGear_CombatWeaponType( string menuCategory )
{
	switch ( menuCategory )
	{
		case "handgun":
		case "pistol":
			return "pistol"
		case "ar":
			return "rifle"
		case "smg":
			return "smg"
		case "lmg":
			return "lmg"
		case "shotgun":
			return "shotgun"
		case "sniper":
			return "sniper"
		case "special":
			return "special"
		case "at":
			return "anti_titan"
	}
	return "unknown"
}

string function TrueGear_CombatReadWeaponType( entity weapon )
{
	if ( !IsValid( weapon ) )
		return "unknown"
	try
	{
		// The client weapon API returns var; absent or non-string metadata is
		// deliberately left on the existing generic shot route.
		var menuCategory = weapon.GetWeaponInfoFileKeyField( "menu_category" )
		if ( typeof menuCategory == "string" )
			return TrueGear_CombatWeaponType( expect string( menuCategory ) )
	}
	catch ( error )
	{
		return "unknown"
	}
	return "unknown"
}

string function TrueGear_CombatShotEvent( string weaponType )
{
	switch ( weaponType )
	{
		case "pistol":
			return "weapon_shot_pistol"
		case "rifle":
			return "weapon_shot_rifle"
		case "smg":
			return "weapon_shot_smg"
		case "lmg":
			return "weapon_shot_lmg"
		case "shotgun":
			return "weapon_shot_shotgun"
		case "sniper":
			return "weapon_shot_sniper"
		case "special":
			return "weapon_shot_special"
		case "anti_titan":
			return "weapon_shot_anti_titan"
	}
	return "weapon_shot_inferred"
}

void function TrueGear_CombatWeaponPoll( entity player )
{
	entity weapon = player.GetActiveWeapon()
	// Keep the native value for existing consumers. The normalized hand helper
	// excludes Titan/default-hand and empty-hand cases from physical attribution.
	int hand = TrueGear_WeaponHandIndex( player )
	int handRaw = TF2VR_WeaponHand()
	string handFields = TrueGear_CombatWeaponHandFields( hand, handRaw, "weapon_hand_at_sample" )
	int ejections = GetConVarInt( "tf2vr_reload_ejections" )
	int rackedRounds = GetConVarInt( "tf2vr_reload_rounds" )
	bool magazineOut = GetConVarBool( "tf2vr_reload_magazine_out" )
	bool manualAmmoChange = ejections != file.vrEjections || rackedRounds != file.vrRackedRounds || magazineOut || file.vrMagazineOut
	file.vrEjections = ejections
	file.vrRackedRounds = rackedRounds
	file.vrMagazineOut = magazineOut

	if ( weapon != file.weapon )
	{
		if ( file.enabled && file.weaponName != "" )
		{
			string oldFields = TrueGear_PlayerFields( player ) + TrueGear_String( "weapon", file.weaponName ) + TrueGear_String( "reason", "weapon_changed" )
				+ TrueGear_CombatWeaponHandFields( file.weaponHand, file.weaponHandRaw, "weapon_hand_at_previous_sample", "previous_sample" )
			TrueGearPlay( TrueGear_RecordEvent( "weapon_holstered", oldFields ) )
			if ( file.reloading )
				TrueGearPlay( TrueGear_RecordEvent( "weapon_reload_end", oldFields ) )
			if ( file.chargeRising )
				TrueGearPlay( TrueGear_RecordEvent( "weapon_charge_rise_end", oldFields + TrueGear_Bool( "inferred", true ) ) )
		}
		file.weapon = weapon
		file.weaponName = IsValid( weapon ) ? weapon.GetWeaponClassName() : ""
		// Cache classification with the weapon entity, rather than querying
		// weapon metadata on every observation tick.
		file.weaponType = TrueGear_CombatReadWeaponType( weapon )
		file.weaponHand = hand
		file.weaponHandRaw = handRaw
		file.reloading = false
		file.reloadRefilled = false
		file.chargeRising = false
		if ( IsValid( weapon ) )
		{
			file.clip = weapon.GetWeaponPrimaryClipCount()
			file.reserve = player.GetWeaponAmmoStockpile( weapon )
			file.reloading = weapon.IsReloading()
			file.reloadStartClip = file.clip
			file.charge = weapon.GetWeaponChargeFraction()
			file.chargeBucket = int( file.charge * 4.0 )
			if ( file.enabled )
				TrueGearPlay( TrueGear_RecordEvent( "weapon_equipped", TrueGear_CombatWeaponFields( player, weapon ) + handFields + TrueGear_Bool( "baseline", true ) ) )
		}
	}

	bool aiming = player.GetZoomFrac() > 0.01
	if ( aiming != file.aiming && file.enabled )
		TrueGearPlay( TrueGear_RecordEvent( aiming ? "weapon_aim_start" : "weapon_aim_end", TrueGear_PlayerFields( player )
			+ TrueGear_String( "weapon", file.weaponName ) + handFields
			+ TrueGear_Float( "zoom_fraction", player.GetZoomFrac() ) + TrueGear_String( "basis", "game_zoom_not_physical_sight_alignment" ) ) )
	file.aiming = aiming
	if ( !IsValid( weapon ) )
	{
		file.weaponHand = hand
		file.weaponHandRaw = handRaw
		return
	}

	int clip = weapon.GetWeaponPrimaryClipCount()
	int reserve = player.GetWeaponAmmoStockpile( weapon )
	bool reloading = weapon.IsReloading()
	string observationFields = TrueGear_CombatWeaponFields( player, weapon ) + TrueGear_Bool( "native_reloading", reloading )
		+ TrueGear_Bool( "manual_reload_change", manualAmmoChange ) + TrueGear_Bool( "magazine_out", magazineOut )
	string fields = observationFields + handFields
	if ( file.enabled )
	{
		if ( clip != file.clip || reserve != file.reserve )
			TrueGearPlay( TrueGear_RecordEvent( "weapon_ammo_changed", fields + TrueGear_Int( "clip_delta", clip - file.clip )
				+ TrueGear_Int( "reserve_delta", reserve - file.reserve ) ) )
		if ( clip < file.clip && !reloading && !file.reloading && !manualAmmoChange )
		{
			int cost = maxint( 1, weapon.GetWeaponSettingInt( eWeaponVar.ammo_per_shot ) )
			int used = file.clip - clip
			int shots = maxint( 1, used / cost )
			bool handChanged = hand != file.weaponHand
			int shotHand = handChanged ? -1 : hand
			// An ammo delta can include several shots. If the hand changed since
			// the previous sample, no shot in that interval is assigned to a side.
			string shotFields = observationFields
				+ TrueGear_CombatWeaponHandFields( shotHand, handRaw,
					handChanged ? "hand_changed_during_sample" : "weapon_hand_at_sample", "sampled", true )
				+ TrueGear_String( "previous_hand", TrueGear_HandName( file.weaponHand ) ) + TrueGear_Int( "previous_hand_index", file.weaponHand )
				+ TrueGear_String( "current_hand", TrueGear_HandName( hand ) ) + TrueGear_Int( "current_hand_index", hand )
				+ TrueGear_Bool( "hand_changed_during_sample", handChanged )
			int physicalGripMask = player.IsTitan() ? 0 : TrueGear_WeaponGripMask()
			int nativeHandState = player.IsTitan() ? -1 : TrueGear_ReadVRHandState()
			bool bothHands = physicalGripMask == 3
			string shotEvent = player.IsTitan() ? "weapon_shot_inferred" : TrueGear_CombatShotEvent( file.weaponType )
			string shotName = TrueGear_RecordEvent( "weapon_shot_inferred", shotFields + TrueGear_Int( "ammo_used", used )
				+ TrueGear_Int( "ammo_per_shot", cost ) + TrueGear_Int( "estimated_shots", shots )
				+ TrueGear_String( "weapon_type", file.weaponType ) + TrueGear_String( "shot_event", shotEvent )
				+ TrueGear_Bool( "ammo_delta_aligned", used % cost == 0 )
				+ TrueGear_Bool( "inferred", true ) + TrueGear_Bool( "aggregated", true )
				+ TrueGear_Bool( "two_handed", bothHands ) + TrueGear_Int( "physical_grip_mask", physicalGripMask )
				+ TrueGear_Int( "native_hand_state", nativeHandState )
				+ TrueGear_Bool( "native_gun_confirmed", nativeHandState >= 0 && ( nativeHandState & 32 ) != 0 )
				+ TrueGear_Bool( "native_support_held", nativeHandState >= 0 && ( nativeHandState & 16 ) != 0 )
				+ TrueGear_String( "support_basis", "native_support_grip_latch" ) )
			if ( shotName != "" )
			{
				if ( bothHands )
				{
					TrueGearPlay( TrueGear_HandEventName( shotEvent, 0, false ) )
					TrueGearPlay( TrueGear_HandEventName( shotEvent, 1, false ) )
				}
				else
					TrueGearPlay( TrueGear_HandEventName( shotEvent, shotHand, player.IsTitan() ) )
			}
		}
		if ( file.clip > 0 && clip == 0 )
			TrueGearPlay( TrueGear_RecordEvent( "weapon_ammo_empty", fields ) )
		if ( clip > file.clip )
		{
			TrueGearPlay( TrueGear_RecordEvent( "weapon_ammo_refilled", fields + TrueGear_Int( "rounds_added", clip - file.clip ) ) )
			if ( reloading || file.reloading )
				file.reloadRefilled = true
		}
		if ( reloading && !file.reloading )
		{
			file.reloadStartClip = file.clip
			file.reloadRefilled = clip > file.clip
			TrueGearPlay( TrueGear_RecordEvent( "weapon_reload_start", fields ) )
		}
		if ( !reloading && file.reloading )
		{
			TrueGearPlay( TrueGear_RecordEvent( "weapon_reload_end", fields + TrueGear_Bool( "ammo_refilled", file.reloadRefilled ) ) )
			if ( file.reloadRefilled )
				TrueGearPlay( TrueGear_RecordEvent( "weapon_reload_complete_inferred", fields + TrueGear_Bool( "inferred", true )
					+ TrueGear_Int( "clip_at_reload_start", file.reloadStartClip ) ) )
			file.reloadRefilled = false
		}
	}
	file.clip = clip
	file.reserve = reserve
	file.reloading = reloading
	TrueGear_CombatChargePoll( weapon, fields )
	file.weaponHand = hand
	file.weaponHandRaw = handRaw
}

void function TrueGear_CombatChargePoll( entity weapon, string fields )
{
	// Charge fraction can represent charge, heat, or expended energy depending
	// on the weapon. Expose levels/trends without claiming a native fire event.
	float charge = weapon.GetWeaponChargeFraction()
	charge = max( 0.0, min( 1.0, charge ) )
	int bucket = int( charge * 4.0 )
	string chargeFields = fields + TrueGear_Float( "charge_fraction", charge ) + TrueGear_Int( "quarter", bucket )
		+ TrueGear_String( "semantics", "weapon_specific_resource" )
	if ( file.enabled )
	{
		if ( bucket != file.chargeBucket )
			TrueGearPlay( TrueGear_RecordEvent( "weapon_charge_level", chargeFields ) )
		if ( file.charge < 0.999 && charge >= 0.999 )
			TrueGearPlay( TrueGear_RecordEvent( "weapon_charge_fraction_max", chargeFields ) )
		if ( file.charge > 0.001 && charge <= 0.001 )
			TrueGearPlay( TrueGear_RecordEvent( "weapon_charge_fraction_min", chargeFields ) )
		if ( charge > file.charge + 0.002 )
		{
			if ( !file.chargeRising )
				TrueGearPlay( TrueGear_RecordEvent( "weapon_charge_rise_start", chargeFields + TrueGear_Bool( "inferred", true ) ) )
			file.chargeRising = true
			file.lastChargeRiseTime = Time()
		}
		if ( file.chargeRising && ( charge < file.charge - 0.002 || charge >= 0.999 || Time() - file.lastChargeRiseTime >= 0.15 ) )
		{
			TrueGearPlay( TrueGear_RecordEvent( "weapon_charge_rise_end", chargeFields + TrueGear_Bool( "inferred", true ) ) )
			file.chargeRising = false
		}
	}
	file.charge = charge
	file.chargeBucket = bucket
}
