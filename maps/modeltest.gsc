// iw4-radiant zone_source demo: crates (rigid models) and swaying tails (skinned model + animation)
#using_animtree("custom");

main()
{
	precachemodel("viewhands_op_force");
	precacheitem("ak74uwu");
	maps\_load::main();

	level.player thread onPlayerSpawned();

	tails = getentarray("demo_tail", "targetname");
	for (i = 0; i < tails.size; i++)
		tails[i] thread sway(i * 0.3);
}

sway(delay)
{
	wait delay;
	self useAnimTree(#animtree);
	self setAnim(%demo_tail_loop, 1, 0, 1);
}

onPlayerSpawned()
{
	wait 1;  // after the stock loadout has set its own hands
	self setViewmodel("viewhands_op_force");
	self giveWeapon("ak74uwu");
	self switchToWeapon("ak74uwu");
}
