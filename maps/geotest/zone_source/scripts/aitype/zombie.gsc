// iw4-radiant zombie AI type: loaded by "actor_zombie" spawners (the game calls main() on every
// AI it spawns, precache() at level load, spawner() on each spawner).
// Body: character\zombie (the Special Ops juggernaut until there is a zombie model), so the map
// needs worldspawn "weaponpack" "so_juggernauts_favela" for that body and the m240 it carries.
// maps\_zombies hides the gun and makes it a melee rusher.
main()
{
	self.animTree = "";
	self.team = "axis";
	self.type = "human";
	self.accuracy = 0.2;
	self.health = 150;
	self.weapon = "m240";
	self.secondaryweapon = "";
	self.sidearm = "";
	self.grenadeWeapon = "fraggrenade";
	self.grenadeAmmo = 0;

	if ( isAI( self ) )
	{
		self setEngagementMinDist( 0.000000, 0.000000 );
		self setEngagementMaxDist( 64.000000, 128.000000 );
	}

	character\zombie::main();
}

spawner()
{
	self setspawnerteam("axis");
}

precache()
{
	character\zombie::precache();

	precacheItem("m240");
	precacheItem("fraggrenade");
}
