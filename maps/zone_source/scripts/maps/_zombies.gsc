// iw4-radiant zombies mode: rounds of zombies from actor_zombie spawners that path to a boarded
// window, tear the boards off one by one, climb in and chase the player. The player rebuilds
// boards by holding Use inside a window. Call maps\_zombies::main() after maps\_load::main().
//
// Map setup (iw4-radiant entities):
//   actor_zombie        spawner (spawnflags 1) outside; zombies go to the window nearest to it
//   script_origin       targetname "zm_window": where zombies stand outside a window, facing in
//                       ("angles" yaw points into the building); "target" = a script_origin
//                       inside the window where they land and where the player rebuilds
//   script_model        model "zm_board", targetname "zm_board": the window's boards (each
//                       belongs to the nearest zm_window)
//   node_pathnode       path nodes outside (reaching each zm_window) and inside
//   a monster/player clip brush filling each window opening (zombies climb, nobody walks through)
#include maps\_utility;
#include common_scripts\utility;
#using_animtree("generic_human");

main()
{
	precacheModel("zm_board");
	level.zm_round = 0;
	level.zm_points = 500;
	level.zm_alive = 0;
	level.zm_max_alive = 24;
	windows_init();
	hud_init();
	thread rounds();
}

// ---- windows ----

windows_init()
{
	level.zm_windows = getentarray("zm_window", "targetname");
	boards = getentarray("zm_board", "targetname");
	for (i = 0; i < level.zm_windows.size; i++)
	{
		w = level.zm_windows[i];
		w.boards = [];
		if (isdefined(w.target))
			w.inside = getent(w.target, "targetname").origin;
		else
			w.inside = w.origin + anglestoforward(w.angles) * 64;
		dir = w.origin - w.inside;
		w.outward = vectornormalize((dir[0], dir[1], 0));
		w.yaw = vectortoangles((0, 0, 0) - w.outward)[1];  // facing into the building
	}
	for (i = 0; i < boards.size; i++)
	{
		w = nearest(level.zm_windows, boards[i].origin);
		if (!isdefined(w))
			continue;
		b = spawnstruct();
		b.ent = boards[i];
		b.home = boards[i].origin;
		b.homeangles = boards[i].angles;
		w.boards[w.boards.size] = b;
	}
	for (i = 0; i < level.zm_windows.size; i++)
	{
		level.zm_windows[i].up = level.zm_windows[i].boards.size;
		level.zm_windows[i] thread window_repair();
	}
}

nearest(ents, origin)
{
	best = undefined;
	bestd = 0;
	for (i = 0; i < ents.size; i++)
	{
		d = distancesquared(ents[i].origin, origin);
		if (!isdefined(best) || d < bestd)
		{
			best = ents[i];
			bestd = d;
		}
	}
	return best;
}

board_remove()
{
	if (self.up <= 0)
		return;
	self.up--;
	b = self.boards[self.up];
	b.ent moveto(b.home + self.outward * 36 + (0, 0, -28), 0.35, 0.1, 0);
	b.ent rotateroll(randomfloatrange(-70, 70), 0.35);
	wait 0.35;
	b.ent hide();
}

board_add()
{
	if (self.up >= self.boards.size)
		return;
	b = self.boards[self.up];
	self.up++;
	b.ent.origin = b.home + self.outward * -24;
	b.ent.angles = b.homeangles;
	b.ent show();
	b.ent moveto(b.home, 0.25, 0, 0.1);
}

window_repair()
{
	for (;;)
	{
		wait 0.05;
		if (self.up >= self.boards.size)
			continue;
		if (distance(level.player.origin, self.inside) > 80)
			continue;
		if (!level.player usebuttonpressed())
			continue;
		self board_add();
		add_points(10);
		wait 1.0;
	}
}

// ---- rounds ----

rounds()
{
	wait 2;
	spawners = [];
	all = getspawnerarray();
	for (i = 0; i < all.size; i++)
		if (all[i].classname == "actor_zombie")
			spawners[spawners.size] = all[i];
	if (!spawners.size)
	{
		iprintlnbold("zombies: no actor_zombie spawners (spawnflags 1) in the map");
		return;
	}
	iprintlnbold("Hold USE inside a window to rebuild its boards");
	for (;;)
	{
		level.zm_round++;
		level.zm_hud_round setvalue(level.zm_round);
		total = 6 + level.zm_round * 3;
		level.zm_health = 150 + 100 * (level.zm_round - 1);
		if (level.zm_health > 1500)
			level.zm_health = 1500;
		delay = 2.0 - level.zm_round * 0.15;
		if (delay < 0.5)
			delay = 0.5;
		spawned = 0;
		failures = 0;
		while (spawned < total)
		{
			while (level.zm_alive >= level.zm_max_alive)
				wait 0.5;
			sp = spawners[randomint(spawners.size)];
			sp.count = 1;
			guy = sp stalingradspawn();
			if (spawn_failed(guy))
			{
				failures++;
				if (failures > 50)
				{
					iprintlnbold("zombies: spawning keeps failing");
					return;
				}
				wait 0.5;
				continue;
			}
			spawned++;
			guy thread zombie_think(sp);
			wait delay;
		}
		while (level.zm_alive > 0)
			wait 0.5;
		wait 6;
	}
}

