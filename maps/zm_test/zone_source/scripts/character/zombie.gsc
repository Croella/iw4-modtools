// iw4-radiant zombie character. Swap the model for a zone_source zombie (it needs the stock
// human skeleton: j_mainroot, j_spine4, ... so the generic_human anims drive it).
main()
{
	self setModel("body_complete_sp_juggernaut");
	self.voice = "russian";
}

precache()
{
	precacheModel("body_complete_sp_juggernaut");
}
