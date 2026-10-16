// Origin: TrueGear.Events (original implementation).
// Observes CircuitLord's existing convars and native weapon/animation state.
// Candidate pickup/holster availability are NOT reported as successful grabs.
global function TrueGear_VRInit

struct TrueGear_AbilityState
{
	entity weapon
	int clip = -1
	float cooldown = 0.0
	int resourceBucket = -1
}

struct
{
	entity player
	bool baseline = false
	table<string, int> counters
	array<TrueGear_AbilityState> abilities
	float cycle = -1.0
	float lastStep = -1.0
	entity soul
	int coreBucket = -1
	bool coreActive = false
} file

void function TrueGear_VRInit()
{
	for ( int slot = 0; slot < OFFHAND_COUNT; slot++ )
	{
		TrueGear_AbilityState state
		file.abilities.append( state )
	}
	thread TrueGear_VRWatch_Threaded()
}

// Per-tick error isolation preserves the stream if a map-specific resource is absent.
void function TrueGear_VRWatch_Threaded()
{
	while ( true )
	{
		wait TrueGear_PollInterval()
		try
		{
			TrueGear_VRTick()
		}
		catch ( error )
		{
		}
	}
}

void function TrueGear_VRTick()
{
	entity player = GetLocalViewPlayer()
	if ( !IsValid( player ) || !IsAlive( player ) )
	{
		file.player = null
		file.baseline = false
		file.cycle = -1.0
		file.soul = null
		return
	}
	bool first = !file.baseline || file.player != player
	if ( first )
	{
		file.player = player
		file.counters.clear()
		file.cycle = -1.0
		file.soul = null
		foreach ( state in file.abilities )
			state.weapon = null
	}

	// Counter deltas preserve multiple occurrences between samples. A reset is baseline only.
	TrueGear_Counter( player, "tf2vr_reload_ejections", "vr_reload_magazine_ejected", first )
	TrueGear_Counter( player, "tf2vr_reload_rounds", "vr_reload_round_ejected", first )
	TrueGear_Counter( player, "tf2vr_reload_empty_grabs", "vr_reload_empty_pouch", first )
	TrueGear_Counter( player, "tf2vr_recenters", "vr_recenter", first )
	TrueGear_Toggle( player, "tf2vr_reload_magazine_out", "vr_reload_magazine_out", "vr_reload_magazine_out_cleared", first )
	TrueGear_Toggle( player, "tf2vr_reload_highlight", "vr_reload_guide_started", "vr_reload_guide_stopped", first )
	TrueGear_Toggle( player, "tf2vr_chest_visible", "vr_chest_visible", "vr_chest_hidden", first )
	TrueGear_Ack( player, "tf2vr_gun_ack", "vr_weapon_equip_accepted", "vr_weapon_equip_rejected", first )
	TrueGear_Ack( player, "tf2vr_grenade_ack", "vr_grenade_grab_accepted", "vr_grenade_grab_rejected", first )
	TrueGear_PickupTarget( player, "left", first )
	TrueGear_PickupTarget( player, "right", first )
	TrueGear_HolsterAvailability( player, first )
	TrueGear_Abilities( player, first )
	TrueGear_TitanMotion( player )
	file.baseline = true
}

string function TrueGear_VRFields( entity player, bool grenade = false )
{
	string fields = TrueGear_PlayerFields( player )
	entity weapon = grenade ? player.GetOffhandWeapon( OFFHAND_ORDNANCE ) : player.GetActiveWeapon()
	if ( IsValid( weapon ) )
		fields += TrueGear_String( "weapon", weapon.GetWeaponClassName() ) + TrueGear_Int( "weapon_id", weapon.GetEntIndex() )
	return fields
}

void function TrueGear_VREmit( string name, string fields )
{
	if ( TrueGear_Enabled( "vr" ) )
		TrueGearPlay( TrueGear_RecordEvent( name, fields ) )
}

