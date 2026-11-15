// iw4-radiant footstep puffs: a small cloud kicked up at every step when running, picked by the
// surface underfoot (brush Surface type in the editor, else the material's own).
//
//   main() { maps\_iw4r_footfx::main(); maps\_load::main(); ... }
//
// AI (zombies, soldiers): the stock animscripts already play level._effect["step_<surface>"] on
// their footstep notetracks for every surface registered with animscripts\utility::setFootstepEffect,
// so registering is all they need. The player has no footstep notetracks: a thread watches their
// speed and drops a puff every stride while moving fast on the ground.
//
// Effects: BO1's bio/player/fx_footstep_* (zone_source/fx/bio/player), compiled by iw4-radiant.

main()
{
	level._iw4r_footfx = [];
	dust = loadfx("bio/player/fx_footstep_dust");
	sand = loadfx("bio/player/fx_footstep_sand");
	snow = loadfx("bio/player/fx_footstep_snow");
	mud = loadfx("bio/player/fx_footstep_mud");
	water = loadfx("bio/player/fx_footstep_water");
	footfx_surface("dirt", dust);
	footfx_surface("gravel", dust);
	footfx_surface("asphalt", dust);
	footfx_surface("concrete", dust);
	footfx_surface("rock", dust);
	footfx_surface("brick", dust);
	footfx_surface("plaster", dust);
	footfx_surface("default", dust);
	footfx_surface("sand", sand);
	footfx_surface("snow", snow);
	footfx_surface("slush", snow);
	footfx_surface("ice", snow);
	footfx_surface("mud", mud);
	footfx_surface("water", water);
	level thread footfx_player_wait();
}

// One surface -> effect, for AI (stock hook) and the player thread.
footfx_surface(surface, fx)
{
	level._iw4r_footfx[surface] = fx;
	animscripts\utility::setFootstepEffect(surface, fx);
	animscripts\utility::setFootstepEffectSmall(surface, fx);
}

footfx_player_wait()
{
	while (!isdefined(level.player))
		wait 0.05;
	level.player thread footfx_player();
}

// Player: a puff every `stride` units travelled on the ground faster than `minSpeed` (units/s;
// MW2 runs at ~190, sprints at ~290), alternating left/right of the centre line.
footfx_player(minSpeed, stride)
{
	self endon("death");
	if (!isdefined(minSpeed))
		minSpeed = 170;
	if (!isdefined(stride))
		stride = 70;
	travelled = 0;
	side = 1;
	last = self.origin;
	for (;;)
	{
		wait 0.05;
		moved = distance2d(self.origin, last);
		last = self.origin;
		speed = distance2d(self getvelocity(), (0, 0, 0));
		if (!self isonground() || speed < minSpeed || self getstance() != "stand")
		{
			travelled = 0;
			continue;
		}
		travelled += moved;
		if (travelled < stride)
			continue;
		travelled = 0;
		side = 0 - side;
		surface = footfx_surface_under(self);
		if (!isdefined(level._iw4r_footfx[surface]))
			continue;
		right = anglestoright(self.angles);
		org = self.origin + right * (6 * side) + (0, 0, 2);
		playfx(level._iw4r_footfx[surface], org, (0, 0, 1), anglestoforward(self.angles) * -1);
	}
}

footfx_surface_under(ent)
{
	trace = bullettrace(ent.origin + (0, 0, 8), ent.origin - (0, 0, 32), false, ent);
	if (!isdefined(trace["surfacetype"]) || trace["surfacetype"] == "none")
		return "default";
	return trace["surfacetype"];
}
