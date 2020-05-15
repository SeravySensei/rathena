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
clif_displaymessage(fd, "Monk : Will use Steel Body regardless of enemy strength."); 
clif_displaymessage(fd, "Merchant classes : Will use skills that cost zeny to eliminate enemies."); 
clif_displaymessage(fd, "Lord Knight : Will use Berserk, or if not available then Concentration."); 
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

ACMD_FUNC(autoconc)
{
sd->state.enableconc=!sd->state.enableconc;
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
clif_displaymessage(fd, "Not Implemented");
 }
else {
sd->state.autosong3 = 0; clif_displaymessage(fd, message);

if (!strcmp(message,"Swing")) {sd->state.autosong3 = WA_SWING_DANCE;};
if (!strcmp(message,"Harmonize")) {sd->state.autosong3 = MI_HARMONIZE;  };
if (!strcmp(message,"Echo")) {sd->state.autosong3 = MI_ECHOSONG; };
if (!strcmp(message,"Windmill")) {sd->state.autosong3 = MI_RUSH_WINDMILL; };
if (!strcmp(message,"Serenade")) {sd->state.autosong3 = WA_MOONLIT_SERENADE; };
if (!strcmp(message,"Lover")) {sd->state.autosong3 = WA_SYMPHONY_OF_LOVER; };
}
return 0; 
}
