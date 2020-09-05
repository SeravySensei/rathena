// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

/**
 * Custom Atcommands
 * Place the body of custom atcommands in this file.
 * Format:
 *	ACMD_FUNC(command_name)
 *	{
 *		<code>;
 *	}
 **/

//ACMD_FUNC(newcommand)
//{
//	clif_displaymessage(fd, "It works!");
//	clif_specialeffect(&sd->bl, EF_HEARTCASTING, AREA);
//	return 0;
//}

ACMD_FUNC(autopilot)
{
if (!message || !*message) { clif_displaymessage(fd, "please specify type : Tank,Skill,Support,Tank+, Off"); 
clif_displaymessage(fd, "Tank : Character engages enemy in melee combat and uses melee skills and support skills."); 
clif_displaymessage(fd, "Skill : Character will use long range attacks, long range DPS skills and support skills."); 
clif_displaymessage(fd, "Support : Character uses support skills only and will not attack enemies."); 
clif_displaymessage(fd, "Tank+ : Same as above but character will also do the following :"); 
clif_displaymessage(fd, " Monk : Will use Steel Body regardless of enemy strength."); 
clif_displaymessage(fd, " Merchant classes : Will use skills that cost zeny to eliminate enemies."); 
clif_displaymessage(fd, " Lord Knight : Will use Berserk."); 
}
else {
sd->state.autopilotmode = 0; clif_displaymessage(fd, message);

if (!strcmp(message,"Tank")) {sd->state.autopilotmode = 1; sd->state.specialtanking=false; clif_displaymessage(fd, "Tanking..."); };
if (!strcmp(message,"Tank+")) {sd->state.autopilotmode = 1; sd->state.specialtanking=true; clif_displaymessage(fd, "Tanking..."); };
if (!strcmp(message,"Skill")) {sd->state.autopilotmode = 2; clif_displaymessage(fd, "Skilling..."); };
if (!strcmp(message,"Support")) {sd->state.autopilotmode = 3; clif_displaymessage(fd, "Supporting..."); };
//char buffer [33];
//clif_displaymessage(fd, itoa(sd->status.autopilotmode,buffer,10));
}
return 0; 
}

ACMD_FUNC(autopilothom)
{
if (!sd->hd) {clif_displaymessage(fd, "Homunculus is required"); return 0;}
if (!message || !*message) { clif_displaymessage(fd, "please specify type : Tank,Skill,Support, Off"); 
clif_displaymessage(fd, "Tank : Homunculus engages enemy in melee combat and uses melee skills and support skills."); 
clif_displaymessage(fd, "Skill : Homunculus will use long range attacks, long range DPS skills and support skills."); 
clif_displaymessage(fd, "Support : Homunculus uses support skills only and will not attack enemies."); 
}
else {
sd->hd->autopilotmode = 0; clif_displaymessage(fd, message);

if (!strcmp(message,"Tank")) {sd->hd->autopilotmode = 1; clif_displaymessage(fd, "Homunculus Tanking..."); };
if (!strcmp(message,"Skill")) {sd->hd->autopilotmode = 2; clif_displaymessage(fd, "Homunculus Skilling..."); };
if (!strcmp(message,"Support")) {sd->hd->autopilotmode = 3; clif_displaymessage(fd, "Homunculus Supporting..."); };

}
return 0; 
}




ACMD_FUNC(autopilotsp)
{
if (!message || !*message) { clif_displaymessage(fd, "please specify amount : 0-100"); }
else {
sd->state.autospgoal = atoi(message); clif_displaymessage(fd, "Will use SP items automatically.");
}
return 0; 
}

ACMD_FUNC(autopilotdist)
{
if (!message || !*message) { clif_displaymessage(fd, "please specify amount!"); }
else {
sd->state.autopilotdist = atoi(message); 
}
return 0; 
}

