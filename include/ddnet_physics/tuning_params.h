/* The tuning parameters, in network order. This file can be included several times.
 *
 * DDNET_TUNING_PARAM(name, default value) */
#ifndef DDNET_TUNING_PARAM
#error "define DDNET_TUNING_PARAM before including this file"
#endif

/* physics tuning */
DDNET_TUNING_PARAM(ground_control_speed, 10.0f)
DDNET_TUNING_PARAM(ground_control_accel, 100.0f / (float)DDNET_TICK_SPEED)
DDNET_TUNING_PARAM(ground_friction, 0.5f)
DDNET_TUNING_PARAM(ground_jump_impulse, 13.2f)
DDNET_TUNING_PARAM(air_jump_impulse, 12.0f)
DDNET_TUNING_PARAM(air_control_speed, 250.0f / (float)DDNET_TICK_SPEED)
DDNET_TUNING_PARAM(air_control_accel, 1.5f)
DDNET_TUNING_PARAM(air_friction, 0.95f)
DDNET_TUNING_PARAM(hook_length, 380.0f)
DDNET_TUNING_PARAM(hook_fire_speed, 80.0f)
DDNET_TUNING_PARAM(hook_drag_accel, 3.0f)
DDNET_TUNING_PARAM(hook_drag_speed, 15.0f)
DDNET_TUNING_PARAM(gravity, 0.5f)
DDNET_TUNING_PARAM(velramp_start, 550)
DDNET_TUNING_PARAM(velramp_range, 2000)
DDNET_TUNING_PARAM(velramp_curvature, 1.4f)

/* weapon tuning */
DDNET_TUNING_PARAM(gun_curvature, 1.25f)
DDNET_TUNING_PARAM(gun_speed, 2200.0f)
DDNET_TUNING_PARAM(gun_lifetime, 2.0f)
DDNET_TUNING_PARAM(shotgun_curvature, 1.25f)
DDNET_TUNING_PARAM(shotgun_speed, 2750.0f)
DDNET_TUNING_PARAM(shotgun_speeddiff, 0.8f)
DDNET_TUNING_PARAM(shotgun_lifetime, 0.20f)
DDNET_TUNING_PARAM(grenade_curvature, 7.0f)
DDNET_TUNING_PARAM(grenade_speed, 1000.0f)
DDNET_TUNING_PARAM(grenade_lifetime, 2.0f)
DDNET_TUNING_PARAM(laser_reach, 800.0f)
DDNET_TUNING_PARAM(laser_bounce_delay, 150)
DDNET_TUNING_PARAM(laser_bounce_num, 1000)
DDNET_TUNING_PARAM(laser_bounce_cost, 0)
DDNET_TUNING_PARAM(laser_damage, 5)
DDNET_TUNING_PARAM(player_collision, 1)
DDNET_TUNING_PARAM(player_hooking, 1)

/* ddnet tuning */
DDNET_TUNING_PARAM(jetpack_strength, 400.0f)
DDNET_TUNING_PARAM(shotgun_strength, 10.0f)
DDNET_TUNING_PARAM(explosion_strength, 6.0f)
DDNET_TUNING_PARAM(hammer_strength, 1.0f)
DDNET_TUNING_PARAM(hook_duration, 1.25f)
DDNET_TUNING_PARAM(hammer_fire_delay, 125)
DDNET_TUNING_PARAM(gun_fire_delay, 125)
DDNET_TUNING_PARAM(shotgun_fire_delay, 500)
DDNET_TUNING_PARAM(grenade_fire_delay, 500)
DDNET_TUNING_PARAM(laser_fire_delay, 800)
DDNET_TUNING_PARAM(ninja_fire_delay, 800)
DDNET_TUNING_PARAM(hammer_hit_fire_delay, 320)
DDNET_TUNING_PARAM(ground_elasticity_x, 0)
DDNET_TUNING_PARAM(ground_elasticity_y, 0)
