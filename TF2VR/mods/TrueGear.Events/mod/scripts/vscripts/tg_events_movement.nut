// SERVER && SP. Observe only: these callbacks never change player movement.
global function TrueGear_MovementInit

struct TGMovementState
{
	bool lifeActive = false
	bool deathReported = false
	bool initialized = false
	vector previousVelocity = <0, 0, 0>
	float previousSampleTime = 0.0
	vector lastAirVelocity = <0, 0, 0>
	float lastAirSampleTime = -1.0
	float airborneSince = -1.0
	float highestAirZ = 0.0
	float minimumAirVelocity = 0.0
	bool crouched = false
	bool sliding = false
	bool sprinting = false
	bool moving = false
	bool falling = false
	bool wallhanging = false
	bool contextAction = false
	bool execution = false
	bool phaseShifted = false
	float nextPulseTime = 0.0
	float nextHeartbeatTime = 0.0
	string lastPlayerFields = ""
}

struct
{
	bool initialized = false
	table<entity, TGMovementState> players
} file

void function TrueGear_MovementInit()
{
	if ( file.initialized )
		return
	file.initialized = true
	AddSpawnCallback( "player", TrueGear_MovementOnSpawn )
	AddCallback_OnPlayerRespawned( TrueGear_MovementOnRespawn )
	AddCallback_OnPlayerKilled( TrueGear_MovementOnKilled )
	// Also covers a VM/mod initialized after the campaign player already exists.
	foreach ( entity player in GetPlayerArray() )
		TrueGear_MovementAttach( player, "late_attach" )
}

void function TrueGear_MovementOnSpawn( entity player )
{
	TrueGear_MovementAttach( player, "spawn_callback" )
}

void function TrueGear_MovementOnRespawn( entity player )
{
	TrueGear_MovementAttach( player, "respawn_callback" )
}

void function TrueGear_MovementAttach( entity player, string reason )
{
	if ( !IsValid( player ) || !player.IsPlayer() )
		return
	if ( !( player in file.players ) )
	{
		TGMovementState state
		state.lastPlayerFields = TrueGear_PlayerFields( player )
		file.players[player] <- state
		AddPlayerMovementEventCallback( player, ePlayerMovementEvents.JUMP, TrueGear_MovementJump )
		AddPlayerMovementEventCallback( player, ePlayerMovementEvents.DOUBLE_JUMP, TrueGear_MovementDoubleJump )
		AddPlayerMovementEventCallback( player, ePlayerMovementEvents.DODGE, TrueGear_MovementDodge )
		AddPlayerMovementEventCallback( player, ePlayerMovementEvents.LEAVE_GROUND, TrueGear_MovementLeaveGround )
		AddPlayerMovementEventCallback( player, ePlayerMovementEvents.TOUCH_GROUND, TrueGear_MovementLand )
		thread TrueGear_MovementWatch( player )
	}
	TGMovementState state = file.players[player]
	// Spawn and respawn callbacks can describe the same life; register/watch once.
	if ( state.lifeActive )
		return
	if ( IsAlive( player ) )
		TrueGear_MovementBeginLife( player, state, reason )
}

void function TrueGear_MovementBeginLife( entity player, TGMovementState state, string reason )
{
	state.lifeActive = true
	state.deathReported = false
	state.initialized = false
	state.airborneSince = -1.0
	state.minimumAirVelocity = 0.0
	state.lastAirVelocity = <0, 0, 0>
	state.lastAirSampleTime = -1.0
	state.highestAirZ = player.GetOrigin().z
	state.nextPulseTime = Time()
	TrueGear_MovementSampleBaseline( player, state )
	TrueGear_MovementEmit( "player_spawn", player, TrueGear_String( "reason", reason ) )
}