// ---- zombies ----

zombie_think(spawner)
{
	self endon("death");
	level.zm_alive++;
	self thread zombie_death();
	self thread zombie_damage_points();
	self.ignoreall = true;  // never picks an enemy, so never shoots; it just follows its goal
	self.goalradius = 24;
	self.disableArrivals = true;
	self.disableExits = true;
	self.grenadeammo = 0;
	self.health = level.zm_health;
	self.maxhealth = level.zm_health;
	self disable_pain();
	self gun_remove();
	if (level.zm_round >= 4)
		self.run_overrideanim = %sprint1_loop;
	w = nearest(level.zm_windows, spawner.origin);
	if (isdefined(w))
		self zombie_window(w);
	self zombie_chase();
}

zombie_window(w)
{
	self endon("death");
	self setgoalpos(w.origin);
	// wait to arrive (re-issuing the goal now and then in case the path was blocked)
	for (t = 0; distance2d(self.origin, w.origin) > 40; t++)
	{
		wait 0.25;
		if (t % 20 == 19)
			self setgoalpos(w.origin);
	}
	self orientmode("face angle", w.yaw);
	while (w.up > 0)
	{
		self animscripted("zm_tear", self.origin, (0, w.yaw, 0), %melee_1);
		wait 0.5;
		w board_remove();
		wait getanimlength(%melee_1) - 0.5;
	}
	self animscripted("zm_climb", w.origin, (0, w.yaw, 0), %traverse_window_quick);
	wait getanimlength(%traverse_window_quick);
	self forceteleport(w.inside, (0, w.yaw, 0));
	self orientmode("face default");
}

zombie_chase()
{
	self endon("death");
	self setgoalentity(level.player);
	for (;;)
	{
		wait 0.1;
		if (!isalive(level.player))
			return;
		d = level.player.origin - self.origin;
		if (abs(d[2]) > 48 || distance2d(level.player.origin, self.origin) > 52)
			continue;
		self animscripted("zm_attack", self.origin, (0, vectortoangles(d)[1], 0), %melee_1);
		wait 0.4;
		if (distance(level.player.origin, self.origin) < 76)
			level.player dodamage(45, self.origin, self);
		wait getanimlength(%melee_1) - 0.4;
		self setgoalentity(level.player);
	}
}

zombie_death()
{
	self waittill("death", attacker);
	level.zm_alive--;
	if (isdefined(attacker) && attacker == level.player)
		add_points(60);
}

zombie_damage_points()
{
	self endon("death");
	for (;;)
	{
		self waittill("damage", amount, attacker);
		if (isdefined(attacker) && attacker == level.player)
			add_points(10);
	}
}

// ---- hud ----

hud_init()
{
	level.zm_hud_points = newhudelem();
	level.zm_hud_points.alignx = "right";
	level.zm_hud_points.aligny = "bottom";
	level.zm_hud_points.horzalign = "right";
	level.zm_hud_points.vertalign = "bottom";
	level.zm_hud_points.x = -24;
	level.zm_hud_points.y = -96;
	level.zm_hud_points.fontscale = 2;
	level.zm_hud_points.color = (1, 0.85, 0.3);
	level.zm_hud_points setvalue(level.zm_points);

	level.zm_hud_round = newhudelem();
	level.zm_hud_round.alignx = "left";
	level.zm_hud_round.aligny = "bottom";
	level.zm_hud_round.horzalign = "left";
	level.zm_hud_round.vertalign = "bottom";
	level.zm_hud_round.x = 24;
	level.zm_hud_round.y = -24;
	level.zm_hud_round.fontscale = 3;
	level.zm_hud_round.color = (0.7, 0.05, 0.05);
	level.zm_hud_round setvalue(0);
}

add_points(n)
{
	level.zm_points += n;
	level.zm_hud_points setvalue(level.zm_points);
}
