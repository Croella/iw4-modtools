// zm_test: maps\_zombies sample (zone_source/scripts/maps/_zombies.gsc)
main()
{
	precachemodel("viewmodel_usa_cuban_casual_arms");
	precachemodel("viewmodel_usa_cuban_casual_fullbody");
	precacheitem("ak74uwu");
	maps\_iw4r_footfx::main();  // dust puffs at every running step (zombies + player)
	maps\_load::main();
	maps\_zombies::main();
	level.player thread loadout();
}

loadout()
{
	wait 1;  // after the stock loadout
	self takeallweapons();
	self setviewmodel("viewmodel_usa_cuban_casual_arms");
	self setmodel("viewmodel_usa_cuban_casual_fullbody");
	self giveweapon("ak74uwu");
	self givemaxammo("ak74uwu");
	self switchtoweapon("ak74uwu");
}