void function TrueGear_MovementSampleBaseline( entity player, TGMovementState state )
{
	state.previousVelocity = player.GetVelocity()
	state.previousSampleTime = Time()
	state.crouched = player.IsCrouched()
	state.sliding = player.IsSliding()
	state.sprinting = player.IsSprinting()
	state.contextAction = player.ContextAction_IsActive()
	state.execution = player.ContextAction_IsMeleeExecution()
	state.phaseShifted = player.IsPhaseShifted()
	state.wallhanging = player.IsWallHanging()
	state.moving = Length( <state.previousVelocity.x, state.previousVelocity.y, 0> ) > 25.0
	state.falling = !player.IsOnGround() && state.previousVelocity.z < -100.0
		&& !player.IsWallRunning() && !player.IsWallHanging() && !player.IsZiplining()
	state.lastPlayerFields = TrueGear_PlayerFields( player )
	if ( !player.IsOnGround() )
		TrueGear_MovementBeginAir( player, state )
	state.initialized = true
}

void function TrueGear_MovementOnKilled( entity victim, entity attacker, var damageInfo )
{
	if ( !TrueGear_MovementLifecyclePlayer( victim ) || !( victim in file.players ) )
		return
	TGMovementState state = file.players[victim]
	if ( state.deathReported || !state.lifeActive )
		return
	state.deathReported = true
	state.lifeActive = false
	TrueGear_MovementEmit( "player_death", victim,
		TrueGear_String( "reason", "killed" ) + TrueGear_EntityFields( attacker, "attacker_" ) )
	TrueGear_MovementEndContinuous( victim, state, "death" )
}

// SP lifecycle feedback belongs to the campaign player, never a viewed NPC or
// an unoccupied Titan. IsPlayer still allows the local player's occupied Titan.
bool function TrueGear_MovementLifecyclePlayer( entity player )
{
	if ( !IsValid( player ) || !player.IsPlayer() )
		return false
	foreach ( entity campaignPlayer in GetPlayerArray() )
	{
		if ( IsValid( campaignPlayer ) && campaignPlayer.IsPlayer() )
			return player == campaignPlayer
	}
	return false
}

string function TrueGear_MovementFields( entity player )
{
	vector velocity = player.GetVelocity()
	return TrueGear_PlayerFields( player )
		+ TrueGear_Float( "speed", Length( velocity ) )
		+ TrueGear_Float( "horizontal_speed", Length( <velocity.x, velocity.y, 0> ) )
		+ TrueGear_Bool( "grounded", player.IsOnGround() )
		+ TrueGear_Bool( "crouched", player.IsCrouched() )
		+ TrueGear_Bool( "sliding", player.IsSliding() )
		+ TrueGear_Bool( "sprinting", player.IsSprinting() )
		+ TrueGear_Bool( "wallhanging", player.IsWallHanging() )
		+ TrueGear_Bool( "context_action", player.ContextAction_IsActive() )
		+ TrueGear_Bool( "execution", player.ContextAction_IsMeleeExecution() )
		+ TrueGear_Bool( "phase_shifted", player.IsPhaseShifted() )
}

void function TrueGear_MovementEmit( string eventName, entity player, string extra = "" )
{
	bool lifecycle = eventName == "player_death" || eventName == "player_spawn"
	if ( !TrueGear_Enabled( lifecycle ? "session" : "movement" ) || !IsValid( player ) || !player.IsPlayer() )
		return
	if ( lifecycle && !TrueGear_MovementLifecyclePlayer( player ) )
		return
	string name = TrueGear_RecordEvent( eventName, TrueGear_MovementFields( player ) + extra )
	// CLIENT always maintains slide output, including restored SERVER saves.
	if ( eventName != "movement_slide_start" && eventName != "movement_slide_stop" )
		TrueGearPlay( name )
}

void function TrueGear_MovementBeginAir( entity player, TGMovementState state )
{
	if ( state.airborneSince >= 0.0 )
		return
	state.airborneSince = Time()
	state.highestAirZ = player.GetOrigin().z
	state.minimumAirVelocity = 0.0
}

void function TrueGear_MovementJump( entity player )
{
	if ( player in file.players )
		TrueGear_MovementBeginAir( player, file.players[player] )
	TrueGear_MovementEmit( "movement_jump", player )
}