// Reload counters expose the gun context but do not expose the acting controller.
string function TrueGear_ReloadHandFields( entity player )
{
	return TrueGear_HandFields( -1, "operation_hand_not_exposed" ) + TrueGear_String( "hand_role", "actor" )
		+ TrueGear_Int( "weapon_hand", TF2VR_WeaponHand() ) + TrueGear_String( "weapon_hand_name", TrueGear_HandName( TrueGear_WeaponHandIndex( player ) ) )
}

void function TrueGear_Counter( entity player, string key, string eventName, bool first )
{
	int now = GetConVarInt( key )
	if ( !first && key in file.counters && now > file.counters[key] )
	{
		string fields = TrueGear_VRFields( player ) + TrueGear_Int( "count", now - file.counters[key] ) + TrueGear_Int( "counter", now )
		if ( key != "tf2vr_recenters" )
			fields += TrueGear_ReloadHandFields( player )
		TrueGear_VREmit( eventName, fields )
	}
	file.counters[key] <- now
}

void function TrueGear_Toggle( entity player, string key, string onName, string offName, bool first )
{
	int now = GetConVarInt( key ) != 0 ? 1 : 0
	if ( !first && key in file.counters && now != file.counters[key] )
	{
		string fields = TrueGear_VRFields( player ) + TrueGear_Bool( "active", now != 0 )
		if ( key == "tf2vr_reload_magazine_out" || key == "tf2vr_reload_highlight" )
			fields += TrueGear_ReloadHandFields( player )
		TrueGear_VREmit( now != 0 ? onName : offName, fields )
	}
	file.counters[key] <- now
}

// A negative ticket is an initial pending/failure value. Wait before declaring rejection.
void function TrueGear_Ack( entity player, string key, string accepted, string rejected, bool first )
{
	int now = GetConVarInt( key )
	string pending = key + "_pending"
	string deadline = key + "_deadline_ms"
	if ( first || !( key in file.counters ) )
	{
		file.counters[key] <- now
		file.counters[pending] <- 0
		file.counters[deadline] <- 0
		return
	}
	if ( now != file.counters[key] )
	{
		if ( now > 0 )
		{
			string fields = TrueGear_VRFields( player, key == "tf2vr_grenade_ack" ) + TrueGear_Int( "ticket", now )
			if ( key == "tf2vr_gun_ack" )
			{
				fields += TrueGear_String( "equipped_weapon", GetConVarString( "tf2vr_gun_name" ) )
					+ TrueGear_Int( "equipped_slot", GetConVarInt( "tf2vr_gun_slot" ) )
				entity active = player.GetActiveWeapon()
				int side = IsValid( active ) && active.GetWeaponClassName() == GetConVarString( "tf2vr_gun_name" ) ? TrueGear_WeaponHandIndex( player ) : -1
				fields += TrueGear_HandFields( side, "weapon_hand_at_ack_sample", true ) + TrueGear_String( "hand_role", "weapon" )
			}
			else
				fields += TrueGear_Bool( "consumes_ammo", GetConVarBool( "tf2vr_grenade_consumes_ammo" ) )
					+ TrueGear_HandFields( -1, "operation_hand_not_exposed" ) + TrueGear_String( "hand_role", "actor" )
			TrueGear_VREmit( accepted, fields )
			file.counters[pending] = 0
		}
		else if ( now < 0 )
		{
			file.counters[pending] = -now
			file.counters[deadline] = int( Time() * 1000.0 ) + 250
		}
	}
	if ( file.counters[pending] > 0 && int( Time() * 1000.0 ) >= file.counters[deadline] )
	{
		TrueGear_VREmit( rejected, TrueGear_VRFields( player, key == "tf2vr_grenade_ack" ) + TrueGear_Int( "ticket", file.counters[pending] )
			+ TrueGear_HandFields( -1, "operation_hand_not_exposed" ) + TrueGear_String( "hand_role", "actor" )
			+ TrueGear_Bool( "inferred", true ) + TrueGear_String( "reason", "negative_ack_persisted_250ms" ) )
		file.counters[pending] = 0
	}
	file.counters[key] = now
}