ACMD_FUNC(autosong)
{
if (!message || !*message) { clif_displaymessage(fd, "please specify type a song to play :");
clif_displaymessage(fd, "Whistle Sinx Bragi Apple Lullaby Loki Siegfried Drum Abyss Chaos Kim Ring Moonlit Humming Forget Kiss Service Diss Ugly Hermode");
 }
else {
sd->state.autosong = 0; clif_displaymessage(fd, message);

if (!strcmp(message,"Whistle")) {sd->state.autosong = BA_WHISTLE;};
if (!strcmp(message,"Sinx")) {sd->state.autosong = BA_ASSASSINCROSS;  };
if (!strcmp(message,"Bragi")) {sd->state.autosong = BA_POEMBRAGI; };
if (!strcmp(message,"Apple")) {sd->state.autosong = BA_APPLEIDUN; };
if (!strcmp(message,"Lullaby")) {sd->state.autosong = BD_LULLABY; };
if (!strcmp(message,"Loki")) {sd->state.autosong = BD_ROKISWEIL; };
if (!strcmp(message,"Siegfried")) {sd->state.autosong = BD_SIEGFRIED; };
if (!strcmp(message,"Drum")) {sd->state.autosong = BD_DRUMBATTLEFIELD; };
if (!strcmp(message,"Abyss")) {sd->state.autosong = BD_INTOABYSS; };
if (!strcmp(message,"Chaos")) {sd->state.autosong = BD_ETERNALCHAOS; };
if (!strcmp(message,"Kim")) {sd->state.autosong = BD_RICHMANKIM; };
if (!strcmp(message,"Ring")) {sd->state.autosong =  BD_RINGNIBELUNGEN; };
if (!strcmp(message,"Moonlit")) {sd->state.autosong = CG_MOONLIT; };
if (!strcmp(message,"Humming")) {sd->state.autosong =  DC_HUMMING; };
if (!strcmp(message,"Forget")) {sd->state.autosong = DC_DONTFORGETME; };
if (!strcmp(message,"Kiss")) {sd->state.autosong =  DC_FORTUNEKISS; };
if (!strcmp(message,"Service")) {sd->state.autosong = DC_SERVICEFORYOU; };
if (!strcmp(message,"Diss")) {sd->state.autosong = BA_DISSONANCE; };
if (!strcmp(message,"Ugly")) {sd->state.autosong = DC_UGLYDANCE; };
if (!strcmp(message,"Hermode")) {sd->state.autosong = CG_HERMODE; };
}
return 0; 
}

ACMD_FUNC(autospec)
{
sd->state.enableconc=!sd->state.enableconc;
clif_displaymessage(fd, "Toggled conditional skill usage :"); 
clif_displaymessage(fd, "Concentration, Heat Barrel, Moonlight Fantasy, Auto Berserk, Meltdown "); 

return 0;
}

ACMD_FUNC(autosoulshadow)
{
sd->state.shadowwant=!sd->state.shadowwant;
return 0;
}

ACMD_FUNC(autosong2)
{
if (!message || !*message) { clif_displaymessage(fd, "please specify type a song to play :");
clif_displaymessage(fd, "Swing Lover Serenade Windmill Echo Harmonize");
 }
else {
sd->state.autosong2 = 0; clif_displaymessage(fd, message);

if (!strcmp(message,"Swing")) {sd->state.autosong2 = WA_SWING_DANCE;};
if (!strcmp(message,"Harmonize")) {sd->state.autosong2 = MI_HARMONIZE;  };
if (!strcmp(message,"Echo")) {sd->state.autosong2 = MI_ECHOSONG; };
if (!strcmp(message,"Windmill")) {sd->state.autosong2 = MI_RUSH_WINDMILL; };
if (!strcmp(message,"Serenade")) {sd->state.autosong2 = WA_MOONLIT_SERENADE; };
if (!strcmp(message,"Lover")) {sd->state.autosong2 = WA_SYMPHONY_OF_LOVER; };
}
return 0; 
}

ACMD_FUNC(autosong3)
{
if (!message || !*message) { clif_displaymessage(fd, "please specify type a song to play :");
clif_displaymessage(fd, "Circle Fever Warg Mana Sinking Humming Lerad Warcry Frigg");
 }
else {
sd->state.autosong3 = 0; clif_displaymessage(fd, message);

if (!strcmp(message,"Circle")) {sd->state.autosong3 = WM_SIRCLEOFNATURE;};
if (!strcmp(message,"Fever")) {sd->state.autosong3 = WM_SATURDAY_NIGHT_FEVER;  };
if (!strcmp(message,"Warg")) {sd->state.autosong3 = WM_DANCE_WITH_WUG; };
if (!strcmp(message,"Mana")) {sd->state.autosong3 = WM_SONG_OF_MANA; };
if (!strcmp(message,"Sinking")) {sd->state.autosong3 = WM_MELODYOFSINK; };
if (!strcmp(message,"Humming")) {sd->state.autosong3 = WM_UNLIMITED_HUMMING_VOICE; };
if (!strcmp(message,"Lerad")) {sd->state.autosong3 = WM_LERADS_DEW; };
if (!strcmp(message,"Warcry")) {sd->state.autosong3 = WM_BEYOND_OF_WARCRY; };
if (!strcmp(message,"Frigg")) {sd->state.autosong3 = WM_FRIGG_SONG; };
}
return 0; 
}