void function TrueGear_MovementDoubleJump( entity player )
{
	TrueGear_MovementEmit( "movement_double_jump", player )
}

void function TrueGear_MovementDodge( entity player )
{
	TrueGear_MovementEmit( "movement_dash", player )
}

void function TrueGear_MovementLeaveGround( entity player )
{
	if ( player in file.players )
		TrueGear_MovementBeginAir( player, file.players[player] )
	TrueGear_MovementEmit( "movement_leave_ground", player )
}

void function TrueGear_MovementLand( entity player )
{
	if ( !IsValid( player ) || !( player in file.players ) )
		return
	TGMovementState state = file.players[player]
	float age = state.lastAirSampleTime >= 0.0 ? Time() - state.lastAirSampleTime : -1.0
	float impactSpeed = max( 0.0, -state.lastAirVelocity.z )
	// The callback may run after the engine has already zeroed vertical speed.
	// Use the last sample, and identify the estimate/age instead of fabricating a force.
	if ( age < 0.0 || age > 0.2 )
		impactSpeed = 0.0
	float airTime = state.airborneSince >= 0.0 ? Time() - state.airborneSince : 0.0
	float fallDistance = max( 0.0, state.highestAirZ - player.GetOrigin().z )
	if ( state.airborneSince >= 0.0 && TrueGear_ShouldEmitLanding( fallDistance ) )
		TrueGear_MovementEmit( "movement_land", player,
			TrueGear_Float( "impact_speed_estimate", impactSpeed )
			+ TrueGear_Float( "peak_fall_speed", max( 0.0, -state.minimumAirVelocity ) )
			+ TrueGear_Float( "air_time", airTime ) + TrueGear_Float( "fall_distance", fallDistance )
			+ TrueGear_Float( "sample_age", age ) + TrueGear_Bool( "estimated", true )
			+ TrueGear_Bool( "impact_estimate_valid", age >= 0.0 && age <= 0.2 )
			+ TrueGear_Bool( "heavy", age >= 0.0 && age <= 0.2 && impactSpeed >= GetConVarFloat( "tg_events_heavy_land_speed" ) )
			+ TrueGear_String( "estimate_method", "pre_touch_velocity_sample" ) )
	// A filtered step still ends this segment, so later steps cannot add to it.
	state.airborneSince = -1.0
	state.minimumAirVelocity = 0.0
	state.lastAirVelocity = <0, 0, 0>
	state.lastAirSampleTime = -1.0
	state.highestAirZ = player.GetOrigin().z
}

void function TrueGear_MovementTransition( entity player, string name, bool previous, bool current )
{
	if ( previous != current )
		TrueGear_MovementEmit( name + ( current ? "_start" : "_stop" ), player )
}

void function TrueGear_MovementEndContinuous( entity player, TGMovementState state, string reason )
{
	string fields = TrueGear_String( "reason", reason )
	if ( state.crouched )
		TrueGear_MovementEmit( "movement_crouch_stop", player, fields )
	if ( state.sliding )
		TrueGear_MovementEmit( "movement_slide_stop", player, fields )
	if ( state.sprinting )
		TrueGear_MovementEmit( "movement_sprint_stop", player, fields )
	if ( state.moving )
		TrueGear_MovementEmit( "movement_move_stop", player, fields )
	if ( state.falling )
		TrueGear_MovementEmit( "movement_fall_stop", player, fields )
	if ( state.contextAction )
		TrueGear_MovementEmit( "player_context_action_stop", player, fields )
	if ( state.execution )
		TrueGear_MovementEmit( "player_execution_stop", player, fields )
	if ( state.phaseShifted )
		TrueGear_MovementEmit( "player_phase_shift_stop", player, fields )
	state.crouched = false
	state.sliding = false
	state.sprinting = false
	state.moving = false
	state.falling = false
	state.wallhanging = false
	state.contextAction = false
	state.execution = false
	state.phaseShifted = false
}

