untyped

// CLIENT && SP. Old saves restore their original SERVER script set, while
// CLIENT mod scripts load normally. Observe replicated state without compiling
// into the restored interpreter or changing input/gameplay.
global function TrueGear_ClientMovementInit

struct TrueGear_ClientMovementState
{
	bool alive = false
	vector velocity = <0, 0, 0>
	vector origin = <0, 0, 0>
	bool grounded = false
	bool doubleJumping = false
	bool dodging = false
	bool mantling = false
	bool traversing = false
	bool crouched = false
	bool sliding = false
	bool sprinting = false
	bool wallhanging = false
	bool ziplining = false
	bool contextAction = false
	bool execution = false
	bool phaseShifted = false
	bool moving = false
	bool falling = false
}

struct
{
	bool initialized = false
	entity player = null
	TrueGear_ClientMovementState previous
	bool haveBaseline = false
	float lastSample = -1.0
	float lastServerHeartbeat = -100.0
	float nextPulse = 0.0
	float airStarted = -1.0
	float highestAirZ = 0.0
	float peakFallSpeed = 0.0
	bool slideFeedbackActive = false
	float nextSlideRefresh = 0.0
	entity lifePlayer = null
	bool haveLifeBaseline = false
	bool lifeAlive = false
} file

void function TrueGear_ClientMovementInit()
{
	if ( file.initialized )
		return
	file.initialized = true
	AddServerToClientStringCommandCallback( "TrueGear_MovementReady", TrueGear_ClientMovementServerReady )
	thread TrueGear_ClientMovementWatch()
}

void function TrueGear_ClientMovementServerReady( array<string> args )
{
	file.lastServerHeartbeat = Time()
}

// Track the actual local player's life independently of movement/perspective
// sampling. A death camera may already be viewing another entity when the
// local player's replicated alive flag changes. Never observe that view target.
void function TrueGear_ClientLifecycleTick( entity player )
{
	if ( !IsValid( player ) || !player.IsPlayer() || player != GetLocalClientPlayer() )
	{
		file.lifePlayer = null
		file.haveLifeBaseline = false
		return
	}
	bool alive = IsAlive( player )
	if ( !file.haveLifeBaseline || player != file.lifePlayer )
	{
		file.lifePlayer = player
		file.haveLifeBaseline = true
		file.lifeAlive = alive
		// An existing live player is a rearm boundary; attaching to an already
		// dead entity must not manufacture a new death event.
		if ( alive && TrueGear_Enabled( "session" ) )
			TrueGearPlay( TrueGear_RecordEvent( "player_spawn", TrueGear_PlayerFields( player )
				+ TrueGear_String( "reason", "client_life_baseline" ) ) )
		return
	}
	if ( alive == file.lifeAlive )
		return
	file.lifeAlive = alive
	if ( TrueGear_Enabled( "session" ) )
		TrueGearPlay( TrueGear_RecordEvent( alive ? "player_spawn" : "player_death", TrueGear_PlayerFields( player )
			+ TrueGear_String( "reason", "client_alive_transition" ) ) )
}

TrueGear_ClientMovementState function TrueGear_ClientMovementSample( entity player )
{
	vector velocity = player.GetVelocity()
	bool grounded = player.IsOnGround()
	bool wallhanging = player.IsWallHanging()
	// Keep traversal classification only; zipline has no dedicated output.
	bool ziplining = player.IsZiplining()
	TrueGear_ClientMovementState state
	state.alive = IsAlive( player )
	state.velocity = velocity
	state.origin = player.GetOrigin()
	state.grounded = grounded
	state.doubleJumping = player.IsDoubleJumping()
	state.dodging = player.IsDodging()
	// Mantling only classifies traversal so it cannot infer a ground jump.
	state.mantling = player.IsMantling()
	state.traversing = player.IsTraversing()
	state.crouched = player.IsCrouched()
	state.sliding = player.IsSliding()
	state.sprinting = player.IsSprinting()
	state.wallhanging = wallhanging
	state.ziplining = ziplining
	state.contextAction = player.ContextAction_IsActive()
	state.execution = player.ContextAction_IsMeleeExecution()
	state.phaseShifted = player.IsPhaseShifted()
	state.moving = Length( <velocity.x, velocity.y, 0> ) > ( file.haveBaseline && file.previous.moving ? 15.0 : 25.0 )
	state.falling = !grounded && !player.IsWallRunning() && !wallhanging && !ziplining
		&& velocity.z < ( file.haveBaseline && file.previous.falling ? -50.0 : -100.0 )
	return state
}