ACMD_FUNC(autoelem)
{
if (!message || !*message || !(strlen(message)==3)) { clif_displaymessage(fd, "please specify elemental to use with a 3 character code:");
clif_displaymessage(fd, "F/W/I/E = Fire/Water/Wind/Earth");
clif_displaymessage(fd, "1/2/3 = Elemental level");
clif_displaymessage(fd, "P/O/D = Passive/Offense/Defense");
clif_displaymessage(fd, "Example : @autoelem F3P - level 3 Fire elemental in Passive mode.");

 }
else {
clif_displaymessage(fd, message);
sd->state.autoelemtype=ELE_WIND;
sd->state.autoelemlevel=1;
sd->state.autoelemmode=0;

if (message[0]=='F') {sd->state.autoelemtype = ELE_FIRE;};
if (message[0]=='W') {sd->state.autoelemtype = ELE_WATER;};
if (message[0]=='I') {sd->state.autoelemtype = ELE_WIND;};
if (message[0]=='E') {sd->state.autoelemtype = ELE_EARTH;};
if (message[1]=='1') {sd->state.autoelemlevel = 1;};
if (message[1]=='2') {sd->state.autoelemlevel = 2;};
if (message[1]=='3') {sd->state.autoelemlevel = 3;};
if (message[2]=='P') {sd->state.autoelemmode = EL_MODE_PASSIVE;};
if (message[2]=='O') {sd->state.autoelemmode = EL_MODE_AGGRESSIVE;};
if (message[2]=='D') {sd->state.autoelemmode = EL_MODE_ASSIST;};

if ((sd->state.autoelemtype == ELE_FIRE) && (sd->state.autoelemlevel == 1))
{ sd->state.autoelemreqitem=990; sd->state.autoelemreqitemnum=3;}
if ((sd->state.autoelemtype == ELE_FIRE) && (sd->state.autoelemlevel == 2))
{ sd->state.autoelemreqitem=990; sd->state.autoelemreqitemnum=6;}
if ((sd->state.autoelemtype == ELE_FIRE) && (sd->state.autoelemlevel == 3))
{ sd->state.autoelemreqitem=994; sd->state.autoelemreqitemnum=1;}

if ((sd->state.autoelemtype == ELE_WATER) && (sd->state.autoelemlevel == 1))
{ sd->state.autoelemreqitem=991; sd->state.autoelemreqitemnum=3;}
if ((sd->state.autoelemtype == ELE_WATER) && (sd->state.autoelemlevel == 2))
{ sd->state.autoelemreqitem=991; sd->state.autoelemreqitemnum=6;}
if ((sd->state.autoelemtype == ELE_WATER) && (sd->state.autoelemlevel == 3))
{ sd->state.autoelemreqitem=995; sd->state.autoelemreqitemnum=1;}

if ((sd->state.autoelemtype == ELE_WIND) && (sd->state.autoelemlevel == 1))
{ sd->state.autoelemreqitem=992; sd->state.autoelemreqitemnum=3;}
if ((sd->state.autoelemtype == ELE_WIND) && (sd->state.autoelemlevel == 2))
{ sd->state.autoelemreqitem=992; sd->state.autoelemreqitemnum=6;}
if ((sd->state.autoelemtype == ELE_WIND) && (sd->state.autoelemlevel == 3))
{ sd->state.autoelemreqitem=996; sd->state.autoelemreqitemnum=1;}

if ((sd->state.autoelemtype == ELE_EARTH) && (sd->state.autoelemlevel == 1))
{ sd->state.autoelemreqitem=993; sd->state.autoelemreqitemnum=3;}
if ((sd->state.autoelemtype == ELE_EARTH) && (sd->state.autoelemlevel == 2))
{ sd->state.autoelemreqitem=993; sd->state.autoelemreqitemnum=6;}
if ((sd->state.autoelemtype == ELE_EARTH) && (sd->state.autoelemlevel == 3))
{ sd->state.autoelemreqitem=997; sd->state.autoelemreqitemnum=1;}


}
return 0; 
}
