# 事件目录

共 151 个事件名。此列表由源码生成；表示已实现的检测入口，尚未全部经游戏实测。

`callback` 为游戏／生命周期回调，`poll` 为状态观察，`vr_state` 为 CircuitLord 状态值。

事件是否构成推断还需看 `data.inferred`、`data.estimated` 和 README 的语义说明。

| 事件名 | 来源 | 实现文件 |
|---|---|---|
| `ability_ammo_consumed` | poll | tg_events_vr.nut |
| `ability_ammo_restored` | poll | tg_events_vr.nut |
| `ability_charge_fraction_changed` | poll | tg_events_vr.nut |
| `ability_charge_fraction_max` | poll | tg_events_vr.nut |
| `ability_charge_fraction_min` | poll | tg_events_vr.nut |
| `ability_cooldown_end` | poll | tg_events_vr.nut |
| `ability_cooldown_start` | poll | tg_events_vr.nut |
| `ability_equipped` | poll | tg_events_vr.nut |
| `ability_removed` | poll | tg_events_vr.nut |
| `combat_damage_bullet` | callback | tg_events_combat.nut |
| `combat_damage_critical` | callback | tg_events_combat.nut |
| `combat_damage_crush` | callback | tg_events_combat.nut |
| `combat_damage_dealt` | callback | tg_events_combat.nut |
| `combat_damage_electric` | callback | tg_events_combat.nut |
| `combat_damage_environment` | callback | tg_events_combat.nut |
| `combat_damage_explosion` | callback | tg_events_combat.nut |
| `combat_damage_fall` | callback | tg_events_combat.nut |
| `combat_damage_fire` | callback | tg_events_combat.nut |
| `combat_damage_headshot` | callback | tg_events_combat.nut |
| `combat_damage_melee` | callback | tg_events_combat.nut |
| `combat_damage_shield` | callback | tg_events_combat.nut |
| `combat_damage_taken` | callback | tg_events_combat.nut |
| `combat_damage_taken_nondirectional` | callback | tg_events_combat.nut |
| `combat_enemy_killed` | callback | tg_events_titan.nut |
| `combat_hit_critical` | callback | tg_events_combat.nut |
| `combat_hit_headshot` | callback | tg_events_combat.nut |
| `combat_hit_melee` | callback | tg_events_combat.nut |
| `combat_kill_confirmed` | callback | tg_events_combat.nut |
| `combat_melee_end` | poll | tg_events_combat.nut |
| `combat_melee_start` | poll | tg_events_combat.nut |
| `health_changed` | poll | tg_events_combat.nut |
| `health_full` | poll | tg_events_combat.nut |
| `health_low` | poll | tg_events_combat.nut |
| `health_recovered` | poll | tg_events_combat.nut |
| `health_regen_start` | poll | tg_events_combat.nut |
| `health_snapshot` | poll | tg_events_combat.nut |
| `movement_crouch_start` | poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `movement_crouch_stop` | poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `movement_dash` | callback, poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `movement_double_jump` | callback, poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `movement_fall_start` | poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `movement_fall_stop` | poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `movement_jump` | callback, poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `movement_land` | callback, poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `movement_leave_ground` | callback, poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `movement_move_start` | poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `movement_move_stop` | poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `movement_pulse` | poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `movement_slide_start` | poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `movement_slide_stop` | poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `movement_sprint_start` | poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `movement_sprint_stop` | poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `player_context_action_start` | poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `player_context_action_stop` | poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `player_death` | callback, poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `player_despawn` | callback | tg_events_movement.nut |
| `player_execution_start` | poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `player_execution_stop` | poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `player_phase_shift_start` | poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `player_phase_shift_stop` | poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `player_spawn` | callback, poll | tg_events_client_movement.nut, tg_events_movement.nut |
| `session_ready` | callback | tg_events_core.gnut |
| `shield_broken` | poll | tg_events_combat.nut |
| `shield_changed` | poll | tg_events_combat.nut |
| `shield_full` | poll | tg_events_combat.nut |
| `shield_regen_end` | poll | tg_events_combat.nut |
| `shield_regen_start` | poll | tg_events_combat.nut |
| `shield_restored` | poll | tg_events_combat.nut |
| `shield_snapshot` | poll | tg_events_combat.nut |
| `titan_auto_assigned` | poll | tg_events_titan.nut |
| `titan_auto_unassigned` | poll | tg_events_titan.nut |
| `titan_core_active_end` | poll | tg_events_vr.nut |
| `titan_core_active_start` | poll | tg_events_vr.nut |
| `titan_core_charge_changed` | poll | tg_events_vr.nut |
| `titan_core_ready` | poll | tg_events_vr.nut |
| `titan_destroyed` | callback | tg_events_titan.nut |
| `titan_disembark_complete` | callback | tg_events_titan.nut |
| `titan_doomed` | callback | tg_events_titan.nut |
| `titan_embark_complete` | callback | tg_events_titan.nut |
| `titan_footstep_inferred` | poll | tg_events_vr.nut |
| `titan_gun_shield_impact` | callback | tg_events_titan_shields.nut |
| `titan_heat_shield_impact` | callback | tg_events_titan_shields.nut |
| `titan_particle_wall_impact` | callback | tg_events_titan_shields.nut |
| `titan_shield_damage` | callback | tg_events_titan.nut |
| `titan_sword_block_impact` | callback | tg_events_titan_shields.nut |
| `titan_vortex_shield_impact` | callback | tg_events_titan_shields.nut |
| `titan_vortex_shield_reflect` | callback | tg_events_titan_shields.nut |
| `vr_ammo_pouch_taken_left_shoulder` | vr_state | tg_events_hands.gnut |
| `vr_ammo_pouch_taken_right_shoulder` | vr_state | tg_events_hands.gnut |
| `vr_ammo_pouch_taken_shoulders` | vr_state | tg_events_hands.gnut |
| `vr_chest_hidden` | vr_state | tg_events_vr.nut |
| `vr_chest_visible` | vr_state | tg_events_vr.nut |
| `vr_controller_haptic_left` | vr_state | tg_events_hands.gnut |
| `vr_controller_haptic_right` | vr_state | tg_events_hands.gnut |
| `vr_grenade_cancel_requested` | callback | tg_events_hands.gnut |
| `vr_grenade_grab_accepted` | vr_state | tg_events_vr.nut |
| `vr_grenade_grab_rejected` | vr_state | tg_events_vr.nut |
| `vr_grenade_store_requested` | callback | tg_events_hands.gnut |
| `vr_grenade_thrown_predicted` | vr_state | tg_events_hands.gnut |
| `vr_holster_available` | vr_state | tg_events_vr.nut |
| `vr_holster_unavailable` | vr_state | tg_events_vr.nut |
| `vr_item_grab_rejected` | callback | tg_events_hands.gnut |
| `vr_item_grabbed` | callback | tg_events_hands.gnut |
| `vr_pickup_target_entered` | vr_state | tg_events_vr.nut |
| `vr_pickup_target_left` | vr_state | tg_events_vr.nut |
| `vr_recenter` | vr_state | tg_events_vr.nut |
| `vr_reload_empty_pouch` | vr_state | tg_events_vr.nut |
| `vr_reload_guide_started` | vr_state | tg_events_vr.nut |
| `vr_reload_guide_stopped` | vr_state | tg_events_vr.nut |
| `vr_reload_magazine_ejected` | vr_state | tg_events_vr.nut |
| `vr_reload_magazine_out` | vr_state | tg_events_vr.nut |
| `vr_reload_magazine_out_cleared` | vr_state | tg_events_vr.nut |
| `vr_reload_round_ejected` | vr_state | tg_events_vr.nut |
| `vr_slot_item_stored_grenade` | vr_state | tg_events_hands.gnut |
| `vr_slot_item_stored_left_hip` | vr_state | tg_events_hands.gnut |
| `vr_slot_item_stored_left_shoulder` | vr_state | tg_events_hands.gnut |
| `vr_slot_item_stored_right_hip` | vr_state | tg_events_hands.gnut |
| `vr_slot_item_stored_right_shoulder` | vr_state | tg_events_hands.gnut |
| `vr_slot_item_stored_shoulders` | vr_state | tg_events_hands.gnut |
| `vr_slot_item_taken_grenade` | vr_state | tg_events_hands.gnut |
| `vr_slot_item_taken_left_hip` | vr_state | tg_events_hands.gnut |
| `vr_slot_item_taken_left_shoulder` | vr_state | tg_events_hands.gnut |
| `vr_slot_item_taken_right_hip` | vr_state | tg_events_hands.gnut |
| `vr_slot_item_taken_right_shoulder` | vr_state | tg_events_hands.gnut |
| `vr_slot_item_taken_shoulders` | vr_state | tg_events_hands.gnut |
| `vr_weapon_equip_accepted` | vr_state | tg_events_vr.nut |
| `vr_weapon_equip_rejected` | vr_state | tg_events_vr.nut |
| `weapon_aim_end` | poll | tg_events_combat.nut |
| `weapon_aim_start` | poll | tg_events_combat.nut |
| `weapon_ammo_changed` | poll | tg_events_combat.nut |
| `weapon_ammo_empty` | poll | tg_events_combat.nut |
| `weapon_ammo_refilled` | poll | tg_events_combat.nut |
| `weapon_charge_fraction_max` | poll | tg_events_combat.nut |
| `weapon_charge_fraction_min` | poll | tg_events_combat.nut |
| `weapon_charge_level` | poll | tg_events_combat.nut |
| `weapon_charge_rise_end` | poll | tg_events_combat.nut |
| `weapon_charge_rise_start` | poll | tg_events_combat.nut |
| `weapon_equipped` | poll | tg_events_combat.nut |
| `weapon_holstered` | poll | tg_events_combat.nut |
| `weapon_reload_complete_inferred` | poll | tg_events_combat.nut |
| `weapon_reload_end` | poll | tg_events_combat.nut |
| `weapon_reload_start` | poll | tg_events_combat.nut |
| `weapon_shot_anti_titan` | poll | tg_events_combat.nut |
| `weapon_shot_inferred` | poll | tg_events_combat.nut |
| `weapon_shot_lmg` | poll | tg_events_combat.nut |
| `weapon_shot_pistol` | poll | tg_events_combat.nut |
| `weapon_shot_rifle` | poll | tg_events_combat.nut |
| `weapon_shot_shotgun` | poll | tg_events_combat.nut |
| `weapon_shot_smg` | poll | tg_events_combat.nut |
| `weapon_shot_sniper` | poll | tg_events_combat.nut |
| `weapon_shot_special` | poll | tg_events_combat.nut |
