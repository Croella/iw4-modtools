// iw4-radiant sound helpers: play a sound alias on an entity at a time, or when an animation hits
// a notetrack. Works on any entity (AI, script_model, player); `tag` (optional) attaches the sound
// to a bone/tag so it follows it, otherwise it plays from the entity.
//
//   #include maps\_iw4r_sound;
//
//   // 0.4 seconds from now
//   guy thread play_sound_at_time("zmb_attack", 0.4);
//
//   // when an animation started with a notify name reaches a notetrack (animscripted /
//   // setflaggedanim send each notetrack as that notify)
//   guy animscripted("zm_tear", guy.origin, guy.angles, %melee_1);
//   guy thread play_sound_on_notetrack("zm_tear", "fire", "zmb_board_break", "tag_weapon_right");
//
//   // at a notetrack's time read from the animation itself (no notify needed; call it when the
//   // animation starts; rate = playback rate if not 1)
//   model setanim(%my_anim);
//   model thread play_sound_at_notetrack_time(%my_anim, "door_hit", "door_slam");
//
//   // every notetrack named "snd_<alias>" plays <alias> (prefix optional, default "snd_")
//   guy thread notetrack_sounds("zm_tear");
//
//   // footsteps on "footstep..." notetracks, by the surface underfoot (see footsteps() below)
//   model thread footsteps("walking", "walk");
//
//   // cancel everything above that is still waiting on this entity
//   guy stop_notetrack_sounds();
//
// Notetrack names are lowercase (iw4-radiant lowercases them when it compiles xanims). Every helper
// also ends when its animation sends "end", or when the entity dies.

play_sound_at_time(alias, time, tag)
{
	self endon("death");
	self endon("iw4r_stop_sounds");
	if (isdefined(time) && time > 0)
		wait time;
	self iw4r_play_sound(alias, tag);
}

play_sound_on_notetrack(notifyName, notetrack, alias, tag)
{
	self endon("death");
	self endon("iw4r_stop_sounds");
	for (;;)
	{
		self waittill(notifyName, note);
		if (!isdefined(note))
			continue;
		if (note == notetrack)
		{
			self iw4r_play_sound(alias, tag);
			return;
		}
		if (note == "end")
			return;
	}
}

play_sound_at_notetrack_time(animation, notetrack, alias, tag, rate)
{
	self endon("death");
	self endon("iw4r_stop_sounds");
	times = getnotetracktimes(animation, notetrack);
	if (!isdefined(times) || !times.size)
		return;
	length = getanimlength(animation);
	if (isdefined(rate) && rate > 0)
		length = length / rate;
	start = gettime();
	for (i = 0; i < times.size; i++)
	{
		due = times[i] * length - (gettime() - start) / 1000;
		if (due > 0)
			wait due;
		self iw4r_play_sound(alias, tag);
	}
}

notetrack_sounds(notifyName, prefix)
{
	self endon("death");
	self endon("iw4r_stop_sounds");
	if (!isdefined(prefix))
		prefix = "snd_";
	for (;;)
	{
		self waittill(notifyName, note);
		if (!isdefined(note))
			continue;
		if (note == "end")
			return;
		if (note.size > prefix.size && getsubstr(note, 0, prefix.size) == prefix)
			self thread iw4r_play_sound(getsubstr(note, prefix.size, note.size));
	}
}

stop_notetrack_sounds()
{
	self notify("iw4r_stop_sounds");
}

// ---- footsteps ----
// Footsteps the way stock AI make them (animscripts/shared.gsc): a notetrack with "footstep" in its
// name (stock anims: footstep_left_large, footstep_right_small, ...; name yours footstep_left /
// footstep_right) plays step_<gait>_<surface>, the surface being what the entity stands on.
// common.ff has those aliases for every surface (step_run_concrete, step_walk_wood, ...).
// gait: "run" (default), "walk", "sprint" or "prone".
//
//   model setflaggedanim("walking", %my_walk);
//   model thread footsteps("walking", "walk");
//
// AI already get footsteps from the stock animscripts; this is for script_models and other
// entities playing your own animations.
footsteps(notifyName, gait)
{
	self endon("death");
	self endon("iw4r_stop_sounds");
	for (;;)
	{
		self waittill(notifyName, note);
		if (!isdefined(note))
			continue;
		if (note == "end")
			return;
		if (issubstr(note, "footstep"))
			self play_footstep(gait);
	}
}

// One footstep now, for the surface under the entity.
play_footstep(gait)
{
	if (!isdefined(gait))
		gait = "run";
	self playsound("step_" + gait + "_" + self footstep_surface());
}

// The surface under the entity: an AI's groundtype, else a trace down from its origin.
footstep_surface()
{
	if (isai(self) && isdefined(self.groundtype))
		return self.groundtype;
	trace = bullettrace(self.origin + (0, 0, 8), self.origin - (0, 0, 32), false, self);
	if (!isdefined(trace["surfacetype"]) || trace["surfacetype"] == "none")
		return "default";
	return trace["surfacetype"];
}

// The sound itself: from the entity, or from a script_origin linked to `tag` that is deleted when
// the sound finishes.
iw4r_play_sound(alias, tag)
{
	if (!isdefined(tag))
	{
		self playsound(alias);
		return;
	}
	org = spawn("script_origin", self gettagorigin(tag));
	org linkto(self, tag, (0, 0, 0), (0, 0, 0));
	org playsound(alias, "sounddone");
	org waittill("sounddone");
	org delete();
}