void function TrueGear_ClientMovementEmit( string name, entity player, TrueGear_ClientMovementState sample, string extra = "" )
{
	// The SERVER watcher sends a heartbeat only while its callbacks/watch are
	// running. Always sample locally, so takeover cannot replay old transitions.
	if ( Time() - file.lastServerHeartbeat < 0.75 || !TrueGear_Enabled( "movement" ) )
		return
	vector velocity = sample.velocity
	string eventName = TrueGear_RecordEvent( name, TrueGear_PlayerFields( player )
		+ TrueGear_Bool( "grounded", sample.grounded ) + TrueGear_Float( "speed", Length( velocity ) )
		+ TrueGear_Vector( "velocity", velocity ) + TrueGear_String( "detection", "client_state_poll" ) + extra )
	// Sliding playback is maintained by the fresh CLIENT state below, even
	// when SERVER handles ordinary movement or a save is loaded mid-slide.
	if ( name != "movement_slide_start" && name != "movement_slide_stop" )
		TrueGearPlay( eventName )
}

void function TrueGear_ClientSlideFeedback( bool active )
{
	if ( !active )
	{
		if ( file.slideFeedbackActive )
			TrueGear_ContinuousPlay( "movement_slide_start", false )
		file.slideFeedbackActive = false
		return
	}
	if ( !file.slideFeedbackActive || Time() >= file.nextSlideRefresh )
	{
		TrueGear_ContinuousPlay( "movement_slide_start", true )
		file.slideFeedbackActive = true
		file.nextSlideRefresh = Time() + 0.1
	}
}

void function TrueGear_ClientMovementTransition( entity player, TrueGear_ClientMovementState current, string name, bool previous, bool active )
{
	if ( previous != active )
		TrueGear_ClientMovementEmit( name + ( active ? "_start" : "_stop" ), player, current )
}

// Respawn Squirrel embeds value structs in file; copy their fields rather than
// assigning the compound member. This also keeps each prior sample independent.
void function TrueGear_ClientMovementStore( TrueGear_ClientMovementState current )
{
	file.previous.alive = current.alive
	file.previous.velocity = current.velocity
	file.previous.origin = current.origin
	file.previous.grounded = current.grounded
	file.previous.doubleJumping = current.doubleJumping
	file.previous.dodging = current.dodging
	file.previous.mantling = current.mantling
	file.previous.traversing = current.traversing
	file.previous.crouched = current.crouched
	file.previous.sliding = current.sliding
	file.previous.sprinting = current.sprinting
	file.previous.wallhanging = current.wallhanging
	file.previous.ziplining = current.ziplining
	file.previous.contextAction = current.contextAction
	file.previous.execution = current.execution
	file.previous.phaseShifted = current.phaseShifted
	file.previous.moving = current.moving
	file.previous.falling = current.falling
}

void function TrueGear_ClientMovementBaseline( entity player, TrueGear_ClientMovementState current )
{
	TrueGear_ClientMovementStore( current )
	file.haveBaseline = true
	file.lastSample = Time()
	file.airStarted = current.grounded ? -1.0 : Time()
	file.highestAirZ = current.origin.z
	file.peakFallSpeed = 0.0
	file.nextPulse = Time()
}

// A ledge exit has no upward impulse. Exclude native traversal states, Titan
// dodges and double jumps. Airborne impulses do not infer another ground jump.
bool function TrueGear_ClientMovementIsJump( TrueGear_ClientMovementState previous, TrueGear_ClientMovementState current, bool titan )
{
	if ( titan || current.doubleJumping || current.dodging || current.mantling
		|| current.traversing || current.ziplining || current.velocity.z <= 80.0 )
		return false
	return previous.grounded && !current.grounded
}