void function TrueGear_PickupTarget( entity player, string hand, bool first )
{
	string key = "tf2vr_pickup_" + hand
	int now = GetConVarInt( key )
	if ( !first && key in file.counters && now != file.counters[key] )
	{
		string fields = TrueGear_VRFields( player ) + TrueGear_HandFields( hand == "left" ? 0 : 1, "pickup_target_side" )
			+ TrueGear_String( "hand_role", "actor" )
		if ( file.counters[key] != -1 )
			TrueGear_VREmit( "vr_pickup_target_left", fields + TrueGear_Int( "target_id", file.counters[key] ) )
		if ( now != -1 )
			TrueGear_VREmit( "vr_pickup_target_entered", fields + TrueGear_Int( "target_id", now )
				+ TrueGear_Int( "kind", GetConVarInt( "tf2vr_pickup_kind_" + hand ) ) )
	}
	file.counters[key] <- now
}

void function TrueGear_HolsterAvailability( entity player, bool first )
{
	string key = "tf2vr_holster_mask"
	int now = GetConVarInt( key )
	if ( !first && key in file.counters && now != file.counters[key] )
	{
		for ( int slot = 0; slot <= 4; slot++ )
		{
			bool was = ( file.counters[key] & ( 1 << slot ) ) != 0
			bool available = ( now & ( 1 << slot ) ) != 0
			if ( was != available )
				TrueGear_VREmit( available ? "vr_holster_available" : "vr_holster_unavailable",
					TrueGear_VRFields( player ) + TrueGear_Int( "slot", slot ) + TrueGear_Int( "mask", now ) )
		}
	}
	file.counters[key] <- now
}

// Slot metadata retains weapon class so shield, tactical, grenade, core, etc. can be mapped separately.
void function TrueGear_Abilities( entity player, bool first )
{
	for ( int slot = 0; slot < OFFHAND_COUNT; slot++ )
	{
		TrueGear_AbilityState previous = file.abilities[slot]
		entity weapon = player.GetOffhandWeapon( slot )
		if ( !IsValid( weapon ) )
		{
			if ( previous.weapon != null && TrueGear_Enabled( player.IsTitan() ? "titan" : "combat" ) )
				TrueGearPlay( TrueGear_RecordEvent( "ability_removed", TrueGear_PlayerFields( player ) + TrueGear_Int( "slot", slot ) ) )
			previous.weapon = null
			continue
		}
		int clip = weapon.GetWeaponPrimaryClipCount()
		float cooldown = max( 0.0, weapon.GetNextAttackAllowedTime() - Time() )
		float resource = weapon.GetWeaponChargeFraction()
		int bucket = int( clamp( resource, 0.0, 1.0 ) * 10.0 )
		string fields = TrueGear_PlayerFields( player ) + TrueGear_Int( "slot", slot )
			+ TrueGear_String( "weapon", weapon.GetWeaponClassName() ) + TrueGear_Int( "weapon_id", weapon.GetEntIndex() )
			+ TrueGear_Int( "clip", clip ) + TrueGear_Float( "cooldown", cooldown ) + TrueGear_Float( "charge_fraction", resource )
			+ TrueGear_String( "semantics", "weapon_specific_charge_fraction_not_remaining_energy" )
		bool output = TrueGear_Enabled( player.IsTitan() ? "titan" : "combat" )
		if ( previous.weapon != weapon || first )
		{
			if ( output )
				TrueGearPlay( TrueGear_RecordEvent( "ability_equipped", fields + TrueGear_Bool( "baseline", first ) ) )
		}
		else if ( output )
		{
			if ( clip >= 0 && previous.clip >= 0 && clip < previous.clip && weapon != player.GetActiveWeapon() )
				TrueGearPlay( TrueGear_RecordEvent( "ability_ammo_consumed", fields + TrueGear_Int( "ammo_used", previous.clip - clip ) + TrueGear_Bool( "inferred", true ) ) )
			if ( clip > previous.clip && previous.clip >= 0 )
				TrueGearPlay( TrueGear_RecordEvent( "ability_ammo_restored", fields + TrueGear_Int( "ammo_added", clip - previous.clip ) ) )
			if ( previous.cooldown <= 0.02 && cooldown > 0.05 )
				TrueGearPlay( TrueGear_RecordEvent( "ability_cooldown_start", fields + TrueGear_Bool( "inferred", true ) ) )
			if ( previous.cooldown > 0.02 && cooldown <= 0.02 )
				TrueGearPlay( TrueGear_RecordEvent( "ability_cooldown_end", fields ) )
			if ( previous.resourceBucket != bucket )
			{
				TrueGearPlay( TrueGear_RecordEvent( "ability_charge_fraction_changed", fields ) )
				if ( bucket == 10 )
					TrueGearPlay( TrueGear_RecordEvent( "ability_charge_fraction_max", fields ) )
				else if ( bucket == 0 )
					TrueGearPlay( TrueGear_RecordEvent( "ability_charge_fraction_min", fields ) )
			}
		}
		previous.weapon = weapon
		previous.clip = clip
		previous.cooldown = cooldown
		previous.resourceBucket = bucket
	}
}