void function TrueGear_MovementWatch( entity player )
{
	player.EndSignal( "OnDestroy" )
	OnThreadEnd(
		function() : ( player )
		{
			if ( player in file.players )
			{
				if ( TrueGear_Enabled( "movement" ) )
					TrueGearPlay( TrueGear_RecordEvent( "player_despawn", file.players[player].lastPlayerFields + TrueGear_String( "reason", "watch_ended" ) ) )
				delete file.players[player]
			}
		}
	)
	while ( IsValid( player ) )
	{
		wait TrueGear_PollInterval()
		if ( !IsValid( player ) )
			break
		TGMovementState state = file.players[player]
		if ( Time() >= state.nextHeartbeatTime )
		{
			ServerToClientStringCommand( player, "TrueGear_MovementReady" )
			state.nextHeartbeatTime = Time() + 0.25
		}
		if ( !IsAlive( player ) )
		{
			if ( state.lifeActive && !state.deathReported )
			{
				state.lifeActive = false
				state.deathReported = true
				TrueGear_MovementEmit( "player_death", player, TrueGear_String( "reason", "not_alive" ) )
				TrueGear_MovementEndContinuous( player, state, "death" )
			}
			continue
		}
		if ( !state.lifeActive )
			TrueGear_MovementBeginLife( player, state, "alive_transition" )
		if ( !state.initialized )
			TrueGear_MovementSampleBaseline( player, state )

		vector velocity = player.GetVelocity()
		bool grounded = player.IsOnGround()
		bool crouched = player.IsCrouched()
		bool sliding = player.IsSliding()
		bool sprinting = player.IsSprinting()
		bool contextAction = player.ContextAction_IsActive()
		bool execution = player.ContextAction_IsMeleeExecution()
		bool phaseShifted = player.IsPhaseShifted()
		float horizontalSpeed = Length( <velocity.x, velocity.y, 0> )
		// Hysteresis prevents stop/start chatter near rest or the fall threshold.
		bool moving = horizontalSpeed > ( state.moving ? 15.0 : 25.0 )
		bool falling = !grounded && !player.IsWallRunning() && !player.IsWallHanging() && !player.IsZiplining()
			&& velocity.z < ( state.falling ? -50.0 : -100.0 )
		TrueGear_MovementTransition( player, "movement_crouch", state.crouched, crouched )
		TrueGear_MovementTransition( player, "movement_slide", state.sliding, sliding )
		TrueGear_MovementTransition( player, "movement_sprint", state.sprinting, sprinting )
		TrueGear_MovementTransition( player, "movement_move", state.moving, moving )
		TrueGear_MovementTransition( player, "movement_fall", state.falling, falling )
		TrueGear_MovementTransition( player, "player_context_action", state.contextAction, contextAction )
		TrueGear_MovementTransition( player, "player_execution", state.execution, execution )
		TrueGear_MovementTransition( player, "player_phase_shift", state.phaseShifted, phaseShifted )
		state.crouched = crouched
		state.sliding = sliding
		state.sprinting = sprinting
		state.moving = moving
		state.falling = falling
		state.contextAction = contextAction
		state.execution = execution
		state.wallhanging = player.IsWallHanging()
		state.phaseShifted = phaseShifted
		if ( !grounded )
		{
			TrueGear_MovementBeginAir( player, state )
			state.highestAirZ = max( state.highestAirZ, player.GetOrigin().z )
			state.minimumAirVelocity = min( state.minimumAirVelocity, velocity.z )
			state.lastAirVelocity = velocity
			state.lastAirSampleTime = Time()
		}
		if ( TrueGear_Enabled( "movement" ) && GetConVarBool( "tg_events_continuous" ) && Time() >= state.nextPulseTime )
		{
			if ( moving || !grounded || player.IsWallHanging() )
				TrueGear_MovementEmit( "movement_pulse", player,
					TrueGear_Bool( "falling", falling ) )
			state.nextPulseTime = Time() + max( 0.1, GetConVarFloat( "tg_events_pulse_interval" ) )
		}
		state.previousVelocity = velocity
		state.previousSampleTime = Time()
		state.lastPlayerFields = TrueGear_PlayerFields( player )
	}
}