void function TrueGear_ClientMovementUpdate( entity player, TrueGear_ClientMovementState previous, TrueGear_ClientMovementState current )
{
	if ( current.alive != previous.alive )
	{
		// Lifecycle forwarding uses the separate local-player tracker, without
		// SERVER heartbeat suppression or movement-category gating.
		TrueGear_ClientMovementBaseline( player, current )
		return
	}
	if ( !current.alive )
		return
	if ( previous.grounded && !current.grounded )
	{
		file.airStarted = Time()
		file.highestAirZ = current.origin.z
		file.peakFallSpeed = 0.0
		TrueGear_ClientMovementEmit( "movement_leave_ground", player, current )
	}
	if ( TrueGear_ClientMovementIsJump( previous, current, player.IsTitan() ) )
		TrueGear_ClientMovementEmit( "movement_jump", player, current, TrueGear_Bool( "inferred", true ) )
	if ( !previous.doubleJumping && current.doubleJumping )
		TrueGear_ClientMovementEmit( "movement_double_jump", player, current )
	if ( !previous.dodging && current.dodging )
		TrueGear_ClientMovementEmit( "movement_dash", player, current )
	if ( !previous.grounded && current.grounded )
	{
		float impact = max( 0.0, -previous.velocity.z )
		float fallDistance = max( 0.0, file.highestAirZ - current.origin.z )
		if ( file.airStarted >= 0.0 && TrueGear_ShouldEmitLanding( fallDistance ) )
			TrueGear_ClientMovementEmit( "movement_land", player, current,
				TrueGear_Float( "impact_speed_estimate", impact ) + TrueGear_Float( "peak_fall_speed", file.peakFallSpeed )
				+ TrueGear_Float( "air_time", Time() - file.airStarted )
				+ TrueGear_Float( "fall_distance", fallDistance )
				+ TrueGear_Bool( "estimated", true ) + TrueGear_Bool( "heavy", impact >= GetConVarFloat( "tg_events_heavy_land_speed" ) ) )
		// End every segment, including filtered steps, before another begins.
		file.airStarted = -1.0
		file.highestAirZ = current.origin.z
		file.peakFallSpeed = 0.0
	}
	TrueGear_ClientMovementTransition( player, current, "movement_crouch", previous.crouched, current.crouched )
	TrueGear_ClientMovementTransition( player, current, "movement_slide", previous.sliding, current.sliding )
	TrueGear_ClientMovementTransition( player, current, "movement_sprint", previous.sprinting, current.sprinting )
	TrueGear_ClientMovementTransition( player, current, "movement_move", previous.moving, current.moving )
	TrueGear_ClientMovementTransition( player, current, "movement_fall", previous.falling, current.falling )
	TrueGear_ClientMovementTransition( player, current, "player_context_action", previous.contextAction, current.contextAction )
	TrueGear_ClientMovementTransition( player, current, "player_execution", previous.execution, current.execution )
	TrueGear_ClientMovementTransition( player, current, "player_phase_shift", previous.phaseShifted, current.phaseShifted )
	if ( !current.grounded )
	{
		file.highestAirZ = max( file.highestAirZ, current.origin.z )
		file.peakFallSpeed = max( file.peakFallSpeed, -current.velocity.z )
	}
	if ( GetConVarBool( "tg_events_continuous" ) && Time() >= file.nextPulse )
	{
		if ( current.moving || !current.grounded || current.wallhanging )
			TrueGear_ClientMovementEmit( "movement_pulse", player, current )
		file.nextPulse = Time() + max( 0.1, GetConVarFloat( "tg_events_pulse_interval" ) )
	}
}

void function TrueGear_ClientMovementWatch()
{
	while ( true )
	{
		wait TrueGear_PollInterval()
		try
		{
			entity player = GetLocalClientPlayer()
			TrueGear_ClientLifecycleTick( player )
			if ( !IsValid( player ) || GetLocalViewPlayer() != player )
			{
				TrueGear_ClientSlideFeedback( false )
				file.player = null
				file.haveBaseline = false
				continue
			}
			TrueGear_ClientMovementState current = TrueGear_ClientMovementSample( player )
			TrueGear_ClientSlideFeedback( current.alive && current.sliding && !player.IsTitan() && TrueGear_Enabled( "movement" ) )
			if ( player != file.player || !file.haveBaseline || Time() - file.lastSample > 0.5 )
			{
				file.player = player
				TrueGear_ClientMovementBaseline( player, current )
				continue
			}
			TrueGear_ClientMovementUpdate( player, file.previous, current )
			TrueGear_ClientMovementStore( current )
			file.lastSample = Time()
		}
		catch ( error )
		{
			TrueGear_ClientSlideFeedback( false )
			file.player = null
			file.haveBaseline = false
		}
	}
}