void function TrueGear_TitanMotion( entity player )
{
	if ( !player.IsTitan() )
	{
		file.cycle = -1.0
		file.soul = null
		return
	}
	float cycle = player.GetCycle()
	vector velocity = player.GetVelocity()
	float speed = sqrt( velocity.x * velocity.x + velocity.y * velocity.y )
	if ( file.cycle >= 0.0 && player.IsOnGround() && speed > 25.0 )
	{
		float advance = cycle - file.cycle
		if ( advance < 0.0 )
			advance += 1.0
		bool left = file.cycle > cycle
		bool right = file.cycle < 0.5 && cycle >= 0.5
		if ( advance < 0.3 && ( left || right ) && Time() - file.lastStep > 0.15 && TrueGear_Enabled( "titan" ) )
		{
			TrueGearPlay( TrueGear_RecordEvent( "titan_footstep_inferred", TrueGear_PlayerFields( player ) + TrueGear_String( "foot", left ? "cycle_0" : "cycle_half" )
				+ TrueGear_Float( "speed", speed ) + TrueGear_Float( "cycle", cycle ) + TrueGear_Bool( "inferred", true ) ) )
			file.lastStep = Time()
		}
	}
	file.cycle = cycle
	entity soul = player.GetTitanSoul()
	if ( !IsValid( soul ) )
		return
	float core = soul.GetTitanSoulNetFloat( "coreAvailableFrac" )
	int bucket = int( clamp( core, 0.0, 1.0 ) * 10.0 )
	bool active = soul.GetCoreChargeExpireTime() > Time()
	if ( file.soul == soul && TrueGear_Enabled( "titan" ) )
	{
		string fields = TrueGear_PlayerFields( player ) + TrueGear_Float( "charge", core ) + TrueGear_Int( "soul_id", soul.GetEntIndex() )
		if ( file.coreBucket != bucket )
		{
			TrueGearPlay( TrueGear_RecordEvent( "titan_core_charge_changed", fields ) )
			if ( bucket == 10 )
				TrueGearPlay( TrueGear_RecordEvent( "titan_core_ready", fields ) )
		}
		if ( file.coreActive != active )
			TrueGearPlay( TrueGear_RecordEvent( active ? "titan_core_active_start" : "titan_core_active_end", fields + TrueGear_Bool( "inferred", true ) ) )
	}
	file.soul = soul
	file.coreBucket = bucket
	file.coreActive = active
}

