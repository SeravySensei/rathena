// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "unit.hpp"

#include <stdlib.h>
#include <string.h>

#include "../common/db.hpp"
#include "../common/ers.hpp"  // ers_destroy
#include "../common/malloc.hpp"
#include "../common/nullpo.hpp"
#include "../common/random.hpp"
#include "../common/showmsg.hpp"
#include "../common/socket.hpp"
#include "../common/timer.hpp"

#include "achievement.hpp"
#include "battle.hpp"
#include "battleground.hpp"
#include "channel.hpp"
#include "chat.hpp"
#include "clif.hpp"
#include "duel.hpp"
#include "elemental.hpp"
#include "guild.hpp"
#include "homunculus.hpp"
#include "intif.hpp"
#include "map.hpp"
#include "mercenary.hpp"
#include "mob.hpp"
#include "npc.hpp"
#include "party.hpp"
#include "path.hpp"
#include "pc.hpp"
#include "pet.hpp"
#include "storage.hpp"
#include "trade.hpp"

// Directions values
// 1 0 7
// 2 . 6
// 3 4 5
// See also path.cpp walk_choices
const short dirx[DIR_MAX]={0,-1,-1,-1,0,1,1,1}; ///lookup to know where will move to x according dir
const short diry[DIR_MAX]={1,1,0,-1,-1,-1,0,1}; ///lookup to know where will move to y according dir

#define AUTOPILOT_RANGE_CAP 14 // Max distance the @autopilot is allowed to attack at using single target skills.

#define CLIENT_COMPLEX_PATH_LIMIT 14

//early declaration
static TIMER_FUNC(unit_attack_timer);
static TIMER_FUNC(unit_walktoxy_timer);
int unit_unattackable(struct block_list *bl);

/**
 * Get the unit_data related to the bl
 * @param bl : Object to get the unit_data from
 *	valid type are : BL_PC|BL_MOB|BL_PET|BL_NPC|BL_HOM|BL_MER|BL_ELEM
 * @return unit_data of bl or NULL
 */
struct unit_data* unit_bl2ud(struct block_list *bl)
{
	if( bl == NULL) return NULL;
	switch(bl->type){
	case BL_PC: return &((struct map_session_data*)bl)->ud;
	case BL_MOB: return &((struct mob_data*)bl)->ud;
	case BL_PET: return &((struct pet_data*)bl)->ud;
	case BL_NPC: return &((struct npc_data*)bl)->ud;
	case BL_HOM: return &((struct homun_data*)bl)->ud;
	case BL_MER: return &((struct mercenary_data*)bl)->ud;
	case BL_ELEM: return &((struct elemental_data*)bl)->ud;
	default : return NULL;
	}
}

/**
 * Tells a unit to walk to a specific coordinate
 * @param bl: Unit to walk [ALL]
 * @return 1: Success 0: Fail
 */
int unit_walktoxy_sub(struct block_list *bl)
{
	int i;
	struct walkpath_data wpd;
	struct unit_data *ud = NULL;

	nullpo_retr(1, bl);
	ud = unit_bl2ud(bl);
	if(ud == NULL) return 0;

	if( !path_search(&wpd,bl->m,bl->x,bl->y,ud->to_x,ud->to_y,ud->state.walk_easy,CELL_CHKNOPASS) )
		return 0;

	if (!(path_search_long(NULL, bl->m, bl->x, bl->y, ud->to_x, ud->to_y, CELL_CHKNOPASS, MAX_WALKPATH)) // Check if there is an obstacle between
		&& (wpd.path_len > CLIENT_COMPLEX_PATH_LIMIT)	// Official number of walkable cells is 14 if and only if there is an obstacle between. [malufett]
		&& (bl->type != BL_NPC)) // If type is a NPC, please disregard.
	{
		if (bl->type == BL_MOB) {
			ud->to_x = bl->x;
			ud->to_y = bl->y;
			for (i = 0; i < CLIENT_COMPLEX_PATH_LIMIT-1; i++) { ud->to_x = ud->to_x + dirx[wpd.path[i]];
			ud->to_y = ud->to_y + diry[wpd.path[i]];
			}

		}
			//return 0; // prevent monsters jumping through walls, client can't display
#ifdef OFFICIAL_WALKPATH
		return 0;
#endif
	}

	memcpy(&ud->walkpath,&wpd,sizeof(wpd));

	if (ud->target_to && ud->chaserange>1) {
		// Generally speaking, the walk path is already to an adjacent tile
		// so we only need to shorten the path if the range is greater than 1.
		// Trim the last part of the path to account for range,
		// but always move at least one cell when requested to move.
		for (i = (ud->chaserange*10)-10; i > 0 && ud->walkpath.path_len>1;) {
			ud->walkpath.path_len--;
			enum directions dir = ud->walkpath.path[ud->walkpath.path_len];
			if( direction_diagonal( dir ) )
				i -= MOVE_COST*20; //When chasing, units will target a diamond-shaped area in range [Playtester]
			else
				i -= MOVE_COST;
			ud->to_x -= dirx[dir];
			ud->to_y -= diry[dir];
		}
	}

	ud->state.change_walk_target=0;

	if (bl->type == BL_PC) {
		((TBL_PC *)bl)->head_dir = 0;
		clif_walkok((TBL_PC*)bl);
	}
	clif_move(ud);

	if(ud->walkpath.path_pos>=ud->walkpath.path_len)
		i = -1;
	else if( direction_diagonal( ud->walkpath.path[ud->walkpath.path_pos] ) )
		i = status_get_speed(bl)*MOVE_DIAGONAL_COST/MOVE_COST;
	else
		i = status_get_speed(bl);
	if( i > 0)
		ud->walktimer = add_timer(gettick()+i,unit_walktoxy_timer,bl->id,i);
	return 1;
}

/**
 * Retrieve the direct master of a bl if one exists.
 * @param bl: char to get his master [HOM|ELEM|PET|MER]
 * @return map_session_data of master or NULL
 */
TBL_PC* unit_get_master(struct block_list *bl)
{
	if(bl)
		switch(bl->type) {
			case BL_HOM: return (((TBL_HOM *)bl)->master);
			case BL_ELEM: return (((TBL_ELEM *)bl)->master);
			case BL_PET: return (((TBL_PET *)bl)->master);
			case BL_MER: return (((TBL_MER *)bl)->master);
		}
	return NULL;
}

/**
 * Retrieve a unit's master's teleport timer
 * @param bl: char to get his master's teleport timer [HOM|ELEM|PET|MER]
 * @return timer or NULL
 */
int* unit_get_masterteleport_timer(struct block_list *bl)
{
	if(bl)
		switch(bl->type) {
			case BL_HOM: return &(((TBL_HOM *)bl)->masterteleport_timer);
			case BL_ELEM: return &(((TBL_ELEM *)bl)->masterteleport_timer);
			case BL_PET: return &(((TBL_PET *)bl)->masterteleport_timer);
			case BL_MER: return &(((TBL_MER *)bl)->masterteleport_timer);
		}
	return NULL;
}

/**
 * Warps a unit to its master if the master has gone out of sight (3 second default)
 * Can be any object with a master [MOB|PET|HOM|MER|ELEM]
 * @param tid: Timer
 * @param tick: tick (unused)
 * @param id: Unit to warp
 * @param data: Data transferred from timer call
 * @return 0
 */
TIMER_FUNC(unit_teleport_timer){
	struct block_list *bl = map_id2bl(id);
	int *mast_tid = unit_get_masterteleport_timer(bl);

	if(tid == INVALID_TIMER || mast_tid == NULL)
		return 0;
	else if(*mast_tid != tid || bl == NULL)
		return 0;
	else {
		TBL_PC *msd = unit_get_master(bl);
		if(msd && !check_distance_bl(&msd->bl, bl, data)) {
			*mast_tid = INVALID_TIMER;
			unit_warp(bl, msd->bl.m, msd->bl.x, msd->bl.y, CLR_TELEPORT );
		} else // No timer needed
			*mast_tid = INVALID_TIMER;
	}
	return 0;
}

/**
 * Checks if a slave unit is outside their max distance from master
 * If so, starts a timer (default: 3 seconds) which will teleport the unit back to master
 * @param sbl: Object with a master [MOB|PET|HOM|MER|ELEM]
 * @return 0
 */
int unit_check_start_teleport_timer(struct block_list *sbl)
{
	TBL_PC *msd = NULL;
	int max_dist = 0;

	switch(sbl->type) {
		case BL_HOM:	
		case BL_ELEM:	
		case BL_PET:	
		case BL_MER:	
			msd = unit_get_master(sbl);
			break;
		default:
			return 0;
	}

	switch(sbl->type) {
		case BL_HOM:	max_dist = AREA_SIZE;			break;
		case BL_ELEM:	max_dist = MAX_ELEDISTANCE;		break;
		case BL_PET:	max_dist = AREA_SIZE;			break;
		case BL_MER:	max_dist = MAX_MER_DISTANCE;	break;
	}
	// If there is a master and it's a valid type
	if(msd && max_dist) {
		int *msd_tid = unit_get_masterteleport_timer(sbl);

		if(msd_tid == NULL)
			return 0;
		if (!check_distance_bl(&msd->bl, sbl, max_dist)) {
			if(*msd_tid == INVALID_TIMER || *msd_tid == 0)
				*msd_tid = add_timer(gettick()+3000,unit_teleport_timer,sbl->id,max_dist);
		} else {
			if(*msd_tid && *msd_tid != INVALID_TIMER)
				delete_timer(*msd_tid,unit_teleport_timer);
			*msd_tid = INVALID_TIMER; // Cancel recall
		}
	}
	return 0;
}

/**
 * Triggered on full step if stepaction is true and executes remembered action.
 * @param tid: Timer ID
 * @param tick: Unused
 * @param id: ID of bl to do the action
 * @param data: Not used
 * @return 1: Success 0: Fail (No valid bl)
 */
TIMER_FUNC(unit_step_timer){
	struct block_list *bl;
	struct unit_data *ud;
	int target_id;

	bl = map_id2bl(id);

	if (!bl || bl->prev == NULL)
		return 0;

	ud = unit_bl2ud(bl);

	if(!ud)
		return 0;

	if(ud->steptimer != tid) {
		ShowError("unit_step_timer mismatch %d != %d\n",ud->steptimer,tid);
		return 0;
	}

	ud->steptimer = INVALID_TIMER;

	if(!ud->stepaction)
		return 0;

	//Set to false here because if an error occurs, it should not be executed again
	ud->stepaction = false;

	if(!ud->target_to)
		return 0;

	//Flush target_to as it might contain map coordinates which should not be used by other functions
	target_id = ud->target_to;
	ud->target_to = 0;

	//If stepaction is set then we remembered a client request that should be executed on the next step
	//Execute request now if target is in attack range
	if(ud->stepskill_id && skill_get_inf(ud->stepskill_id) & INF_GROUND_SKILL) {
		//Execute ground skill
		struct map_data *md = &map[bl->m];			
		unit_skilluse_pos(bl, target_id%md->xs, target_id/md->xs, ud->stepskill_id, ud->stepskill_lv);
	} else {
		//If a player has target_id set and target is in range, attempt attack
		struct block_list *tbl = map_id2bl(target_id);
		if (!tbl || !status_check_visibility(bl, tbl)) {
			return 0;
		}
		if(ud->stepskill_id == 0) {
			//Execute normal attack
			unit_attack(bl, tbl->id, (ud->state.attack_continue) + 2);
		} else {
			//Execute non-ground skill
			unit_skilluse_id(bl, tbl->id, ud->stepskill_id, ud->stepskill_lv);
		}
	}

	return 1;
}

/**
 * Defines when to refresh the walking character to object and restart the timer if applicable
 * Also checks for speed update, target location, and slave teleport timers
 * @param tid: Timer ID
 * @param tick: Current tick to decide next timer update
 * @param data: Data used in timer calls
 * @return 0 or unit_walktoxy_sub() or unit_walktoxy()
 */
static TIMER_FUNC(unit_walktoxy_timer){
	int i;
	int x,y,dx,dy;
	unsigned char icewall_walk_block;
	struct block_list *bl;
	struct unit_data *ud;
	TBL_PC *sd=NULL;
	TBL_MOB *md=NULL;

	bl = map_id2bl(id);
	
	if(bl == NULL)
		return 0;

	switch(bl->type) { // svoid useless cast, we can only be 1 type
		case BL_PC: sd = BL_CAST(BL_PC, bl); break;
		case BL_MOB: md = BL_CAST(BL_MOB, bl); break;
	}

	ud = unit_bl2ud(bl);

	if(ud == NULL)
		return 0;

	if(ud->walktimer != tid) {
		ShowError("unit_walk_timer mismatch %d != %d\n",ud->walktimer,tid);
		return 0;
	}

	ud->walktimer = INVALID_TIMER;

	if (bl->prev == NULL)
		return 0; // Stop moved because it is missing from the block_list

	if(ud->walkpath.path_pos>=ud->walkpath.path_len)
		return 0;

	if(ud->walkpath.path[ud->walkpath.path_pos]>=DIR_MAX)
		return 1;

	x = bl->x;
	y = bl->y;

	enum directions dir = ud->walkpath.path[ud->walkpath.path_pos];
	ud->dir = dir;

	dx = dirx[dir];
	dy = diry[dir];

	// Get icewall walk block depending on Status Immune mode (players can't be trapped)
	if(md && status_has_mode(&md->status,MD_STATUS_IMMUNE))
		icewall_walk_block = battle_config.boss_icewall_walk_block;
	else if(md)
		icewall_walk_block = battle_config.mob_icewall_walk_block;
	else
		icewall_walk_block = 0;

	//Monsters will walk into an icewall from the west and south if they already started walking
	if(map_getcell(bl->m,x+dx,y+dy,CELL_CHKNOPASS) 
		&& (icewall_walk_block == 0 || !map_getcell(bl->m,x+dx,y+dy,CELL_CHKICEWALL) || dx < 0 || dy < 0))
		return unit_walktoxy_sub(bl);

	//Monsters can only leave icewalls to the west and south
	//But if movement fails more than icewall_walk_block times, they can ignore this rule
	if(md && md->walktoxy_fail_count < icewall_walk_block && map_getcell(bl->m,x,y,CELL_CHKICEWALL) && (dx > 0 || dy > 0)) {
		//Needs to be done here so that rudeattack skills are invoked
		md->walktoxy_fail_count++;
		clif_fixpos(bl);
		//Monsters in this situation first use a chase skill, then unlock target and then use an idle skill
		if (!(++ud->walk_count%WALK_SKILL_INTERVAL))
			mobskill_use(md, tick, -1);
		mob_unlocktarget(md, tick);
		if (!(++ud->walk_count%WALK_SKILL_INTERVAL))
			mobskill_use(md, tick, -1);
		return 0;
	}

	// Refresh view for all those we lose sight
	map_foreachinmovearea(clif_outsight, bl, AREA_SIZE, dx, dy, sd?BL_ALL:BL_PC, bl);

	x += dx;
	y += dy;
	map_moveblock(bl, x, y, tick);
	ud->walk_count++; // Walked cell counter, to be used for walk-triggered skills. [Skotlex]

	if (bl->x != x || bl->y != y || ud->walktimer != INVALID_TIMER)
		return 0; // map_moveblock has altered the object beyond what we expected (moved/warped it)

	ud->walktimer = CLIF_WALK_TIMER; // Arbitrary non-INVALID_TIMER value to make the clif code send walking packets
	map_foreachinmovearea(clif_insight, bl, AREA_SIZE, -dx, -dy, sd?BL_ALL:BL_PC, bl);
	ud->walktimer = INVALID_TIMER;

	// When stopped walking, immediately execute AI. This is required to ensure there is no time lost between walks waiting for the AI to trigger
	if (bl->type == BL_PC) add_timer(gettick() + 1, unit_autopilot_timer, id, 0);
	else if (bl->type == BL_HOM) add_timer(gettick() + 1, unit_autopilot_homunculus_timer, id, 0);

	if (bl->x == ud->to_x && bl->y == ud->to_y) {
		if (ud->walk_done_event[0]){
			char walk_done_event[EVENT_NAME_LENGTH];

			// Copying is required in case someone uses unitwalkto inside the event code
			safestrncpy(walk_done_event, ud->walk_done_event, EVENT_NAME_LENGTH);

			ud->state.walk_script = true;

			// Execute the event
			npc_event_do_id(walk_done_event,bl->id);

			ud->state.walk_script = false;

			// Check if the unit was killed
			if( status_isdead(bl) ){
				struct mob_data* md = BL_CAST(BL_MOB, bl);

				if( md && !md->spawn ){
					unit_free(bl, CLR_OUTSIGHT);
				}

				return 0;
			}

			// Check if another event was set
			if( !strcmp(ud->walk_done_event,walk_done_event) ){
				// If not remove it
				ud->walk_done_event[0] = 0;
			}
		}
	}

	switch(bl->type) {
		case BL_PC:
			if( !sd->npc_ontouch_.empty() )
				npc_touchnext_areanpc(sd,false);
			if(map_getcell(bl->m,x,y,CELL_CHKNPC)) {
				npc_touch_areanpc(sd,bl->m,x,y);
				if (bl->prev == NULL) // Script could have warped char, abort remaining of the function.
					return 0;
			} else
				sd->areanpc.clear();
			pc_cell_basilica(sd);
			break;
		case BL_MOB:
			//Movement was successful, reset walktoxy_fail_count
			md->walktoxy_fail_count = 0;
			if( map_getcell(bl->m,x,y,CELL_CHKNPC) ) {
				if( npc_touch_areanpc2(md) )
					return 0; // Warped
			} else
				md->areanpc_id = 0;
			if (md->min_chase > md->db->range3)
				md->min_chase--;
			// Walk skills are triggered regardless of target due to the idle-walk mob state.
			// But avoid triggering on stop-walk calls.
			if(tid != INVALID_TIMER &&
				!(ud->walk_count%WALK_SKILL_INTERVAL) &&
				map[bl->m].users > 0 &&
				mobskill_use(md, tick, -1)) {
				if (!(ud->skill_id == NPC_SELFDESTRUCTION && ud->skilltimer != INVALID_TIMER)
					&& md->state.skillstate != MSS_WALK) //Walk skills are supposed to be used while walking
				{ // Skill used, abort walking
					clif_fixpos(bl); // Fix position as walk has been cancelled.
					return 0;
				}
				// Resend walk packet for proper Self Destruction display.
				clif_move(ud);
			}
			break;
	}

	if(tid == INVALID_TIMER) // A directly invoked timer is from battle_stop_walking, therefore the rest is irrelevant.
		return 0;

	//If stepaction is set then we remembered a client request that should be executed on the next step
	if (ud->stepaction && ud->target_to) {
		//Delete old stepaction even if not executed yet, the latest command is what counts
		if(ud->steptimer != INVALID_TIMER) {
			delete_timer(ud->steptimer, unit_step_timer);
			ud->steptimer = INVALID_TIMER;
		}
		//Delay stepactions by half a step (so they are executed at full step)
		if( direction_diagonal( ud->walkpath.path[ud->walkpath.path_pos] ) )
			i = status_get_speed(bl)*MOVE_DIAGONAL_COST/MOVE_COST/2;
		else
			i = status_get_speed(bl)/2;
		ud->steptimer = add_timer(tick+i, unit_step_timer, bl->id, 0);
	}

	if(ud->state.change_walk_target) {
		if(unit_walktoxy_sub(bl)) {
			return 1;	
		} else {
			clif_fixpos(bl);
			return 0;
		}
	}

	ud->walkpath.path_pos++;

	if(ud->walkpath.path_pos >= ud->walkpath.path_len)
		i = -1;
	else if( direction_diagonal( ud->walkpath.path[ud->walkpath.path_pos] ) )
		i = status_get_speed(bl)*MOVE_DIAGONAL_COST/MOVE_COST;
	else
		i = status_get_speed(bl);

	if(i > 0) {
		ud->walktimer = add_timer(tick+i,unit_walktoxy_timer,id,i);
		if( md && DIFF_TICK(tick,md->dmgtick) < 3000 ) // Not required not damaged recently
			clif_move(ud);
	} else if(ud->state.running) { // Keep trying to run.
		if (!(unit_run(bl, NULL, SC_RUN) || unit_run(bl, sd, SC_WUGDASH)) )
			ud->state.running = 0;
	} else if (!ud->stepaction && ud->target_to) {
		// Update target trajectory.
		struct block_list *tbl = map_id2bl(ud->target_to);
		if (!tbl || !status_check_visibility(bl, tbl)) { // Cancel chase.
			ud->to_x = bl->x;
			ud->to_y = bl->y;

			if (tbl && bl->type == BL_MOB && mob_warpchase((TBL_MOB*)bl, tbl) )
				return 0;

			ud->target_to = 0;

			return 0;
		}
		if (tbl->m == bl->m && check_distance_bl(bl, tbl, ud->chaserange)) { // Reached destination.
			if (ud->state.attack_continue) {
				// Aegis uses one before every attack, we should
				// only need this one for syncing purposes. [Skotlex]
				ud->target_to = 0;
				clif_fixpos(bl);
				unit_attack(bl, tbl->id, ud->state.attack_continue);
			}
		} else { // Update chase-path
			unit_walktobl(bl, tbl, ud->chaserange, (ud->state.attack_continue?2:0));

			return 0;
		}
	} else { // Stopped walking. Update to_x and to_y to current location [Skotlex]
		ud->to_x = bl->x;
		ud->to_y = bl->y;

		if(battle_config.official_cell_stack_limit > 0
			&& map_count_oncell(bl->m, x, y, BL_CHAR|BL_NPC, 1) > battle_config.official_cell_stack_limit) {
			//Walked on occupied cell, call unit_walktoxy again
			if(ud->steptimer != INVALID_TIMER) {
				//Execute step timer on next step instead
				delete_timer(ud->steptimer, unit_step_timer);
				ud->steptimer = INVALID_TIMER;
			}
			return unit_walktoxy(bl, x, y, 8);
		}
	}

	return 0;
}

/**
 * Delays an xy timer
 * @param tid: Timer ID
 * @param tick: Unused
 * @param id: ID of bl to delay timer on
 * @param data: Data used in timer calls
 * @return 1: Success 0: Fail (No valid bl)
 */
TIMER_FUNC(unit_delay_walktoxy_timer){
	struct block_list *bl = map_id2bl(id);

	if (!bl || bl->prev == NULL)
		return 0;

	unit_walktoxy(bl, (short)((data>>16)&0xffff), (short)(data&0xffff), 0);

	return 1;
}

/**
 * Delays an walk-to-bl timer
 * @param tid: Timer ID
 * @param tick: Unused
 * @param id: ID of bl to delay timer on
 * @param data: Data used in timer calls (target bl)
 * @return 1: Success 0: Fail (No valid bl or target)
 */
TIMER_FUNC(unit_delay_walktobl_timer){
	struct block_list *bl = map_id2bl(id), *tbl = map_id2bl(data);

	if(!bl || bl->prev == NULL || tbl == NULL)
		return 0;
	else {
		struct unit_data* ud = unit_bl2ud(bl);
		unit_walktobl(bl, tbl, 0, 0);
		ud->target_to = 0;
	}

	return 1;
}


// do unit_walktoxy if and only if not already going towards that generic area, otherwise keep current movement order
void newwalk(struct block_list *bl, short x, short y, unsigned char flag)
{
	struct unit_data* ud = NULL;
	ud = unit_bl2ud(bl);

	if (ud == NULL)
		return;

	// We aren't yet walking or are walking to somewhere at least 3 tiles away from the intended destination, start a new walk
	if ((abs(x - ud->to_x) > 2) || (abs(y - ud->to_y) > 2) || (ud->walktimer == INVALID_TIMER)) {
		//unit_stop_walking(bl, USW_MOVE_ONCE);
		unit_walktoxy(bl, x, y, flag);
	}
}


/**
 * Begins the function of walking a unit to an x,y location
 * This is where the path searches and unit can_move checks are done
 * @param bl: Object to send to x,y coordinate
 * @param x: X coordinate where the object will be walking to
 * @param y: Y coordinate where the object will be walking to
 * @param flag: Parameter to decide how to walk
 *	&1: Easy walk (fail if CELL_CHKNOPASS is in direct path)
 *	&2: Force walking (override can_move)
 *	&4: Delay walking for can_move
 *	&8: Search for an unoccupied cell and cancel if none available
 * @return 1: Success 0: Fail or unit_walktoxy_sub()
 */
int unit_walktoxy( struct block_list *bl, short x, short y, unsigned char flag)
{
	struct unit_data* ud = NULL;
	struct status_change* sc = NULL;
	struct walkpath_data wpd;
	TBL_PC *sd = NULL;

	nullpo_ret(bl);

	ud = unit_bl2ud(bl);

	if (ud == NULL)
		return 0;

	if (bl->type == BL_PC)
		sd = BL_CAST(BL_PC, bl);

	if ((flag&8) && !map_closest_freecell(bl->m, &x, &y, BL_CHAR|BL_NPC, 1)) //This might change x and y
		return 0;

	if (!path_search(&wpd, bl->m, bl->x, bl->y, x, y, flag&1, CELL_CHKNOPASS)) // Count walk path cells
		return 0;

	if (!(path_search_long(NULL, bl->m, bl->x, bl->y, x, y, CELL_CHKNOPASS, MAX_WALKPATH)))
		if // Check if there is an obstacle between
		((wpd.path_len > CLIENT_COMPLEX_PATH_LIMIT)	// Official number of walkable cells is 14 if and only if there is an obstacle between. [malufett]
		&& (bl->type != BL_NPC)) // If type is a NPC, please disregard.
	{
			if (bl->type == BL_MOB) if (bl->type == BL_MOB) {
				x = bl->x;
				y = bl->y;
				for (int i = 0; i < CLIENT_COMPLEX_PATH_LIMIT - 1; i++) {
					x = x + dirx[wpd.path[i]];
					y = y + diry[wpd.path[i]];
				}
			}

			//return 0; // prevent monsters jumping through walls, client can't display
#ifdef OFFICIAL_WALKPATH
		return 0;
#endif
	}
	if ((wpd.path_len > battle_config.max_walk_path) && (bl->type != BL_NPC))
		return 0;

	if (flag&4) {
		unit_unattackable(bl);
		unit_stop_attack(bl);

		if(DIFF_TICK(ud->canmove_tick, gettick()) > 0 && DIFF_TICK(ud->canmove_tick, gettick()) < 2000) { // Delay walking command. [Skotlex]
			add_timer(ud->canmove_tick+1, unit_delay_walktoxy_timer, bl->id, (x<<16)|(y&0xFFFF));
			return 1;
		}
	}

	if(!(flag&2) && (!status_bl_has_mode(bl,MD_CANMOVE) || !unit_can_move(bl)))
		return 0;

	ud->state.walk_easy = flag&1;
	ud->to_x = x;
	ud->to_y = y;
	unit_stop_attack(bl); //Sets target to 0

	sc = status_get_sc(bl);
	if (sc && sc->data[SC_CONFUSION]) // Randomize the target position
		map_random_dir(bl, &ud->to_x, &ud->to_y);

	if(ud->walktimer != INVALID_TIMER) {
		// When you come to the center of the grid because the change of destination while you're walking right now
		// Call a function from a timer unit_walktoxy_sub
		ud->state.change_walk_target = 1;
		return 1;
	}

	// Start timer to recall summon
	if (sd && sd->md)
		unit_check_start_teleport_timer(&sd->md->bl);
	if (sd && sd->ed)
		unit_check_start_teleport_timer(&sd->ed->bl);
	if (sd && sd->hd)
		unit_check_start_teleport_timer(&sd->hd->bl);
	if (sd && sd->pd)
		unit_check_start_teleport_timer(&sd->pd->bl);

	return unit_walktoxy_sub(bl);
}

/**
 * Sets a mob's CHASE/FOLLOW state
 * This should not be done if there's no path to reach
 * @param bl: Mob to set state on
 * @param flag: Whether to set state or not
 */
static inline void set_mobstate(struct block_list* bl, int flag)
{
	struct mob_data* md = BL_CAST(BL_MOB,bl);

	if( md && flag )
		md->state.skillstate = md->state.aggressive ? MSS_FOLLOW : MSS_RUSH;
}

/**
 * Timer to walking a unit to another unit's location
 * Calls unit_walktoxy_sub once determined the unit can move
 * @param tid: Object's timer ID
 * @param id: Object's ID
 * @param data: Data passed through timer function (target)
 * @return 0
 */
static TIMER_FUNC(unit_walktobl_sub){
	struct block_list *bl = map_id2bl(id);
	struct unit_data *ud = bl?unit_bl2ud(bl):NULL;

	if (ud && ud->walktimer == INVALID_TIMER && ud->target == data) {
		if (DIFF_TICK(ud->canmove_tick, tick) > 0) // Keep waiting?
			add_timer(ud->canmove_tick+1, unit_walktobl_sub, id, data);
		else if (unit_can_move(bl)) {
			if (unit_walktoxy_sub(bl))
				set_mobstate(bl, ud->state.attack_continue);
		}
	}

	return 0;
}

/**
 * Tells a unit to walk to a target's location (chase)
 * @param bl: Object that is walking to target
 * @param tbl: Target object
 * @param range: How close to get to target (or attack range if flag&2)
 * @param flag: Extra behaviour
 *	&1: Use easy path seek (obstacles will not be walked around)
 *	&2: Start attacking upon arrival within range, otherwise just walk to target
 * @return 1: Started walking or set timer 0: Failed
 */
int unit_walktobl(struct block_list *bl, struct block_list *tbl, int range, unsigned char flag)
{
	struct unit_data *ud = NULL;
	struct status_change *sc = NULL;

	nullpo_ret(bl);
	nullpo_ret(tbl);

	ud = unit_bl2ud(bl);

	if(ud == NULL)
		return 0;

	if (!status_bl_has_mode(bl,MD_CANMOVE))
		return 0;


	if (!unit_can_reach_bl(bl, tbl, distance_bl(bl, tbl)+1, flag&1, &ud->to_x, &ud->to_y)) {
		ud->to_x = bl->x;
		ud->to_y = bl->y;
		ud->target_to = 0;

		return 0;
	} else if (range == 0) {
		//Should walk on the same cell as target (for looters)
		ud->to_x = tbl->x;
		ud->to_y = tbl->y;
	}

	ud->state.walk_easy = flag&1;
	ud->target_to = tbl->id;
	ud->chaserange = range; // Note that if flag&2, this SHOULD be attack-range
	ud->state.attack_continue = flag&2?1:0; // Chase to attack.
	unit_stop_attack(bl); //Sets target to 0

	sc = status_get_sc(bl);
	if (sc && sc->data[SC_CONFUSION]) // Randomize the target position
		map_random_dir(bl, &ud->to_x, &ud->to_y);

	if(ud->walktimer != INVALID_TIMER) {
		ud->state.change_walk_target = 1;
		set_mobstate(bl, flag&2);

		return 1;
	}

	if(DIFF_TICK(ud->canmove_tick, gettick()) > 0) { // Can't move, wait a bit before invoking the movement.
		add_timer(ud->canmove_tick+1, unit_walktobl_sub, bl->id, ud->target);
		return 1;
	}

	if(!unit_can_move(bl))
		return 0;

	if (unit_walktoxy_sub(bl)) {
		set_mobstate(bl, flag&2);

		return 1;
	}

	return 0;
}

/**
 * Called by unit_run when an object is hit.
 * @param sd Required only when using SC_WUGDASH
 */
void unit_run_hit(struct block_list *bl, struct status_change *sc, struct map_session_data *sd, enum sc_type type)
{
	int lv = sc->data[type]->val1;

	// If you can't run forward, you must be next to a wall, so bounce back. [Skotlex]
	if (type == SC_RUN)
		clif_status_change(bl, EFST_TING, 1, 0, 0, 0, 0);

	// Set running to 0 beforehand so status_change_end knows not to enable spurt [Kevin]
	unit_bl2ud(bl)->state.running = 0;
	status_change_end(bl, type, INVALID_TIMER);

	if (type == SC_RUN) {
		skill_blown(bl, bl, skill_get_blewcount(TK_RUN, lv), unit_getdir(bl), BLOWN_NONE);
		clif_status_change(bl, EFST_TING, 0, 0, 0, 0, 0);
	} else if (sd) {
		clif_fixpos(bl);
		skill_castend_damage_id(bl, &sd->bl, RA_WUGDASH, lv, gettick(), SD_LEVEL);
	}
	return;
}

/**
 * Set a unit to run, checking for obstacles
 * @param bl: Object that is running
 * @param sd: Required only when using SC_WUGDASH
 * @return true: Success (Finished running) false: Fail (Hit an object/Couldn't run)
 */
bool unit_run(struct block_list *bl, struct map_session_data *sd, enum sc_type type)
{
	struct status_change *sc;
	short to_x, to_y, dir_x, dir_y;
	int i;

	nullpo_retr(false, bl);

	sc = status_get_sc(bl);

	if (!(sc && sc->data[type]))
		return false;

	if (!unit_can_move(bl)) {
		status_change_end(bl, type, INVALID_TIMER);
		return false;
	}

	dir_x = dirx[sc->data[type]->val2];
	dir_y = diry[sc->data[type]->val2];

	// Determine destination cell
	to_x = bl->x;
	to_y = bl->y;

	// Search for available path
	for(i = 0; i < AREA_SIZE; i++) {
		if(!map_getcell(bl->m, to_x + dir_x, to_y + dir_y, CELL_CHKPASS))
			break;

		// If sprinting and there's a PC/Mob/NPC, block the path [Kevin]
		if(map_count_oncell(bl->m, to_x + dir_x, to_y + dir_y, BL_PC|BL_MOB|BL_NPC, 0))
			break;

		to_x += dir_x;
		to_y += dir_y;
	}

	// Can't run forward.
	if( (to_x == bl->x && to_y == bl->y) || (to_x == (bl->x + 1) || to_y == (bl->y + 1)) || (to_x == (bl->x - 1) || to_y == (bl->y - 1))) {
		unit_run_hit(bl, sc, sd, type);
		return false;
	}

	if (unit_walktoxy(bl, to_x, to_y, 1))
		return true;

	// There must be an obstacle nearby. Attempt walking one cell at a time.
	do {
		to_x -= dir_x;
		to_y -= dir_y;
	} while (--i > 0 && !unit_walktoxy(bl, to_x, to_y, 1));

	if (i == 0) {
		unit_run_hit(bl, sc, sd, type);
		return false;
	}

	return true;
}

/**
 * Makes unit attempt to run away from target using hard paths
 * @param bl: Object that is running away from target
 * @param target: Target
 * @param dist: How far bl should run
 * @return 1: Success 0: Fail
 */
int unit_escape(struct block_list *bl, struct block_list *target, short dist)
{
	uint8 dir = map_calc_dir(target, bl->x, bl->y);

	while( dist > 0 && map_getcell(bl->m, bl->x + dist*dirx[dir], bl->y + dist*diry[dir], CELL_CHKNOREACH) )
		dist--;

	return ( dist > 0 && unit_walktoxy(bl, bl->x + dist*dirx[dir], bl->y + dist*diry[dir], 0) );
}

/**
 * Instant warps a unit to x,y coordinate
 * @param bl: Object to instant warp
 * @param dst_x: X coordinate to warp to
 * @param dst_y: Y coordinate to warp to
 * @param easy: 
 *		0: Hard path check (attempt to go around obstacle)
 *		1: Easy path check (no obstacle on movement path)
 *		2: Long path check (no obstacle on line from start to destination)
 * @param checkpath: Whether or not to do a cell and path check for NOPASS and NOREACH
 * @return True: Success False: Fail
 */
bool unit_movepos(struct block_list *bl, short dst_x, short dst_y, int easy, bool checkpath)
{
	short dx,dy;
	uint8 dir;
	struct unit_data        *ud = NULL;
	struct map_session_data *sd = NULL;

	nullpo_retr(false,bl);

	sd = BL_CAST(BL_PC, bl);
	ud = unit_bl2ud(bl);

	if(ud == NULL)
		return false;

	unit_stop_walking(bl, 1);
	unit_stop_attack(bl);

	if( checkpath && (map_getcell(bl->m,dst_x,dst_y,CELL_CHKNOPASS) || !path_search(NULL,bl->m,bl->x,bl->y,dst_x,dst_y,easy,CELL_CHKNOREACH)) )
		return false; // Unreachable

	ud->to_x = dst_x;
	ud->to_y = dst_y;

	dir = map_calc_dir(bl, dst_x, dst_y);
	ud->dir = dir;

	dx = dst_x - bl->x;
	dy = dst_y - bl->y;

	map_foreachinmovearea(clif_outsight, bl, AREA_SIZE, dx, dy, (sd ? BL_ALL : BL_PC), bl);

	map_moveblock(bl, dst_x, dst_y, gettick());

	ud->walktimer = CLIF_WALK_TIMER; // Arbitrary non-INVALID_TIMER value to make the clif code send walking packets
	map_foreachinmovearea(clif_insight, bl, AREA_SIZE, -dx, -dy, (sd ? BL_ALL : BL_PC), bl);
	ud->walktimer = INVALID_TIMER;

	if(sd) {
		if( !sd->npc_ontouch_.empty() )
			npc_touchnext_areanpc(sd,false);

		if(map_getcell(bl->m,bl->x,bl->y,CELL_CHKNPC)) {
			npc_touch_areanpc(sd, bl->m, bl->x, bl->y);

			if (bl->prev == NULL) // Script could have warped char, abort remaining of the function.
				return false;
		} else
			sd->areanpc.clear();

		if( sd->status.pet_id > 0 && sd->pd && sd->pd->pet.intimate > PET_INTIMATE_NONE ) {
			// Check if pet needs to be teleported. [Skotlex]
			int flag = 0;
			struct block_list* pbl = &sd->pd->bl;

			if( !checkpath && !path_search(NULL,pbl->m,pbl->x,pbl->y,dst_x,dst_y,0,CELL_CHKNOPASS) )
				flag = 1;
			else if (!check_distance_bl(&sd->bl, pbl, AREA_SIZE)) // Too far, teleport.
				flag = 2;

			if( flag ) {
				unit_movepos(pbl,sd->bl.x,sd->bl.y, 0, 0);
				clif_slide(pbl,pbl->x,pbl->y);
			}
		}
	}

	return true;
}

/**
 * Sets direction of a unit
 * @param bl: Object to set direction
 * @param dir: Direction (0-7)
 * @return 0
 */
int unit_setdir(struct block_list *bl, unsigned char dir)
{
	struct unit_data *ud;

	nullpo_ret(bl);

	ud = unit_bl2ud(bl);

	if (!ud)
		return 0;

	ud->dir = dir;

	if (bl->type == BL_PC)
		((TBL_PC *)bl)->head_dir = 0;

	clif_changed_dir(bl, AREA);

	return 0;
}

/**
 * Gets direction of a unit
 * @param bl: Object to get direction
 * @return direction (0-7)
 */
uint8 unit_getdir(struct block_list *bl)
{
	struct unit_data *ud;

	nullpo_ret(bl);

	ud = unit_bl2ud(bl);

	if (!ud)
		return 0;

	return ud->dir;
}

/**
 * Pushes a unit in a direction by a given amount of cells
 * There is no path check, only map cell restrictions are respected
 * @param bl: Object to push
 * @param dx: Destination cell X
 * @param dy: Destination cell Y
 * @param count: How many cells to push bl
 * @param flag: See skill.hpp::e_skill_blown
 * @return count (can be modified due to map cell restrictions)
 */
int unit_blown(struct block_list* bl, int dx, int dy, int count, enum e_skill_blown flag)
{
	if(count) {
		struct map_session_data* sd;
		struct skill_unit* su = NULL;
		int nx, ny, result;

		sd = BL_CAST(BL_PC, bl);
		su = BL_CAST(BL_SKILL, bl);

		result = path_blownpos(bl->m, bl->x, bl->y, dx, dy, count);

		nx = result>>16;
		ny = result&0xffff;

		if(!su)
			unit_stop_walking(bl, 0);

		if( sd ) {
			unit_stop_stepaction(bl); //Stop stepaction when knocked back
			sd->ud.to_x = nx;
			sd->ud.to_y = ny;
		}

		dx = nx-bl->x;
		dy = ny-bl->y;

		if(dx || dy) {
			map_foreachinmovearea(clif_outsight, bl, AREA_SIZE, dx, dy, bl->type == BL_PC ? BL_ALL : BL_PC, bl);

			if(su) {
				if (su->group && skill_get_unit_flag(su->group->skill_id)&UF_KNOCKBACK_GROUP)
					skill_unit_move_unit_group(su->group, bl->m, dx, dy);
				else
					skill_unit_move_unit(bl, nx, ny);
			} else
				map_moveblock(bl, nx, ny, gettick());

			map_foreachinmovearea(clif_insight, bl, AREA_SIZE, -dx, -dy, bl->type == BL_PC ? BL_ALL : BL_PC, bl);

			if(!(flag&BLOWN_DONT_SEND_PACKET))
				clif_blown(bl);

			if(sd) {
				if(!sd->npc_ontouch_.empty())
					npc_touchnext_areanpc(sd, false);

				if(map_getcell(bl->m, bl->x, bl->y, CELL_CHKNPC))
					npc_touch_areanpc(sd, bl->m, bl->x, bl->y);
				else
					sd->areanpc.clear();
			}
		}

		count = distance(dx, dy);
	}

	return count;  // Return amount of knocked back cells
}

/**
 * Checks if unit can be knocked back / stopped by skills.
 * @param bl: Object to check
 * @param flag
 *		0x1 - Offensive (not set: self skill, e.g. Backslide)
 *		0x2 - Knockback type (not set: Stop type, e.g. Ankle Snare)
 *		0x4 - Boss attack
 * @return reason for immunity
 *		UB_KNOCKABLE - can be knocked back / stopped
 *		UB_NO_KNOCKBACK_MAP - at WOE/BG map
 *		UB_MD_KNOCKBACK_IMMUNE - target is MD_KNOCKBACK_IMMUNE
 *		UB_TARGET_BASILICA - target is in Basilica area
 *		UB_TARGET_NO_KNOCKBACK - target has 'special_state.no_knockback'
 *		UB_TARGET_TRAP - target is trap that cannot be knocked back
 */
enum e_unit_blown unit_blown_immune(struct block_list* bl, uint8 flag)
{
	if ((flag&0x1)
		&& (map_flag_gvg2(bl->m) || map_getmapflag(bl->m, MF_BATTLEGROUND))
		&& ((flag&0x2) || !(battle_config.skill_trap_type&0x1)))
		return UB_NO_KNOCKBACK_MAP; // No knocking back in WoE / BG

	switch (bl->type) {
		case BL_MOB:
			// Immune can't be knocked back
			if (((flag&0x1) && status_bl_has_mode(bl,MD_KNOCKBACK_IMMUNE))
				&& ((flag&0x2) || !(battle_config.skill_trap_type&0x2)))
				return UB_MD_KNOCKBACK_IMMUNE;
			break;
		case BL_PC: {
				struct map_session_data *sd = BL_CAST(BL_PC, bl);
				// Basilica caster can't be knocked-back by normal monsters.
				if( !(flag&0x4) && sd->sc.data[SC_BASILICA] && sd->sc.data[SC_BASILICA]->val4 == sd->bl.id)
					return UB_TARGET_BASILICA;
				// Target has special_state.no_knockback (equip)
				if( (flag&(0x1|0x2)) && sd->special_state.no_knockback )
					return UB_TARGET_NO_KNOCKBACK;
			}
			break;
		case BL_SKILL: {
				struct skill_unit* su = (struct skill_unit *)bl;
				// Trap cannot be knocked back
				if (su && su->group && skill_get_unit_flag(su->group->skill_id)&UF_NOKNOCKBACK)
					return UB_TARGET_TRAP;
			}
			break;
	}

	//Object can be knocked back / stopped
	return UB_KNOCKABLE;
}

/**
 * Warps a unit to a map/position
 * pc_setpos is used for player warping
 * This function checks for "no warp" map flags, so it's safe to call without doing nowarpto/nowarp checks
 * @param bl: Object to warp
 * @param m: Map ID from bl structure (NOT index)
 * @param x: Destination cell X
 * @param y: Destination cell Y
 * @param type: Clear type used in clif_clearunit_area()
 * @return Success(0); Failed(1); Error(2); unit_remove_map() Failed(3); map_addblock Failed(4)
 */
int unit_warp(struct block_list *bl,short m,short x,short y,clr_type type)
{
	struct unit_data *ud;

	nullpo_ret(bl);

	ud = unit_bl2ud(bl);

	if(bl->prev==NULL || !ud)
		return 1;

	if (type == CLR_DEAD)
		// Type 1 is invalid, since you shouldn't warp a bl with the "death"
		// animation, it messes up with unit_remove_map! [Skotlex]
		return 1;

	if( m < 0 )
		m = bl->m;

	switch (bl->type) {
		case BL_MOB:
			if (map_getmapflag(bl->m, MF_MONSTER_NOTELEPORT) && ((TBL_MOB*)bl)->master_id == 0)
				return 1;

			if (m != bl->m && map_getmapflag(m, MF_NOBRANCH) && battle_config.mob_warp&4 && !(((TBL_MOB *)bl)->master_id))
				return 1;
			break;
		case BL_PC:
			if (map_getmapflag(bl->m, MF_NOTELEPORT))
				return 1;
			break;
	}

	if (x < 0 || y < 0) { // Random map position.
		if (!map_search_freecell(NULL, m, &x, &y, -1, -1, 1)) {
			ShowWarning("unit_warp failed. Unit Id:%d/Type:%d, target position map %d (%s) at [%d,%d]\n", bl->id, bl->type, m, map[m].name, x, y);
			return 2;

		}
	} else if ( bl->type != BL_NPC && map_getcell(m,x,y,CELL_CHKNOREACH)) { // Invalid target cell
		ShowWarning("unit_warp: Specified non-walkable target cell: %d (%s) at [%d,%d]\n", m, map[m].name, x,y);

		if (!map_search_freecell(NULL, m, &x, &y, 4, 4, 1)) { // Can't find a nearby cell
			ShowWarning("unit_warp failed. Unit Id:%d/Type:%d, target position map %d (%s) at [%d,%d]\n", bl->id, bl->type, m, map[m].name, x, y);
			return 2;
		}
	}

	if (bl->type == BL_PC) // Use pc_setpos
		return pc_setpos((TBL_PC*)bl, map_id2index(m), x, y, type);

	if (!unit_remove_map(bl, type))
		return 3;

	if (bl->m != m && battle_config.clear_unit_onwarp &&
		battle_config.clear_unit_onwarp&bl->type)
		skill_clear_unitgroup(bl);

	bl->x = ud->to_x = x;
	bl->y = ud->to_y = y;
	bl->m = m;

	if (bl->type == BL_NPC) {
		TBL_NPC *nd = (TBL_NPC*)bl;
		map_addnpc(m, nd);
		npc_setcells(nd);
	}

	if(map_addblock(bl))
		return 4; //error on adding bl to map

	clif_spawn(bl);
	skill_unit_move(bl,gettick(),1);

	return 0;
}

/**
 * Stops a unit from walking
 * @param bl: Object to stop walking
 * @param type: Options
 *	USW_FIXPOS: Issue a fixpos packet afterwards
 *	USW_MOVE_ONCE: Force the unit to move one cell if it hasn't yet
 *	USW_MOVE_FULL_CELL: Enable moving to the next cell when unit was already half-way there
 *		(may cause on-touch/place side-effects, such as a scripted map change)
 *	USW_FORCE_STOP: Force stop moving, even if walktimer is currently INVALID_TIMER
 * @return Success(1); Failed(0);
 */
int unit_stop_walking(struct block_list *bl,int type)
{
	struct unit_data *ud;
	const struct TimerData* td = NULL;
	t_tick tick;

	nullpo_ret(bl);

	ud = unit_bl2ud(bl);

	if(!ud || (!(type&USW_FORCE_STOP) && ud->walktimer == INVALID_TIMER))
		return 0;

	// NOTE: We are using timer data after deleting it because we know the
	// delete_timer function does not mess with it. If the function's
	// behaviour changes in the future, this code could break!
	if (ud->walktimer != INVALID_TIMER) {
		td = get_timer(ud->walktimer);
		delete_timer(ud->walktimer, unit_walktoxy_timer);
		ud->walktimer = INVALID_TIMER;
	}
	ud->state.change_walk_target = 0;
	tick = gettick();

	if( (type&USW_MOVE_ONCE && !ud->walkpath.path_pos) // Force moving at least one cell.
	||  (type&USW_MOVE_FULL_CELL && td && DIFF_TICK(td->tick, tick) <= td->data/2) // Enough time has passed to cover half-cell
	) {
		ud->walkpath.path_len = ud->walkpath.path_pos+1;
		unit_walktoxy_timer(INVALID_TIMER, tick, bl->id, ud->walkpath.path_pos);
	}

	if(type&USW_FIXPOS)
		clif_fixpos(bl);

	ud->walkpath.path_len = 0;
	ud->walkpath.path_pos = 0;
	ud->to_x = bl->x;
	ud->to_y = bl->y;

	if(bl->type == BL_PET && type&~USW_ALL)
		ud->canmove_tick = gettick() + (type>>8);

	// Re-added, the check in unit_set_walkdelay means dmg during running won't fall through to this place in code [Kevin]
	if (ud->state.running) {
		status_change_end(bl, SC_RUN, INVALID_TIMER);
		status_change_end(bl, SC_WUGDASH, INVALID_TIMER);
	}

	return 1;
}

/**
 * Initiates a skill use by a unit
 * @param src: Source object initiating skill use
 * @param target_id: Target ID (bl->id)
 * @param skill_id: Skill ID
 * @param skill_lv: Skill Level
 * @return unit_skilluse_id2()
 */
int unit_skilluse_id(struct block_list *src, int target_id, uint16 skill_id, uint16 skill_lv, bool walkqueue)
{
	return unit_skilluse_id2(
		src, target_id, skill_id, skill_lv,
		skill_castfix(src, skill_id, skill_lv),
		skill_get_castcancel(skill_id),  walkqueue
	);
}
int unit_skilluse_id(struct block_list *src, int target_id, uint16 skill_id, uint16 skill_lv)
{
	return unit_skilluse_id2(
		src, target_id, skill_id, skill_lv,
		skill_castfix(src, skill_id, skill_lv),
		skill_get_castcancel(skill_id), true
		);
}

/**
 * Checks if a unit is walking
 * @param bl: Object to check walk status
 * @return Walking(1); Not Walking(0)
 */
int unit_is_walking(struct block_list *bl)
{
	struct unit_data *ud = unit_bl2ud(bl);

	nullpo_ret(bl);

	if(!ud)
		return 0;

	return (ud->walktimer != INVALID_TIMER);
}

/** 
 * Checks if a unit is able to move based on status changes
 * View the StatusChangeStateTable in status.cpp for a list of statuses
 * Some statuses are still checked here due too specific variables
 * @author [Skotlex]
 * @param bl: Object to check
 * @return Can move(1); Can't move(0)
 */
int unit_can_move(struct block_list *bl) {
	struct map_session_data *sd;
	struct unit_data *ud;
	struct status_change *sc;

	nullpo_ret(bl);

	ud = unit_bl2ud(bl);
	sc = status_get_sc(bl);
	sd = BL_CAST(BL_PC, bl);

	if (!ud)
		return 0;

	if (ud->skilltimer != INVALID_TIMER && ud->skill_id != LG_EXEEDBREAK && (!sd || !pc_checkskill(sd, SA_FREECAST) || skill_get_inf2(ud->skill_id)&INF2_GUILD_SKILL))
		return 0; // Prevent moving while casting

	if (DIFF_TICK(ud->canmove_tick, gettick()) > 0)
		return 0;

	if ((sd && (pc_issit(sd) || sd->state.vending || sd->state.buyingstore || (sd->state.block_action & PCBLOCK_MOVE))) || ud->state.blockedmove)
		return 0; // Can't move

	// Status changes that block movement
	if (sc) {
		if( sc->cant.move // status placed here are ones that cannot be cached by sc->cant.move for they depend on other conditions other than their availability
			|| sc->data[SC_SPIDERWEB]
			|| (sc->data[SC_DANCING] && (
				(sc->data[SC_DANCING]->val1&0xFFFF) == CG_MOONLIT 
//			||	(sc->data[SC_DANCING]->val1&0xFFFF) == CG_HERMODE
				) )
			)
			return 0;

		if (sc->opt1 > 0 && sc->opt1 != OPT1_STONEWAIT && sc->opt1 != OPT1_BURNING)
			return 0;

		if ((sc->option & OPTION_HIDE) && (!sd || pc_checkskill(sd, RG_TUNNELDRIVE) <= 0))
			return 0;
	}

	// Icewall walk block special trapped monster mode
	if(bl->type == BL_MOB) {
		struct mob_data *md = BL_CAST(BL_MOB, bl);
		if(md && ((status_has_mode(&md->status,MD_STATUS_IMMUNE) && battle_config.boss_icewall_walk_block == 1 && map_getcell(bl->m,bl->x,bl->y,CELL_CHKICEWALL))
			|| (!status_has_mode(&md->status,MD_STATUS_IMMUNE) && battle_config.mob_icewall_walk_block == 1 && map_getcell(bl->m,bl->x,bl->y,CELL_CHKICEWALL)))) {
			md->walktoxy_fail_count = 1; //Make sure rudeattacked skills are invoked
			return 0;
		}
	}

	return 1;
}

/**
 * Resumes running (RA_WUGDASH or TK_RUN) after a walk delay
 * @param tid: Timer ID
 * @param id: Object ID
 * @param data: Data passed through timer function (unit_data)
 * @return 0
 */
TIMER_FUNC(unit_resume_running){
	struct unit_data *ud = (struct unit_data *)data;
	TBL_PC *sd = map_id2sd(id);

	if (sd && pc_isridingwug(sd))
		clif_skill_nodamage(ud->bl,ud->bl,RA_WUGDASH,ud->skill_lv,
			sc_start4(ud->bl,ud->bl,status_skill2sc(RA_WUGDASH),100,ud->skill_lv,unit_getdir(ud->bl),0,0,0));
	else
		clif_skill_nodamage(ud->bl,ud->bl,TK_RUN,ud->skill_lv,
			sc_start4(ud->bl,ud->bl,status_skill2sc(TK_RUN),100,ud->skill_lv,unit_getdir(ud->bl),0,0,0));

	if (sd)
		clif_walkok(sd);

	return 0;
}

/**
 * Applies a walk delay to a unit
 * @param bl: Object to apply walk delay to
 * @param tick: Current tick
 * @param delay: Amount of time to set walk delay
 * @param type: Type of delay
 *	0: Damage induced delay; Do not change previous delay
 *	1: Skill induced delay; Walk delay can only be increased, not decreased
 * @return Success(1); Fail(0);
 */
int unit_set_walkdelay(struct block_list *bl, t_tick tick, t_tick delay, int type)
{
	struct unit_data *ud = unit_bl2ud(bl);

	if (delay <= 0 || !ud)
		return 0;

	if (type) {
		//Bosses can ignore skill induced walkdelay (but not damage induced)
		if(bl->type == BL_MOB && status_has_mode(status_get_status_data(bl),MD_STATUS_IMMUNE))
			return 0;
		//Make sure walk delay is not decreased
		if (DIFF_TICK(ud->canmove_tick, tick+delay) > 0)
			return 0;
	} else {
		// Don't set walk delays when already trapped.
		if (!unit_can_move(bl)) {
			unit_stop_walking(bl,4); //Unit might still be moving even though it can't move
			return 0;
		}
		//Immune to being stopped for double the flinch time
		if (DIFF_TICK(ud->canmove_tick, tick-delay) > 0)
			return 0;
	}

	ud->canmove_tick = tick + delay;

	if (ud->walktimer != INVALID_TIMER) { // Stop walking, if chasing, readjust timers.
		if (delay == 1) // Minimal delay (walk-delay) disabled. Just stop walking.
			unit_stop_walking(bl,0);
		else {
			// Resume running after can move again [Kevin]
			if(ud->state.running)
				add_timer(ud->canmove_tick, unit_resume_running, bl->id, (intptr_t)ud);
			else {
				unit_stop_walking(bl,4);

				if(ud->target)
					add_timer(ud->canmove_tick+1, unit_walktobl_sub, bl->id, ud->target);
			}
		}
	}

	return 1;
}

/**
 * Performs checks for a unit using a skill and executes after cast time completion
 * @param src: Object using skill
 * @param target_id: Target ID (bl->id)
 * @param skill_id: Skill ID
 * @param skill_lv: Skill Level
 * @param casttime: Initial cast time before cast time reductions
 * @param castcancel: Whether or not the skill can be cancelled by interruption (hit)
 * @return Success(1); Fail(0);
 */
int unit_skilluse_id2(struct block_list *src, int target_id, uint16 skill_id, uint16 skill_lv, int casttime, int castcancel)
{
	return unit_skilluse_id2(src, target_id, skill_id, skill_lv, casttime, castcancel, true);
}
int unit_skilluse_id2(struct block_list *src, int target_id, uint16 skill_id, uint16 skill_lv, int casttime, int castcancel, bool walkqueue)
{
	struct unit_data *ud;
	struct status_data *tstatus;
	struct status_change *sc;
	struct map_session_data *sd = NULL;
	struct block_list * target = NULL;
	t_tick tick = gettick();
	int combo = 0, range;
	uint8 inf = 0;
	uint32 inf2 = 0;

	nullpo_ret(src);

	if(status_isdead(src))
		return 0; // Do not continue source is dead

	sd = BL_CAST(BL_PC, src);
	ud = unit_bl2ud(src);

	if(ud == NULL)
		return 0;

	if (ud && ud->state.blockedskill)
		return 0;

	sc = status_get_sc(src);

	if (sc && !sc->count)
		sc = NULL; // Unneeded

	inf = skill_get_inf(skill_id);
	inf2 = skill_get_inf2(skill_id);

	// temp: used to signal combo-skills right now.
	if (sc && sc->data[SC_COMBO] &&
		skill_is_combo(skill_id) &&
		(sc->data[SC_COMBO]->val1 == skill_id ||
		(sd?skill_check_condition_castbegin(sd,skill_id,skill_lv):0) )) {
		if (skill_is_combo(skill_id) == 2 && target_id == src->id && ud->target > 0)
			target_id = ud->target;
		else if (sc->data[SC_COMBO]->val2)
			target_id = sc->data[SC_COMBO]->val2;
		else if (target_id == src->id || ud->target > 0)
			target_id = ud->target;

		if (inf&INF_SELF_SKILL && skill_get_nk(skill_id)&NK_NO_DAMAGE)// exploit fix
			target_id = src->id;

		combo = 1;
	} else if ( target_id == src->id &&
		inf&INF_SELF_SKILL &&
		(inf2&INF2_NO_TARGET_SELF ||
		(skill_id == RL_QD_SHOT && sc && sc->data[SC_QD_SHOT_READY])) ) {
		target_id = ud->target; // Auto-select target. [Skotlex]
		combo = 1;
	}

	if (sd) {
		// Target_id checking.
		if(skill_isNotOk(skill_id, sd))
			return 0;

		switch(skill_id) { // Check for skills that auto-select target
			case MO_CHAINCOMBO:
				if (sc && sc->data[SC_BLADESTOP]) {
					if ((target=map_id2bl(sc->data[SC_BLADESTOP]->val4)) == NULL)
						return 0;
				}
				break;
			case WE_MALE:
			case WE_FEMALE:
				if (!sd->status.partner_id)
					return 0;

				target = (struct block_list*)map_charid2sd(sd->status.partner_id);

				if (!target) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0);
					return 0;
				}
				break;
		}

		if (target)
			target_id = target->id;
	} else if (src->type == BL_HOM) {
		switch(skill_id) { // Homun-auto-target skills.
			case HLIF_HEAL:
			case HLIF_AVOID:
			case HAMI_DEFENCE:
			case HAMI_CASTLE:
				target = battle_get_master(src);

				if (!target)
					return 0;

				target_id = target->id;
				break;
			case MH_SONIC_CRAW:
			case MH_TINDER_BREAKER: {
				int skill_id2 = ((skill_id==MH_SONIC_CRAW)?MH_MIDNIGHT_FRENZY:MH_EQC);

				if(sc->data[SC_COMBO] && sc->data[SC_COMBO]->val1 == skill_id2) { // It's a combo
					target_id = sc->data[SC_COMBO]->val2;
					combo = 1;
					casttime = -1;
				}
				break;
			}
		}
	}

	if( !target ) // Choose default target
		target = map_id2bl(target_id);

	if( !target || src->m != target->m || !src->prev || !target->prev )
		return 0;

	if( battle_config.ksprotection && sd && mob_ksprotected(src, target) )
		return 0;

	// Normally not needed because clif.cpp checks for it, but the at/char/script commands don't! [Skotlex]
	if(ud->skilltimer != INVALID_TIMER && skill_id != SA_CASTCANCEL && skill_id != SO_SPELLFIST)
		return 0;

	if(inf2&INF2_NO_TARGET_SELF && src->id == target_id)
		return 0;

	if(!status_check_skilluse(src, target, skill_id, 0))
		return 0;

	// Fail if the targetted skill is near NPC [Cydh]
	if(inf2&INF2_NO_NEARNPC && skill_isNotOk_npcRange(src,skill_id,skill_lv,target->x,target->y)) {
		if (sd)
			clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0);

		return 0;
	}

	tstatus = status_get_status_data(target);

	// Record the status of the previous skill)
	if(sd) {
		switch(skill_id) {
			case SA_CASTCANCEL:
				if(ud->skill_id != skill_id) {
					sd->skill_id_old = ud->skill_id;
					sd->skill_lv_old = ud->skill_lv;
				}
				break;
			case BD_ENCORE:
				// Prevent using the dance skill if you no longer have the skill in your tree.
				if(!sd->skill_id_dance || pc_checkskill(sd,sd->skill_id_dance)<=0) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0);
					return 0;
				}

				sd->skill_id_old = skill_id;
				break;
			case WL_WHITEIMPRISON:
				if( battle_check_target(src,target,BCT_SELF|BCT_ENEMY) < 0 ) {
					clif_skill_fail(sd,skill_id,USESKILL_FAIL_TOTARGET,0);
					return 0;
				}
				break;
			case MG_FIREBOLT:
			case MG_LIGHTNINGBOLT:
			case MG_COLDBOLT:
				sd->skill_id_old = skill_id;
				sd->skill_lv_old = skill_lv;
				break;
			case CR_DEVOTION:
				if (target->type == BL_PC) {
					uint8 i = 0, count = min(skill_lv, MAX_DEVOTION);

					ARR_FIND(0, count, i, sd->devotion[i] == target_id);
					if (i == count) {
						ARR_FIND(0, count, i, sd->devotion[i] == 0);
						if (i == count) { // No free slots, skill Fail
							clif_skill_fail(sd, skill_id, USESKILL_FAIL_LEVEL, 0);
							return 0;
						}
					}
				}
				break;
			case RL_C_MARKER: {
					uint8 i = 0;

					ARR_FIND(0, MAX_SKILL_CRIMSON_MARKER, i, sd->c_marker[i] == target_id);
					if (i == MAX_SKILL_CRIMSON_MARKER) {
						ARR_FIND(0, MAX_SKILL_CRIMSON_MARKER, i, sd->c_marker[i] == 0);
						if (i == MAX_SKILL_CRIMSON_MARKER) { // No free slots, skill Fail
							clif_skill_fail(sd, skill_id, USESKILL_FAIL_LEVEL, 0);
							return 0;
						}
					}
				}
				break;
		}

		if (!skill_check_condition_castbegin(sd, skill_id, skill_lv))
			return 0;
	}

	if( src->type == BL_MOB ) {
		switch( skill_id ) {
			case NPC_SUMMONSLAVE:
			case NPC_SUMMONMONSTER:
			case NPC_DEATHSUMMON:
			case AL_TELEPORT:
				if( ((TBL_MOB*)src)->master_id && ((TBL_MOB*)src)->special_state.ai )
					return 0;
		}
	}

	if (src->type == BL_NPC) // NPC-objects can override cast distance
		range = AREA_SIZE; // Maximum visible distance before NPC goes out of sight
	else
		range = skill_get_range2(src, skill_id, skill_lv, true); // Skill cast distance from database

	// New action request received, delete previous action request if not executed yet
	if(ud->stepaction || ud->steptimer != INVALID_TIMER)
		unit_stop_stepaction(src);
	// Remember the skill request from the client while walking to the next cell
	if(walkqueue && src->type == BL_PC && ud->walktimer != INVALID_TIMER && !battle_check_range(src, target, range-1)) {
		ud->stepaction = true;
		ud->target_to = target_id;
		ud->stepskill_id = skill_id;
		ud->stepskill_lv = skill_lv;
		return 0; // Attacking will be handled by unit_walktoxy_timer in this case
	}

	// Check range when not using skill on yourself or is a combo-skill during attack
	// (these are supposed to always have the same range as your attack)
	if( src->id != target_id && (!combo || ud->attacktimer == INVALID_TIMER) ) {
		if( skill_get_state(ud->skill_id) == ST_MOVE_ENABLE ) {
			if( !unit_can_reach_bl(src, target, range + 1, 1, NULL, NULL) )
				return 0; // Walk-path check failed.
		} else if( src->type == BL_MER && skill_id == MA_REMOVETRAP ) {
			if( !battle_check_range(battle_get_master(src), target, range + 1) )
				return 0; // Aegis calc remove trap based on Master position, ignoring mercenary O.O
		} else if( !battle_check_range(src, target, range) )
			return 0; // Arrow-path check failed.
	}

	if (!combo) // Stop attack on non-combo skills [Skotlex]
		unit_stop_attack(src);
	else if(ud->attacktimer != INVALID_TIMER) // Elsewise, delay current attack sequence
		ud->attackabletime = tick + status_get_adelay(src);

	ud->state.skillcastcancel = castcancel;

	// Combo: Used to signal force cast now.
	combo = 0;

	switch(skill_id) {
		case ALL_RESURRECTION:
			if(battle_check_undead(tstatus->race,tstatus->def_ele))
				combo = 1;
			else if (!status_isdead(target))
				return 0; // Can't cast on non-dead characters.
		break;
		case MO_FINGEROFFENSIVE:
			if(sd)
				casttime += casttime * min(skill_lv, sd->spiritball);
		break;
		case MO_EXTREMITYFIST:
			if (sc && sc->data[SC_COMBO] &&
			   (sc->data[SC_COMBO]->val1 == MO_COMBOFINISH ||
				sc->data[SC_COMBO]->val1 == CH_TIGERFIST ||
				sc->data[SC_COMBO]->val1 == CH_CHAINCRUSH))
				casttime = -1;
			combo = 1;
		break;
		case SR_GATEOFHELL:
		case SR_TIGERCANNON:
			if (sc && sc->data[SC_COMBO] &&
			   sc->data[SC_COMBO]->val1 == SR_FALLENEMPIRE)
				casttime = -1;
			combo = 1;
		break;
		case SA_SPELLBREAKER:
			combo = 1;
		break;
#ifndef RENEWAL_CAST
		case ST_CHASEWALK:
			if (sc && sc->data[SC_CHASEWALK])
				casttime = -1;
		break;
#endif
		case TK_RUN:
			if (sc && sc->data[SC_RUN])
				casttime = -1;
		break;
#ifndef RENEWAL_CAST
		case KN_CHARGEATK:
		{
			unsigned int k = (distance_bl(src,target)-1)/3; //Range 0-3: 500ms, Range 4-6: 1000ms, Range 7+: 1500ms
			if(k > 2)
				k = 2;
			casttime += casttime * k;
		}
		break;
#endif
		case GD_EMERGENCYCALL: // Emergency Call double cast when the user has learned Leap [Daegaladh]
			if (sd && (pc_checkskill(sd,TK_HIGHJUMP) || pc_checkskill(sd,SU_LOPE) >= 3))
				casttime *= 2;
			break;
		case RA_WUGDASH:
			if (sc && sc->data[SC_WUGDASH])
				casttime = -1;
			break;
		case EL_WIND_SLASH:
		case EL_HURRICANE:
		case EL_TYPOON_MIS:
		case EL_STONE_HAMMER:
		case EL_ROCK_CRUSHER:
		case EL_STONE_RAIN:
		case EL_ICE_NEEDLE:
		case EL_WATER_SCREW:
		case EL_TIDAL_WEAPON:
			if( src->type == BL_ELEM ) {
				sd = BL_CAST(BL_PC, battle_get_master(src));
				if( sd && sd->skill_id_old == SO_EL_ACTION ) {
					casttime = -1;
					sd->skill_id_old = 0;
				}
			}
			break;
	}

	// Moved here to prevent Suffragium from ending if skill fails
#ifndef RENEWAL_CAST
	casttime = skill_castfix_sc(src, casttime, skill_get_castnodex(skill_id));
#else
	casttime = skill_vfcastfix(src, casttime, skill_id, skill_lv);
#endif

	if(!ud->state.running) // Need TK_RUN or WUGDASH handler to be done before that, see bugreport:6026
		unit_stop_walking(src, 1); // Even though this is not how official works but this will do the trick. bugreport:6829

	// SC_MAGICPOWER needs to switch states at start of cast
	skill_toggle_magicpower(src, skill_id);

	// In official this is triggered even if no cast time.
	clif_skillcasting(src, src->id, target_id, 0,0, skill_id, skill_get_ele(skill_id, skill_lv), casttime);

	if (sd && target->type == BL_MOB) {
		TBL_MOB *md = (TBL_MOB*)target;

		mobskill_event(md, src, tick, -1); // Cast targetted skill event.

		if ((status_has_mode(tstatus,MD_CASTSENSOR_IDLE) || status_has_mode(tstatus,MD_CASTSENSOR_CHASE)) && battle_check_target(target, src, BCT_ENEMY) > 0) {
			switch (md->state.skillstate) {
				case MSS_RUSH:
				case MSS_FOLLOW:
					if (!status_has_mode(tstatus,MD_CASTSENSOR_CHASE))
						break;

					md->target_id = src->id;
					md->state.aggressive = status_has_mode(tstatus,MD_ANGRY)?1:0;
					md->min_chase = md->db->range3;
					break;
				case MSS_IDLE:
				case MSS_WALK:
					if (!status_has_mode(tstatus,MD_CASTSENSOR_IDLE))
						break;

					md->target_id = src->id;
					md->state.aggressive = status_has_mode(tstatus,MD_ANGRY)?1:0;
					md->min_chase = md->db->range3;
					break;
			}
		}
	}

	if( casttime <= 0 )
		ud->state.skillcastcancel = 0;

	if (!sd || sd->skillitem != skill_id || skill_get_cast(skill_id, skill_lv))
		ud->canact_tick = tick + i64max(casttime, max(status_get_amotion(src), battle_config.min_skill_delay_limit)) + SECURITY_CASTTIME;

	if( sd ) {
		switch( skill_id ) {
			case CG_ARROWVULCAN:
				sd->canequip_tick = tick + casttime;
				break;
		}
	}

	ud->skilltarget  = target_id;
	ud->skillx       = 0;
	ud->skilly       = 0;
	ud->skill_id      = skill_id;
	ud->skill_lv      = skill_lv;

	if( sc ) {
		// These 3 status do not stack, so it's efficient to use if-else
 		if( sc->data[SC_CLOAKING] && !(sc->data[SC_CLOAKING]->val4&4) && skill_id != AS_CLOAKING ) {
			status_change_end(src, SC_CLOAKING, INVALID_TIMER);

			if (!src->prev)
				return 0; // Warped away!
		} else if( sc->data[SC_CLOAKINGEXCEED] && !(sc->data[SC_CLOAKINGEXCEED]->val4&4) && skill_id != GC_CLOAKINGEXCEED ) {
			status_change_end(src,SC_CLOAKINGEXCEED, INVALID_TIMER);

			if (!src->prev)
				return 0;
		}
	}


	if( casttime > 0 ) {
		ud->skilltimer = add_timer( tick+casttime, skill_castend_id, src->id, 0 );

		if( sd && (pc_checkskill(sd,SA_FREECAST) > 0 || skill_id == LG_EXEEDBREAK) )
			status_calc_bl(&sd->bl, SCB_SPEED|SCB_ASPD);
	} else
		skill_castend_id(ud->skilltimer,tick,src->id,0);

	if( sd && battle_config.prevent_logout_trigger&PLT_SKILL )
		sd->canlog_tick = gettick();

	return 1;
}

/**
 * Initiates a placement (ground/non-targeted) skill
 * @param src: Object using skill
 * @param skill_x: X coordinate where skill is being casted (center)
 * @param skill_y: Y coordinate where skill is being casted (center)
 * @param skill_id: Skill ID
 * @param skill_lv: Skill Level
 * @return unit_skilluse_pos2()
 */
int unit_skilluse_pos(struct block_list *src, short skill_x, short skill_y, uint16 skill_id, uint16 skill_lv, bool walkqueue)
{
	return unit_skilluse_pos2(
		src, skill_x, skill_y, skill_id, skill_lv,
		skill_castfix(src, skill_id, skill_lv),
		skill_get_castcancel(skill_id), walkqueue
	);
}
int unit_skilluse_pos(struct block_list *src, short skill_x, short skill_y, uint16 skill_id, uint16 skill_lv)
{
	return unit_skilluse_pos2(
		src, skill_x, skill_y, skill_id, skill_lv,
		skill_castfix(src, skill_id, skill_lv),
		skill_get_castcancel(skill_id), true
		);
}

/**
 * Performs checks for a unit using a skill and executes after cast time completion
 * @param src: Object using skill
 * @param skill_x: X coordinate where skill is being casted (center)
 * @param skill_y: Y coordinate where skill is being casted (center)
 * @param skill_id: Skill ID
 * @param skill_lv: Skill Level
 * @param casttime: Initial cast time before cast time reductions
 * @param castcancel: Whether or not the skill can be cancelled by interuption (hit)
 * @return Success(1); Fail(0);
 */
int unit_skilluse_pos2(struct block_list *src, short skill_x, short skill_y, uint16 skill_id, uint16 skill_lv, int casttime, int castcancel)
{
	return unit_skilluse_pos2(src,skill_x, skill_y, skill_id, skill_lv, casttime, castcancel, true);
}
	int unit_skilluse_pos2(struct block_list *src, short skill_x, short skill_y, uint16 skill_id, uint16 skill_lv, int casttime, int castcancel, bool walkqueue)
{
	struct map_session_data *sd = NULL;
	struct unit_data        *ud = NULL;
	struct status_change *sc;
	struct block_list    bl;
	t_tick tick = gettick();
	int range;

	nullpo_ret(src);

	if (!src->prev)
		return 0; // Not on the map

	if(status_isdead(src))
		return 0;

	sd = BL_CAST(BL_PC, src);
	ud = unit_bl2ud(src);

	if(ud == NULL)
		return 0;

	if (ud && ud->state.blockedskill)
		return 0;

	if(ud->skilltimer != INVALID_TIMER) // Normally not needed since clif.cpp checks for it, but at/char/script commands don't! [Skotlex]
		return 0;

	sc = status_get_sc(src);

	if (sc && !sc->count)
		sc = NULL;

	if( sd ) {
		if( skill_isNotOk(skill_id, sd) || !skill_check_condition_castbegin(sd, skill_id, skill_lv) )
			return 0;
		if (skill_id == MG_FIREWALL && !skill_pos_maxcount_check(src, skill_x, skill_y, skill_id, skill_lv, BL_PC, true))
			return 0; // Special check for Firewall only
	}

	if( (skill_id >= SC_MANHOLE && skill_id <= SC_FEINTBOMB) && map_getcell(src->m, skill_x, skill_y, CELL_CHKMAELSTROM) ) {
		clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0);
		return 0;
	}

	if (!status_check_skilluse(src, NULL, skill_id, 0))
		return 0;

	// Fail if the targetted skill is near NPC [Cydh]
	if(skill_get_inf2(skill_id)&INF2_NO_NEARNPC && skill_isNotOk_npcRange(src,skill_id,skill_lv,skill_x,skill_y)) {
		if (sd)
			clif_skill_fail(sd,skill_id,USESKILL_FAIL_LEVEL,0);

		return 0;
	}

	// Check range and obstacle
	bl.type = BL_NUL;
	bl.m = src->m;
	bl.x = skill_x;
	bl.y = skill_y;

	if (src->type == BL_NPC) // NPC-objects can override cast distance
		range = AREA_SIZE; // Maximum visible distance before NPC goes out of sight
	else
		range = skill_get_range2(src, skill_id, skill_lv, true); // Skill cast distance from database

	// New action request received, delete previous action request if not executed yet
	if(ud->stepaction || ud->steptimer != INVALID_TIMER)
		unit_stop_stepaction(src);
	// Remember the skill request from the client while walking to the next cell
	if(walkqueue && src->type == BL_PC && ud->walktimer != INVALID_TIMER && !battle_check_range(src, &bl, range-1)) {
		struct map_data *md = &map[src->m];
		// Convert coordinates to target_to so we can use it as target later
		ud->stepaction = true;
		ud->target_to = (skill_x + skill_y*md->xs);
		ud->stepskill_id = skill_id;
		ud->stepskill_lv = skill_lv;
		return 0; // Attacking will be handled by unit_walktoxy_timer in this case
	}

	if( skill_get_state(ud->skill_id) == ST_MOVE_ENABLE ) {
		if( !unit_can_reach_bl(src, &bl, range + 1, 1, NULL, NULL) )
			return 0; // Walk-path check failed.
	}else if( !battle_check_range(src, &bl, range) )
		return 0; // Arrow-path check failed.

	unit_stop_attack(src);

	// Moved here to prevent Suffragium from ending if skill fails
#ifndef RENEWAL_CAST
	casttime = skill_castfix_sc(src, casttime, skill_get_castnodex(skill_id));
#else
	casttime = skill_vfcastfix(src, casttime, skill_id, skill_lv );
#endif

	ud->state.skillcastcancel = castcancel&&casttime>0?1:0;
	if (!sd || sd->skillitem != skill_id || skill_get_cast(skill_id, skill_lv))
		ud->canact_tick = tick + i64max(casttime, max(status_get_amotion(src), battle_config.min_skill_delay_limit)) + SECURITY_CASTTIME;

// 	if( sd )
// 	{
// 		switch( skill_id )
// 		{
// 		case ????:
// 			sd->canequip_tick = tick + casttime;
// 		}
// 	}

	ud->skill_id    = skill_id;
	ud->skill_lv    = skill_lv;
	ud->skillx      = skill_x;
	ud->skilly      = skill_y;
	ud->skilltarget = 0;

	if( sc ) {
		// These 3 status do not stack, so it's efficient to use if-else
		if (sc->data[SC_CLOAKING] && !(sc->data[SC_CLOAKING]->val4&4)) {
			status_change_end(src, SC_CLOAKING, INVALID_TIMER);

			if (!src->prev)
				return 0; // Warped away!
		} else if (sc->data[SC_CLOAKINGEXCEED] && !(sc->data[SC_CLOAKINGEXCEED]->val4&4)) {
			status_change_end(src, SC_CLOAKINGEXCEED, INVALID_TIMER);

			if (!src->prev)
				return 0;
		}
	}

	unit_stop_walking(src,1);

	// SC_MAGICPOWER needs to switch states at start of cast
	skill_toggle_magicpower(src, skill_id);

	// In official this is triggered even if no cast time.
	clif_skillcasting(src, src->id, 0, skill_x, skill_y, skill_id, skill_get_ele(skill_id, skill_lv), casttime);

	if( casttime > 0 ) {
		ud->skilltimer = add_timer( tick+casttime, skill_castend_pos, src->id, 0 );

		if( (sd && pc_checkskill(sd,SA_FREECAST) > 0) || skill_id == LG_EXEEDBREAK)
			status_calc_bl(&sd->bl, SCB_SPEED|SCB_ASPD);
	} else {
		ud->skilltimer = INVALID_TIMER;
		skill_castend_pos(ud->skilltimer,tick,src->id,0);
	}

	if( sd && battle_config.prevent_logout_trigger&PLT_SKILL )
		sd->canlog_tick = gettick();

	return 1;
}

/**
 * Update a unit's attack target
 * @param ud: Unit data
 * @param target_id: Target ID (bl->id)
 * @return 0
 */
int unit_set_target(struct unit_data* ud, int target_id)
{
	nullpo_ret(ud);

	if( ud->target != target_id ) {
		struct unit_data * ux;
		struct block_list* target;
	
		if (ud->target && (target = map_id2bl(ud->target)) && (ux = unit_bl2ud(target)) && ux->target_count > 0)
			ux->target_count--;

		if (target_id && (target = map_id2bl(target_id)) && (ux = unit_bl2ud(target)) && ux->target_count < 255)
			ux->target_count++;
	}

	ud->target = target_id;

	return 0;
}

/**
 * Helper function used in foreach calls to stop auto-attack timers
 * @param bl: Block object
 * @param ap: func* with va_list values
 *   Parameter: '0' - everyone, 'id' - only those attacking someone with that id
 * @return 1 on success or 0 otherwise
 */
int unit_stopattack(struct block_list *bl, va_list ap)
{
	struct unit_data *ud = unit_bl2ud(bl);
	int id = va_arg(ap, int);

	if (ud && ud->attacktimer != INVALID_TIMER && (!id || id == ud->target)) {
		unit_stop_attack(bl);
		return 1;
	}

	return 0;
}

/**
 * Stop a unit's attacks
 * @param bl: Object to stop
 */
void unit_stop_attack(struct block_list *bl)
{
	struct unit_data *ud;
	nullpo_retv(bl);
	ud = unit_bl2ud(bl);
	nullpo_retv(ud);

	//Clear target
	unit_set_target(ud, 0);

	if(ud->attacktimer == INVALID_TIMER)
		return;

	//Clear timer
	delete_timer(ud->attacktimer, unit_attack_timer);
	ud->attacktimer = INVALID_TIMER;
}

/**
 * Stop a unit's step action
 * @param bl: Object to stop
 */
void unit_stop_stepaction(struct block_list *bl)
{
	struct unit_data *ud;
	nullpo_retv(bl);
	ud = unit_bl2ud(bl);
	nullpo_retv(ud);

	//Clear remembered step action
	ud->stepaction = false;
	ud->target_to = 0;
	ud->stepskill_id = 0;
	ud->stepskill_lv = 0;

	if(ud->steptimer == INVALID_TIMER)
		return;

	//Clear timer
	delete_timer(ud->steptimer, unit_step_timer);
	ud->steptimer = INVALID_TIMER;
}

/**
 * Removes a unit's target due to being unattackable
 * @param bl: Object to unlock target
 * @return 0
 */
int unit_unattackable(struct block_list *bl)
{
	struct unit_data *ud = unit_bl2ud(bl);

	if (ud) {
		ud->state.attack_continue = 0;
		ud->state.step_attack = 0;
		ud->target_to = 0;
		unit_set_target(ud, 0);
	}

	if(bl->type == BL_MOB)
		mob_unlocktarget((struct mob_data*)bl, gettick());
	else if(bl->type == BL_PET)
		pet_unlocktarget((struct pet_data*)bl);

	return 0;
}

/**
 * Checks if the unit can attack, returns yes if so.
*/
bool unit_can_attack(struct block_list *src, int target_id)
{
	struct status_change *sc = status_get_sc(src);

	if( sc != NULL ) {
		if( sc->data[SC__MANHOLE] )
			return false;
	}

	if( src->type == BL_PC )
		return pc_can_attack(BL_CAST(BL_PC, src), target_id);
	return true;
}

/**
 * Requests a unit to attack a target
 * @param src: Object initiating attack
 * @param target_id: Target ID (bl->id)
 * @param continuous: 
 *		0x1 - Whether or not the attack is ongoing
 *		0x2 - Whether function was called from unit_step_timer or not
 * @return Success(0); Fail(1);
 */
int unit_attack(struct block_list *src,int target_id,int continuous)
{
	struct block_list *target;
	struct unit_data  *ud;
	int range;

	nullpo_ret(ud = unit_bl2ud(src));

	target = map_id2bl(target_id);
	if( target == NULL || status_isdead(target) ) {
		unit_unattackable(src);
		return 1;
	}

	if( src->type == BL_PC &&
		target->type == BL_NPC ) {
		// Monster npcs [Valaris]
		npc_click((TBL_PC*)src,(TBL_NPC*)target);
		return 0;
	}

	if( !unit_can_attack(src, target_id) ) {
		unit_stop_attack(src);
		return 0;
	}

	if( battle_check_target(src,target,BCT_ENEMY) <= 0 || !status_check_skilluse(src, target, 0, 0) ) {
		unit_unattackable(src);
		return 1;
	}

	ud->state.attack_continue = (continuous&1)?1:0;
	ud->state.step_attack = (continuous&2)?1:0;
	unit_set_target(ud, target_id);

	range = status_get_range(src);

	if (continuous) // If you're to attack continously, set to auto-chase character
		ud->chaserange = range;

	// Just change target/type. [Skotlex]
	if(ud->attacktimer != INVALID_TIMER)
		return 0;

	// New action request received, delete previous action request if not executed yet
	if(ud->stepaction || ud->steptimer != INVALID_TIMER)
		unit_stop_stepaction(src);
	// Remember the attack request from the client while walking to the next cell
	if(src->type == BL_PC && ud->walktimer != INVALID_TIMER && !battle_check_range(src, target, range-1)) {
		ud->stepaction = true;
		ud->target_to = ud->target;
		ud->stepskill_id = 0;
		ud->stepskill_lv = 0;
		return 0; // Attacking will be handled by unit_walktoxy_timer in this case
	}
	
	if(DIFF_TICK(ud->attackabletime, gettick()) > 0) // nDo attack next time it is possible. [Skotlex]
		ud->attacktimer=add_timer(ud->attackabletime,unit_attack_timer,src->id,0);
	else // Attack NOW.
		unit_attack_timer(INVALID_TIMER, gettick(), src->id, 0);

	return 0;
}

/** 
 * Cancels an ongoing combo, resets attackable time, and restarts the
 * attack timer to resume attack after amotion time
 * @author [Skotlex]
 * @param bl: Object to cancel combo
 * @return Success(1); Fail(0);
 */
int unit_cancel_combo(struct block_list *bl)
{
	struct unit_data  *ud;

	if (!status_change_end(bl, SC_COMBO, INVALID_TIMER))
		return 0; // Combo wasn't active.

	ud = unit_bl2ud(bl);
	nullpo_ret(ud);

	ud->attackabletime = gettick() + status_get_amotion(bl);

	if (ud->attacktimer == INVALID_TIMER)
		return 1; // Nothing more to do.

	delete_timer(ud->attacktimer, unit_attack_timer);
	ud->attacktimer=add_timer(ud->attackabletime,unit_attack_timer,bl->id,0);

	return 1;
}

/**
 * Does a path_search to check if a position can be reached
 * @param bl: Object to check path
 * @param x: X coordinate that will be path searched
 * @param y: Y coordinate that will be path searched
 * @param easy: Easy(1) or Hard(0) path check (hard attempts to go around obstacles)
 * @return true or false
 */
bool unit_can_reach_pos(struct block_list *bl,int x,int y, int easy)
{
	nullpo_retr(false, bl);

	if (bl->x == x && bl->y == y) // Same place
		return true;

	return path_search(NULL,bl->m,bl->x,bl->y,x,y,easy,CELL_CHKNOREACH);
}

/**
 * Does a path_search to check if a unit can be reached
 * @param bl: Object to check path
 * @param tbl: Target to be checked for available path
 * @param range: The number of cells away from bl that the path should be checked
 * @param easy: Easy(1) or Hard(0) path check (hard attempts to go around obstacles)
 * @param x: Pointer storing a valid X coordinate around tbl that can be reached
 * @param y: Pointer storing a valid Y coordinate around tbl that can be reached
 * @return true or false
 */
bool unit_can_reach_bl(struct block_list *bl,struct block_list *tbl, int range, int easy, short *x, short *y)
{
	struct walkpath_data wpd;
	short dx, dy;

	nullpo_retr(false, bl);
	nullpo_retr(false, tbl);

	if( bl->m != tbl->m)
		return false;

	if( bl->x == tbl->x && bl->y == tbl->y )
		return true;

	if(range > 0 && !check_distance_bl(bl, tbl, range))
		return false;

	// It judges whether it can adjoin or not.
	dx = tbl->x - bl->x;
	dy = tbl->y - bl->y;
	dx = (dx > 0) ? 1 : ((dx < 0) ? -1 : 0);
	dy = (dy > 0) ? 1 : ((dy < 0) ? -1 : 0);

	if (map_getcell(tbl->m,tbl->x-dx,tbl->y-dy,CELL_CHKNOPASS)) { // Look for a suitable cell to place in.
		int i;

		for(i = 0; i < 8 && map_getcell(tbl->m,tbl->x-dirx[i],tbl->y-diry[i],CELL_CHKNOPASS); i++);

		if (i == 8)
			return false; // No valid cells.

		dx = dirx[i];
		dy = diry[i];
	}

	if (x)
		*x = tbl->x-dx;

	if (y)
		*y = tbl->y-dy;

	if (!path_search(&wpd,bl->m,bl->x,bl->y,tbl->x-dx,tbl->y-dy,easy,CELL_CHKNOREACH))
		return false;

	if (!(path_search_long(NULL, bl->m, bl->x, bl->y, tbl->x - dx, tbl->y - dy, CELL_CHKNOPASS, MAX_WALKPATH)) // Check if there is an obstacle between
		&& (wpd.path_len > CLIENT_COMPLEX_PATH_LIMIT)	// Official number of walkable cells is 14 if and only if there is an obstacle between. [malufett]
		&& (bl->type != BL_NPC)) // If type is a NPC, please disregard.
	{
		//if (bl->type == BL_MOB) return false;
#ifdef OFFICIAL_WALKPATH
		return false;
#endif
	}

	return true;
}

/**
 * Calculates position of Pet/Mercenary/Homunculus/Elemental
 * @param bl: Object to calculate position
 * @param tx: X coordinate to go to
 * @param ty: Y coordinate to go to
 * @param dir: Direction which to be 2 cells from master's position
 * @return Success(0); Fail(1);
 */
int unit_calc_pos(struct block_list *bl, int tx, int ty, uint8 dir)
{
	int dx, dy, x, y;
	struct unit_data *ud = unit_bl2ud(bl);

	nullpo_ret(ud);

	if(dir > 7)
		return 1;

	ud->to_x = tx;
	ud->to_y = ty;

	// 2 cells from Master Position
	dx = -dirx[dir] * 2;
	dy = -diry[dir] * 2;
	x = tx + dx;
	y = ty + dy;

	if( !unit_can_reach_pos(bl, x, y, 0) ) {
		if( dx > 0 )
			x--;
		else if( dx < 0 )
			x++;

		if( dy > 0 )
			y--;
		else if( dy < 0 )
			y++;

		if( !unit_can_reach_pos(bl, x, y, 0) ) {
			int i;

			for( i = 0; i < 12; i++ ) {
				int k = rnd()%8; // Pick a Random Dir

				dx = -dirx[k] * 2;
				dy = -diry[k] * 2;
				x = tx + dx;
				y = ty + dy;

				if( unit_can_reach_pos(bl, x, y, 0) )
					break;
				else {
					if( dx > 0 )
						x--;
					else if( dx < 0 )
						x++;

					if( dy > 0 )
						y--;
					else if( dy < 0 )
						y++;

					if( unit_can_reach_pos(bl, x, y, 0) )
						break;
				}
			}

			if( i == 12 ) {
				x = tx; y = tx; // Exactly Master Position

				if( !unit_can_reach_pos(bl, x, y, 0) )
					return 1;
			}
		}
	}

	ud->to_x = x;
	ud->to_y = y;

	return 0;
}

/**
 * Function timer to continuously attack
 * @param src: Object to continuously attack
 * @param tid: Timer ID
 * @param tick: Current tick
 * @return Attackable(1); Unattackable(0);
 */
static int unit_attack_timer_sub(struct block_list* src, int tid, t_tick tick)
{
	struct block_list *target;
	struct unit_data *ud;
	struct status_data *sstatus;
	struct map_session_data *sd = NULL;
	struct mob_data *md = NULL;
	int range;

	if( (ud = unit_bl2ud(src)) == NULL )
		return 0;

	if( ud->attacktimer != tid ) {
		ShowError("unit_attack_timer %d != %d\n",ud->attacktimer,tid);
		return 0;
	}

	sd = BL_CAST(BL_PC, src);
	md = BL_CAST(BL_MOB, src);
	ud->attacktimer = INVALID_TIMER;
	target = map_id2bl(ud->target);

	if( src == NULL || src->prev == NULL || target==NULL || target->prev == NULL )
		return 0;

	if( status_isdead(src) || status_isdead(target) ||
			battle_check_target(src,target,BCT_ENEMY) <= 0 || !status_check_skilluse(src, target, 0, 0)
#ifdef OFFICIAL_WALKPATH
	   || !path_search_long(NULL, src->m, src->x, src->y, target->x, target->y, CELL_CHKWALL)
#endif
	   || ((src->type==BL_MOB) && !path_search_long(NULL, src->m, src->x, src->y, target->x, target->y, CELL_CHKWALL))
	   || (sd && !pc_can_attack(sd, target->id)) )
		return 0; // Can't attack under these conditions

	if( src->m != target->m ) {
		if( src->type == BL_MOB && mob_warpchase((TBL_MOB*)src, target) )
			return 1; // Follow up.

		return 0;
	}

	if( ud->skilltimer != INVALID_TIMER && !(sd && pc_checkskill(sd,SA_FREECAST) > 0) )
		return 0; // Can't attack while casting

	if( !battle_config.sdelay_attack_enable && DIFF_TICK(ud->canact_tick,tick) > 0 && !(sd && pc_checkskill(sd,SA_FREECAST) > 0) ) {
		// Attacking when under cast delay has restrictions:
		if( tid == INVALID_TIMER ) { // Requested attack.
			if(sd)
				clif_skill_fail(sd,1,USESKILL_FAIL_SKILLINTERVAL,0);

			return 0;
		}

		// Otherwise, we are in a combo-attack, delay this until your canact time is over. [Skotlex]
		if( ud->state.attack_continue ) {
			if( DIFF_TICK(ud->canact_tick, ud->attackabletime) > 0 )
				ud->attackabletime = ud->canact_tick;

			ud->attacktimer=add_timer(ud->attackabletime,unit_attack_timer,src->id,0);
		}

		return 1;
	}

	sstatus = status_get_status_data(src);
	range = sstatus->rhw.range;

	if( (unit_is_walking(target) || ud->state.step_attack)
		&& (target->type == BL_PC || !map_getcell(target->m,target->x,target->y,CELL_CHKICEWALL)) )
		range++; // Extra range when chasing (does not apply to mobs locked in an icewall)

	if(sd && !check_distance_client_bl(src,target,range)) {
		// Player tries to attack but target is too far, notify client
		clif_movetoattack(sd,target);
		return 1;
	} else if(md && !check_distance_bl(src,target,range)) {
		// Monster: Chase if required
		unit_walktobl(src,target,ud->chaserange,ud->state.walk_easy|2);
		return 1;
	}

	if( !battle_check_range(src,target,range) ) {
	  	// Within range, but no direct line of attack
		if( ud->state.attack_continue ) {
			if(ud->chaserange > 2)
				ud->chaserange-=2;

			unit_walktobl(src,target,ud->chaserange,ud->state.walk_easy|2);
		}

		return 1;
	}

	// Sync packet only for players.
	// Non-players use the sync packet on the walk timer. [Skotlex]
	if (tid == INVALID_TIMER && sd)
		clif_fixpos(src);

	if( DIFF_TICK(ud->attackabletime,tick) <= 0 ) {
		if (battle_config.attack_direction_change && (src->type&battle_config.attack_direction_change))
			ud->dir = map_calc_dir(src, target->x, target->y);

		if(ud->walktimer != INVALID_TIMER)
			unit_stop_walking(src,1);

		if(md) {
			//First attack is always a normal attack
			if(md->state.skillstate == MSS_ANGRY || md->state.skillstate == MSS_BERSERK) {
				if (mobskill_use(md,tick,-1))
					return 1;
			}
			// Set mob's ANGRY/BERSERK states.
			md->state.skillstate = md->state.aggressive?MSS_ANGRY:MSS_BERSERK;

			if (status_has_mode(sstatus,MD_ASSIST) && DIFF_TICK(md->last_linktime, tick) < MIN_MOBLINKTIME) { 
				// Link monsters nearby [Skotlex]
				md->last_linktime = tick;
				map_foreachinrange(mob_linksearch, src, md->db->range2, BL_MOB, md->mob_id, target, tick);
			}
		}

		if(src->type == BL_PET && pet_attackskill((TBL_PET*)src, target->id))
			return 1;

		map_freeblock_lock();
		ud->attacktarget_lv = battle_weapon_attack(src,target,tick,0);

		if(sd && sd->status.pet_id > 0 && sd->pd && battle_config.pet_attack_support)
			pet_target_check(sd->pd,target,0);

		map_freeblock_unlock();

		/**
		 * Applied when you're unable to attack (e.g. out of ammo)
		 * We should stop here otherwise timer keeps on and this happens endlessly
		 */
		if( ud->attacktarget_lv == ATK_NONE )
			return 1;

		ud->attackabletime = tick + sstatus->adelay;

		// You can't move if you can't attack neither.
		if (src->type&battle_config.attack_walk_delay)
			unit_set_walkdelay(src, tick, sstatus->amotion, 1);
	}

	if(ud->state.attack_continue) {
		if (src->type == BL_PC && battle_config.idletime_option&IDLE_ATTACK)
			((TBL_PC*)src)->idletime = last_tick;

		ud->attacktimer = add_timer(ud->attackabletime,unit_attack_timer,src->id,0);
	}

	if( sd && battle_config.prevent_logout_trigger&PLT_ATTACK )
		sd->canlog_tick = gettick();

	return 1;
}

/**
 * Timer function to cancel attacking if unit has become unattackable
 * @param tid: Timer ID
 * @param tick: Current tick
 * @param id: Object to cancel attack if applicable
 * @param data: Data passed from timer call
 * @return 0
 */
static TIMER_FUNC(unit_attack_timer){
	struct block_list *bl;

	bl = map_id2bl(id);

	if(bl && unit_attack_timer_sub(bl, tid, tick) == 0)
		unit_unattackable(bl);

	return 0;
}

/**
 * Cancels a skill's cast
 * @param bl: Object to cancel cast
 * @param type: Cancel check flag
 *	&1: Cast-Cancel invoked
 *	&2: Cancel only if skill is cancellable
 * @return Success(1); Fail(0);
 */
int unit_skillcastcancel(struct block_list *bl, char type)
{
	struct map_session_data *sd = NULL;
	struct unit_data *ud = unit_bl2ud( bl);
	t_tick tick = gettick();
	int ret = 0, skill_id;

	nullpo_ret(bl);

	if (!ud || ud->skilltimer == INVALID_TIMER)
		return 0; // Nothing to cancel.

	sd = BL_CAST(BL_PC, bl);

	if (type&2) { // See if it can be cancelled.
		if (!ud->state.skillcastcancel)
			return 0;

		if (sd && (sd->special_state.no_castcancel2 ||
			((sd->sc.data[SC_UNLIMITEDHUMMINGVOICE] || sd->special_state.no_castcancel) && !map_flag_gvg2(bl->m) && !map_getmapflag(bl->m, MF_BATTLEGROUND)))) // fixed flags being read the wrong way around [blackhole89]
			return 0;
	}

	ud->canact_tick = tick;

	if(type&1 && sd)
		skill_id = sd->skill_id_old;
	else
		skill_id = ud->skill_id;

	if (skill_get_inf(skill_id) & INF_GROUND_SKILL)
		ret=delete_timer( ud->skilltimer, skill_castend_pos );
	else
		ret=delete_timer( ud->skilltimer, skill_castend_id );

	if(ret < 0)
		ShowError("delete timer error : skill_id : %d\n",ret);

	ud->skilltimer = INVALID_TIMER;

	if( sd && (pc_checkskill(sd,SA_FREECAST) > 0 || skill_id == LG_EXEEDBREAK) )
		status_calc_bl(&sd->bl, SCB_SPEED|SCB_ASPD);

	if( sd ) {
		switch( skill_id ) {
			case CG_ARROWVULCAN:
				sd->canequip_tick = tick;
				break;
		}
	}

	if(bl->type==BL_MOB)
		((TBL_MOB*)bl)->skill_idx = -1;

	clif_skillcastcancel(bl);

	return 1;
}

/**
 * Initialized data on a unit
 * @param bl: Object to initialize data on
 */
void unit_dataset(struct block_list *bl)
{
	struct unit_data *ud;

	nullpo_retv(ud = unit_bl2ud(bl));

	memset( ud, 0, sizeof( struct unit_data) );
	ud->bl             = bl;
	ud->walktimer      = INVALID_TIMER;
	ud->skilltimer     = INVALID_TIMER;
	ud->attacktimer    = INVALID_TIMER;
	ud->steptimer      = INVALID_TIMER;
	ud->attackabletime =
	ud->canact_tick    =
	ud->canmove_tick   = gettick();
}

/**
 * Gets the number of units attacking another unit
 * @param bl: Object to check amount of targets
 * @return number of targets or 0
 */
int unit_counttargeted(struct block_list* bl)
{
	struct unit_data* ud;

	if( bl && (ud = unit_bl2ud(bl)) )
		return ud->target_count;

	return 0;
}

/**
 * Makes 'bl' that attacking 'src' switch to attack 'target'
 * @param bl
 * @param ap
 * @param src Current target
 * @param target New target
 **/
int unit_changetarget(struct block_list *bl, va_list ap) {
	struct unit_data *ud = unit_bl2ud(bl);
	struct block_list *src = va_arg(ap,struct block_list *);
	struct block_list *target = va_arg(ap,struct block_list *);

	if (!ud || !target || ud->target == target->id)
		return 1;
	if (!ud->target && !ud->target_to)
		return 1;
	if (ud->target != src->id && ud->target_to != src->id)
		return 1;

	if (bl->type == BL_MOB)
		(BL_CAST(BL_MOB,bl))->target_id = target->id;
	if (ud->target_to)
		ud->target_to = target->id;
	else
		ud->target_to = 0;
	if (ud->skilltarget)
		ud->skilltarget = target->id;
	unit_set_target(ud, target->id);

	//unit_attack(bl, target->id, ud->state.attack_continue);
	return 0;
}

/**
 * Removes a bl/ud from the map
 * On kill specifics are not performed here, check status_damage()
 * @param bl: Object to remove from map
 * @param clrtype: How bl is being removed
 *	0: Assume bl is being warped
 *	1: Death, appropriate cleanup performed
 * @param file, line, func: Call information for debug purposes
 * @return Success(1); Couldn't be removed or bl was free'd(0)
 */
int unit_remove_map_(struct block_list *bl, clr_type clrtype, const char* file, int line, const char* func)
{
	struct unit_data *ud = unit_bl2ud(bl);
	struct status_change *sc = status_get_sc(bl);

	nullpo_ret(ud);

	if(bl->prev == NULL)
		return 0; // Already removed?

	map_freeblock_lock();

	if (ud->walktimer != INVALID_TIMER)
		unit_stop_walking(bl,0);

	if (ud->skilltimer != INVALID_TIMER)
		unit_skillcastcancel(bl,0);

	//Clear target even if there is no timer
	if (ud->target || ud->attacktimer != INVALID_TIMER)
		unit_stop_attack(bl);

	//Clear stepaction even if there is no timer
	if (ud->stepaction || ud->steptimer != INVALID_TIMER)
		unit_stop_stepaction(bl);

	// Do not reset can-act delay. [Skotlex]
	ud->attackabletime = ud->canmove_tick /*= ud->canact_tick*/ = gettick();

	if(sc && sc->count ) { // map-change/warp dispells.
		status_change_end(bl, SC_BLADESTOP, INVALID_TIMER);
		status_change_end(bl, SC_BASILICA, INVALID_TIMER);
		status_change_end(bl, SC_ANKLE, INVALID_TIMER);
		status_change_end(bl, SC_TRICKDEAD, INVALID_TIMER);
		status_change_end(bl, SC_BLADESTOP_WAIT, INVALID_TIMER);
		status_change_end(bl, SC_RUN, INVALID_TIMER);
		status_change_end(bl, SC_DANCING, INVALID_TIMER);
		status_change_end(bl, SC_WARM, INVALID_TIMER);
		status_change_end(bl, SC_DEVOTION, INVALID_TIMER);
		status_change_end(bl, SC_MARIONETTE, INVALID_TIMER);
		status_change_end(bl, SC_MARIONETTE2, INVALID_TIMER);
		status_change_end(bl, SC_CLOSECONFINE, INVALID_TIMER);
		status_change_end(bl, SC_CLOSECONFINE2, INVALID_TIMER);
		status_change_end(bl, SC_TINDER_BREAKER, INVALID_TIMER);
		status_change_end(bl, SC_TINDER_BREAKER2, INVALID_TIMER);
		status_change_end(bl, SC_HIDING, INVALID_TIMER);
		// Ensure the bl is a PC; if so, we'll handle the removal of cloaking and cloaking exceed later
		if ( bl->type != BL_PC ) {
			status_change_end(bl, SC_CLOAKING, INVALID_TIMER);
			status_change_end(bl, SC_CLOAKINGEXCEED, INVALID_TIMER);
		}
		status_change_end(bl, SC_CHASEWALK, INVALID_TIMER);
		if (sc->data[SC_GOSPEL] && sc->data[SC_GOSPEL]->val4 == BCT_SELF)
			status_change_end(bl, SC_GOSPEL, INVALID_TIMER);
		if (sc->data[SC_PROVOKE] && sc->data[SC_PROVOKE]->timer == INVALID_TIMER)
			status_change_end(bl, SC_PROVOKE, INVALID_TIMER); //End infinite provoke to prevent exploit
		status_change_end(bl, SC_CHANGE, INVALID_TIMER);
		status_change_end(bl, SC_STOP, INVALID_TIMER);
		status_change_end(bl, SC_WUGDASH, INVALID_TIMER);
		status_change_end(bl, SC_CAMOUFLAGE, INVALID_TIMER);
		status_change_end(bl, SC_NEUTRALBARRIER_MASTER, INVALID_TIMER);
		status_change_end(bl, SC_STEALTHFIELD_MASTER, INVALID_TIMER);
		status_change_end(bl, SC__SHADOWFORM, INVALID_TIMER);
		status_change_end(bl, SC__MANHOLE, INVALID_TIMER);
		status_change_end(bl, SC_VACUUM_EXTREME, INVALID_TIMER);
		status_change_end(bl, SC_CURSEDCIRCLE_ATKER, INVALID_TIMER); // callme before warp
		status_change_end(bl, SC_SUHIDE, INVALID_TIMER);
	}

	switch( bl->type ) {
		case BL_PC: {
			struct map_session_data *sd = (struct map_session_data*)bl;

			if(sd->shadowform_id) { // If shadow target has leave the map
			    struct block_list *d_bl = map_id2bl(sd->shadowform_id);

			    if( d_bl )
				    status_change_end(d_bl,SC__SHADOWFORM,INVALID_TIMER);
			}

			// Leave/reject all invitations.
			if(sd->chatID)
				chat_leavechat(sd,0);

			if(sd->trade_partner)
				trade_tradecancel(sd);

			searchstore_close(sd);

			if (sd->menuskill_id != AL_TELEPORT) { //bugreport:8027
				if (sd->state.storage_flag == 1)
					storage_storage_quit(sd,0);
				else if (sd->state.storage_flag == 2)
					storage_guild_storage_quit(sd, 0);
				else if (sd->state.storage_flag == 3)
					storage_premiumStorage_quit(sd);

				sd->state.storage_flag = 0; //Force close it when being warped.
			}

			if(sd->party_invite > 0)
				party_reply_invite(sd,sd->party_invite,0);

			if(sd->guild_invite > 0)
				guild_reply_invite(sd,sd->guild_invite,0);

			if(sd->guild_alliance > 0)
				guild_reply_reqalliance(sd,sd->guild_alliance_account,0);

			if(sd->menuskill_id)
				sd->menuskill_id = sd->menuskill_val = 0;

			if( !sd->npc_ontouch_.empty() )
				npc_touchnext_areanpc(sd,true);

			// Check if warping and not changing the map.
			if ( sd->state.warping && !sd->state.changemap ) {
				status_change_end(bl, SC_CLOAKING, INVALID_TIMER);
				status_change_end(bl, SC_CLOAKINGEXCEED, INVALID_TIMER);
			}

			sd->npc_shopid = 0;
			sd->adopt_invite = 0;

			if(sd->pvp_timer != INVALID_TIMER) {
				delete_timer(sd->pvp_timer,pc_calc_pvprank_timer);
				sd->pvp_timer = INVALID_TIMER;
				sd->pvp_rank = 0;
			}

			if(sd->duel_group > 0)
				duel_leave(sd->duel_group, sd);

			if(pc_issit(sd) && pc_setstand(sd, false))
				skill_sit(sd, false);

			party_send_dot_remove(sd);// minimap dot fix [Kevin]
			guild_send_dot_remove(sd);
			bg_send_dot_remove(sd);

			if( map[bl->m].users <= 0 || sd->state.debug_remove_map ) {
				// This is only place where map users is decreased, if the mobs were removed
				// too soon then this function was executed too many times [FlavioJS]
				if( sd->debug_file == NULL || !(sd->state.debug_remove_map) ) {
					sd->debug_file = "";
					sd->debug_line = 0;
					sd->debug_func = "";
				}

				ShowDebug("unit_remove_map: unexpected state when removing player AID/CID:%d/%d"
					" (active=%d connect_new=%d rewarp=%d changemap=%d debug_remove_map=%d)"
					" from map=%s (users=%d)."
					" Previous call from %s:%d(%s), current call from %s:%d(%s)."
					" Please report this!!!\n",
					sd->status.account_id, sd->status.char_id,
					sd->state.active, sd->state.connect_new, sd->state.rewarp, sd->state.changemap, sd->state.debug_remove_map,
					map[bl->m].name, map[bl->m].users,
					sd->debug_file, sd->debug_line, sd->debug_func, file, line, func);
			}
			else if (--map[bl->m].users == 0 && battle_config.dynamic_mobs)
				map_removemobs(bl->m);

			if( !pc_isinvisible(sd) ) // Decrement the number of active pvp players on the map
				--map[bl->m].users_pvp;

			if( sd->state.hpmeter_visible ) {
				map[bl->m].hpmeter_visible--;
				sd->state.hpmeter_visible = 0;
			}

			sd->state.debug_remove_map = 1; // Temporary state to track double remove_map's [FlavioJS]
			sd->debug_file = file;
			sd->debug_line = line;
			sd->debug_func = func;
			break;
		}
		case BL_MOB: {
			struct mob_data *md = (struct mob_data*)bl;

			// Drop previous target mob_slave_keep_target: no.
			if (!battle_config.mob_slave_keep_target)
				md->target_id=0;

			md->attacked_id=0;
			md->state.skillstate= MSS_IDLE;
			break;
		}
		case BL_PET: {
			struct pet_data *pd = (struct pet_data*)bl;

			if( pd->pet.intimate <= PET_INTIMATE_NONE && !(pd->master && !pd->master->state.active) ) {
				// If logging out, this is deleted on unit_free
				clif_clearunit_area(bl,clrtype);
				map_delblock(bl);
				unit_free(bl,CLR_OUTSIGHT);
				map_freeblock_unlock();

				return 0;
			}
			break;
		}
		case BL_HOM: {
			struct homun_data *hd = (struct homun_data *)bl;

			ud->canact_tick = ud->canmove_tick; // It appears HOM do reset the can-act tick.

			if( !hd->homunculus.intimacy && !(hd->master && !hd->master->state.active) ) {
				// If logging out, this is deleted on unit_free
				clif_emotion(bl, ET_CRY);
				clif_clearunit_area(bl,clrtype);
				map_delblock(bl);
				unit_free(bl,CLR_OUTSIGHT);
				map_freeblock_unlock();

				return 0;
			}
			break;
		}
		case BL_MER: {
			struct mercenary_data *md = (struct mercenary_data *)bl;

			ud->canact_tick = ud->canmove_tick;

			if( mercenary_get_lifetime(md) <= 0 && !(md->master && !md->master->state.active) ) {
				clif_clearunit_area(bl,clrtype);
				map_delblock(bl);
				unit_free(bl,CLR_OUTSIGHT);
				map_freeblock_unlock();

				return 0;
			}
			break;
		}
		case BL_ELEM: {
			struct elemental_data *ed = (struct elemental_data *)bl;

			ud->canact_tick = ud->canmove_tick;

			if( elemental_get_lifetime(ed) <= 0 && !(ed->master && !ed->master->state.active) ) {
				clif_clearunit_area(bl,clrtype);
				map_delblock(bl);
				unit_free(bl,CLR_OUTSIGHT);
				map_freeblock_unlock();

				return 0;
			}
			break;
		}
		case BL_NPC:
			if (npc_remove_map( (TBL_NPC*)bl ) != 0)
				return 0;
			break;
		default:
			break;// do nothing
	}

	if (bl->type&(BL_CHAR|BL_PET)) {
		skill_unit_move(bl,gettick(),4);
		skill_cleartimerskill(bl);
	}

	switch (bl->type) {
		case BL_NPC:
			// already handled by npc_remove_map
			break;
		case BL_MOB:
			// /BL_MOB is handled by mob_dead unless the monster is not dead.
			if (status_isdead(bl)) {
				map_delblock(bl);
				break;
			}
			// Fall through
		default:
			clif_clearunit_area(bl, clrtype);
			map_delblock(bl);
			break;
	}

	map_freeblock_unlock();

	return 1;
}

/**
 * Removes units of a master when the master is removed from map
 * @param sd: Player
 * @param clrtype: How bl is being removed
 *	0: Assume bl is being warped
 *	1: Death, appropriate cleanup performed
 */
void unit_remove_map_pc(struct map_session_data *sd, clr_type clrtype)
{
	unit_remove_map(&sd->bl,clrtype);

	//CLR_RESPAWN is the warp from logging out, CLR_TELEPORT is the warp from teleporting, but pets/homunc need to just 'vanish' instead of showing the warping animation.
	if (clrtype == CLR_RESPAWN || clrtype == CLR_TELEPORT)
		clrtype = CLR_OUTSIGHT;

	if(sd->pd)
		unit_remove_map(&sd->pd->bl, clrtype);

	if(hom_is_active(sd->hd))
		unit_remove_map(&sd->hd->bl, clrtype);

	if(sd->md)
		unit_remove_map(&sd->md->bl, clrtype);

	if(sd->ed)
		unit_remove_map(&sd->ed->bl, clrtype);
}

/**
 * Frees units of a player when is removed from map
 * Also free his pets/homon/mercenary/elemental/etc if he have any
 * @param sd: Player
 */
void unit_free_pc(struct map_session_data *sd)
{
	if (sd->pd)
		unit_free(&sd->pd->bl,CLR_OUTSIGHT);

	if (sd->hd)
		unit_free(&sd->hd->bl,CLR_OUTSIGHT);

	if (sd->md)
		unit_free(&sd->md->bl,CLR_OUTSIGHT);

	if (sd->ed)
		unit_free(&sd->ed->bl,CLR_OUTSIGHT);

	unit_free(&sd->bl,CLR_TELEPORT);
}

/**
 * Frees all related resources to the unit
 * @param bl: Object being removed from map
 * @param clrtype: How bl is being removed
 *	0: Assume bl is being warped
 *	1: Death, appropriate cleanup performed
 * @return 0
 */
int unit_free(struct block_list *bl, clr_type clrtype)
{
	struct unit_data *ud = unit_bl2ud( bl );

	nullpo_ret(ud);

	map_freeblock_lock();

	if( bl->prev )	// Players are supposed to logout with a "warp" effect.
		unit_remove_map(bl, clrtype);

	switch( bl->type ) {
		case BL_PC: {
			struct map_session_data *sd = (struct map_session_data*)bl;
			int i;

			if( status_isdead(bl) )
				pc_setrestartvalue(sd,2);

			pc_delinvincibletimer(sd);

			pc_delautobonus(sd, sd->autobonus, false);
			pc_delautobonus(sd, sd->autobonus2, false);
			pc_delautobonus(sd, sd->autobonus3, false);

			if( sd->followtimer != INVALID_TIMER )
				pc_stop_following(sd);

			if( sd->duel_invite > 0 )
				duel_reject(sd->duel_invite, sd);

			channel_pcquit(sd,0xF); // Leave all chan
			skill_blockpc_clear(sd); // Clear all skill cooldown related

			// Notify friends that this char logged out. [Skotlex]
			map_foreachpc(clif_friendslist_toggle_sub, sd->status.account_id, sd->status.char_id, 0);
			party_send_logout(sd);
			guild_send_memberinfoshort(sd,0);
			pc_cleareventtimer(sd);
			pc_inventory_rental_clear(sd);
			pc_delspiritball(sd, sd->spiritball, 1);
			pc_delspiritcharm(sd, sd->spiritcharm, sd->spiritcharm_type);

			if( sd->st && sd->st->state != RUN ) {// free attached scripts that are waiting
				script_free_state(sd->st);
				sd->st = NULL;
				sd->npc_id = 0;
			}

			if( sd->combos.count ) {
				aFree(sd->combos.bonus);
				aFree(sd->combos.id);
				aFree(sd->combos.pos);
				sd->combos.count = 0;
			}

			if( sd->sc_display_count ) { /* [Ind] */
				for( i = 0; i < sd->sc_display_count; i++ )
					ers_free(pc_sc_display_ers, sd->sc_display[i]);

				sd->sc_display_count = 0;
				aFree(sd->sc_display);
				sd->sc_display = NULL;
			}

			if( sd->quest_log != NULL ) {
				aFree(sd->quest_log);
				sd->quest_log = NULL;
				sd->num_quests = sd->avail_quests = 0;
			}

			if (sd->qi_display) {
				aFree(sd->qi_display);
				sd->qi_display = NULL;
			}
			sd->qi_count = 0;

#if PACKETVER >= 20150513
			if( sd->hatEffectCount > 0 ){
				aFree(sd->hatEffectIDs);
				sd->hatEffectIDs = NULL;
				sd->hatEffectCount = 0;
			}
#endif

			if (sd->achievement_data.achievements)
				achievement_free(sd);

			// Clearing...
			if (sd->bonus_script.head)
				pc_bonus_script_clear(sd, BSF_REM_ALL);

			skill_clear_unitgroup(bl);
			status_change_clear(bl,1);
			break;
		}
		case BL_PET: {
			struct pet_data *pd = (struct pet_data*)bl;
			struct map_session_data *sd = pd->master;

			pet_hungry_timer_delete(pd);
			pet_clear_support_bonuses(sd);

			if( pd->pet.intimate > PET_INTIMATE_NONE )
				intif_save_petdata(pd->pet.account_id,&pd->pet);
			else { // Remove pet.
				intif_delete_petdata(pd->pet.pet_id);

				if (sd)
					sd->status.pet_id = 0;
			}

			if( sd )
				sd->pd = NULL;
			pd->master = NULL;

			skill_clear_unitgroup(bl);
			status_change_clear(bl,1);
			break;
		}
		case BL_MOB: {
			struct mob_data *md = (struct mob_data*)bl;

			mob_free_dynamic_viewdata( md );

			if( md->spawn_timer != INVALID_TIMER ) {
				delete_timer(md->spawn_timer,mob_delayspawn);
				md->spawn_timer = INVALID_TIMER;
			}

			if( md->deletetimer != INVALID_TIMER ) {
				delete_timer(md->deletetimer,mob_timer_delete);
				md->deletetimer = INVALID_TIMER;
			}

			if (md->lootitems) {
				aFree(md->lootitems);
				md->lootitems = NULL;
			}

			if( md->guardian_data ) {
				struct guild_castle* gc = md->guardian_data->castle;

				if( md->guardian_data->number >= 0 && md->guardian_data->number < MAX_GUARDIANS )
					gc->guardian[md->guardian_data->number].id = 0;
				else {
					int i;

					ARR_FIND(0, gc->temp_guardians_max, i, gc->temp_guardians[i] == md->bl.id);
					if( i < gc->temp_guardians_max )
						gc->temp_guardians[i] = 0;
				}

				aFree(md->guardian_data);
				md->guardian_data = NULL;
			}

			if( md->spawn ) {
				md->spawn->active--;

				if( !md->spawn->state.dynamic ) { // Permanently remove the mob
					if( --md->spawn->num == 0 ) { // Last freed mob is responsible for deallocating the group's spawn data.
						aFree(md->spawn);
						md->spawn = NULL;
					}
				}
			}

			if( md->base_status) {
				aFree(md->base_status);
				md->base_status = NULL;
			}

			skill_clear_unitgroup(bl);
			status_change_clear(bl,1);

			if( mob_is_clone(md->mob_id) )
				mob_clone_delete(md);

			if( md->tomb_nid )
				mvptomb_destroy(md);
			break;
		}
		case BL_HOM:
		{
			struct homun_data *hd = (TBL_HOM*)bl;
			struct map_session_data *sd = hd->master;

			hom_hungry_timer_delete(hd);

			if( hd->homunculus.intimacy > 0 )
				hom_save(hd);
			else {
				intif_homunculus_requestdelete(hd->homunculus.hom_id);

				if( sd )
					sd->status.hom_id = 0;
			}

			if( sd )
				sd->hd = NULL;
			hd->master = NULL;

			skill_clear_unitgroup(bl);
			status_change_clear(bl,1);
			break;
		}
		case BL_MER: {
			struct mercenary_data *md = (TBL_MER*)bl;
			struct map_session_data *sd = md->master;

			if( mercenary_get_lifetime(md) > 0 )
				mercenary_save(md);
			else {
				intif_mercenary_delete(md->mercenary.mercenary_id);

				if( sd )
					sd->status.mer_id = 0;
			}

			if( sd )
				sd->md = NULL;

			mercenary_contract_stop(md);
			md->master = NULL;

			skill_clear_unitgroup(bl);
			status_change_clear(bl,1);
			break;
		}
		case BL_ELEM: {
			struct elemental_data *ed = (TBL_ELEM*)bl;
			struct map_session_data *sd = ed->master;

			if( elemental_get_lifetime(ed) > 0 )
				elemental_save(ed);
			else {
				intif_elemental_delete(ed->elemental.elemental_id);

				if( sd )
					sd->status.ele_id = 0;
			}

			if( sd )
				sd->ed = NULL;

			elemental_summon_stop(ed);
			ed->master = NULL;

			skill_clear_unitgroup(bl);
			status_change_clear(bl,1);
			break;
		}
	}

	map_deliddb(bl);

	if( bl->type != BL_PC ) // Players are handled by map_quit
		map_freeblock(bl);

	map_freeblock_unlock();

	return 0;
}

int foundtargetID;
int64 targetdistance;
int64 targetdistanceb;
int targetthis;
struct block_list * targetbl;
struct mob_data * targetmd;
int founddangerID;
int64 dangerdistancebest;
struct block_list * dangerbl;
struct mob_data * dangermd;
int dangercount;
int warpx, warpy;
struct party_data *p;

bool ispartymember(struct map_session_data *sd)
{
	if (!p) return false;

	int i;
	for (i = 0; i < MAX_PARTY && !(p->party.member[i].char_id == sd->status.char_id); i++);

	if (!p || (i == MAX_PARTY)) { return false; }
	return true;
}

// nearest monster or other object the player can walk to
int targetnearestwarp(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;

	struct npc_data *nd;
	nd = (TBL_NPC*)bl;


	// looks like a warp then it's a warp even if it's not type warp. Some warps are NPCTYPE_SCRIPT instead!
	if ((nd->subtype != NPCTYPE_WARP) && ((!nd->vd) || (nd->vd->class_ != 45)))
		return 0; //Not a warp
//	ShowError("WarpDetected");
	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting

	int dist = distance_bl(&sd2->bl, bl);
	if ((dist < targetdistance)) { 
//		ShowError("WarpTargeted");
		targetdistance = dist; foundtargetID = bl->id; targetbl = bl;
	};

	return 0;
}

void resettargets()
{
	targetdistance = 999; targetdistanceb = 999; foundtargetID = -1;

}

void resettargets2()
{
	targetdistance = 0;  foundtargetID = -1;

}

int nofreachabletargets,nofshootabletargets;
int64 reachabletargets[32767];
int64 reachabletargetspathlen[32767];
int64 shootabletargets[32767];

int isreachable(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;

	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	struct walkpath_data wpd1;

	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting
	if (path_search(&wpd1, sd2->bl.m, sd2->bl.x, sd2->bl.y, bl->x, bl->y, 0, CELL_CHKNOPASS, MAX_WALKPATH))
	{
		reachabletargets[nofreachabletargets] = bl->id;
		reachabletargetspathlen[nofreachabletargets] = wpd1.path_len;
		nofreachabletargets++;
		return 1;
	}
	return 0;
	}

int isshootable(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;

	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting

	if (path_search_long(NULL, sd2->bl.m, sd2->bl.x, sd2->bl.y, bl->x, bl->y, CELL_CHKWALL, AUTOPILOT_RANGE_CAP)) {
		shootabletargets[nofshootabletargets] = bl->id;
		nofshootabletargets++;
		return 1;
	}
	return 0;
}


bool isshootabletarget(int64 ID)
{
	for (int i = 0; i < nofshootabletargets; i++) {
		if (shootabletargets[i] == ID) return true;
	}
	return false;
}

bool isreachabletarget(int64 ID)
{
	for (int i = 0; i < nofreachabletargets; i++) {
		if (reachabletargets[i] == ID) return true;
	}
	return false;
}

int reachabletargetpathlength(int64 ID)
{
	for (int i = 0; i < nofreachabletargets; i++) {
		if (reachabletargets[i] == ID) return reachabletargetspathlen[i];
	}
	return 999;
}

int rcap(int range) {
	if (range > AUTOPILOT_RANGE_CAP) return AUTOPILOT_RANGE_CAP;
	return range;
}

void getreachabletargets(struct map_session_data * sd)
{
	nofreachabletargets = 0; nofshootabletargets = 0;
	map_foreachinrange(isreachable, &sd->bl, MAX_WALKPATH, BL_MOB, sd);
	map_foreachinrange(isshootable, &sd->bl, AUTOPILOT_RANGE_CAP, BL_MOB, sd);
}



// Use this to pick a target to walk to/tank/engage in melee.
int targetnearestwalkto(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;

	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting

	struct walkpath_data wpd1;

	int dist; 
	if (isreachabletarget(bl->id)) {
		dist = reachabletargetpathlength(bl->id);
		int dist2 = dist + 12;
		if ((status_get_class_(bl) == CLASS_BOSS)) dist2 = dist2 - 12; // Always hit the boss in a crowd of nearby enemies
		if ((dist2 < targetdistanceb)) { targetdistance = dist; targetdistanceb = dist2; foundtargetID = bl->id; targetbl = &md->bl; targetmd = md; };
		return 1;
	}
	else return 0;

}

// Use this to target a skill or attack that goes over cliffs but not through walls
int targetnearest(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;

	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting

	struct walkpath_data wpd1;

	int dist= distance_bl(&sd2->bl, bl);
	int dist2 = dist + 12;
	if ((status_get_class_(bl) == CLASS_BOSS)) dist2 = dist2 - 12; // Always hit the boss in a crowd of nearby enemies
	if (dist2 < targetdistanceb) {
		if (isshootabletarget(bl->id)) {
			targetdistance = dist; targetdistanceb = dist2; foundtargetID = bl->id; targetbl = &md->bl; targetmd = md;
		}
		return 1;
	} else return 0;

}

// Get Hp of enemy in range
int counthp(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;

	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting

	struct walkpath_data wpd1;

	if (isshootabletarget(bl->id)) {
		targetdistance += md->status.hp;
		return 1;
	}
	else return 0;

}

int targetnearestusingranged(block_list * bl, va_list ap)
{
	
	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	if (md->sc.data[SC_PNEUMA]) return 0;

	return targetnearest(bl,ap);
}

int targetsoulexchange(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;
	struct map_session_data *sd = (struct map_session_data*)bl;

	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting

	if (pc_isdead(sd)) return 0;
	if (sd->sc.data[SC_EXTREMITYFIST2]) return 0;
	if (sd->sc.data[SC_NORECOVER_STATE]) return 0;
	// Must recover at least 20% SP
	if (sd->battle_status.sp>0.8*sd->battle_status.max_sp) return 0;

	int dist = min(sd2->battle_status.sp,sd->battle_status.max_sp) - sd->battle_status.sp;
	if (sd->state.asurapreparation) dist = 500;
	if ((dist > targetdistance) && (path_search(NULL, sd2->bl.m, sd2->bl.x, sd2->bl.y, bl->x, bl->y, 0, CELL_CHKNOPASS,9))) { targetdistance = dist; foundtargetID = bl->id; targetbl = bl; };

	return 1;
}

int warplocation(block_list * bl, va_list ap)
{

struct unit_data *ud2;
ud2 = unit_bl2ud(bl);

struct map_session_data *sd2;
sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting

int i;
for (i = 0; i < MAX_SKILLUNITGROUP && ud2->skillunit[i]; i++) {
	if (ud2->skillunit[i]->skill_id == AL_WARP)
	{
		int dist = distance_bl(&sd2->bl, bl);
		if (dist<16)
			if (path_search_long(NULL, sd2->bl.m, sd2->bl.x, sd2->bl.y, ud2->skillunit[i]->unit->bl.x, ud2->skillunit[i]->unit->bl.y, CELL_CHKWALL,16)) {
				warpx = ud2->skillunit[i]->unit->bl.x;
				warpy = ud2->skillunit[i]->unit->bl.y;
				return 1;
			}

//		if (((abs(ud2->skillunit[i]->unit->bl.x - ((sd2->bl.x + bl->x) / 2)) < 3) && (abs(ud2->skillunit[i]->unit->bl.y - ((sd2->bl.y + bl->y) / 2)) < 3))) return 0;
}
	}
return 0;
}

int targetbluepitcher(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;
	struct map_session_data *sd = (struct map_session_data*)bl;

	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting

	if (pc_isdead(sd)) return 0;
	if (sd->sc.data[SC_EXTREMITYFIST2]) return 0;
	if (sd->sc.data[SC_NORECOVER_STATE]) return 0;
	// Consider SP recovery setting on target
	if ((sd->battle_status.sp>sd->state.autospgoal*sd->battle_status.max_sp/100) && !(sd->state.asurapreparation)) return 0;
	// Nearly maxed, don't bother even for asura
	if (sd->battle_status.sp>0.98*sd->battle_status.max_sp) return 0;

	int dist = min(sd2->battle_status.sp, sd->battle_status.max_sp) - sd->battle_status.sp;
	if (sd->state.asurapreparation) dist = 500;
	if ((dist > targetdistance) && (path_search(NULL, sd2->bl.m, sd2->bl.x, sd2->bl.y, bl->x, bl->y, 0, CELL_CHKNOPASS,9))) { targetdistance = dist; foundtargetID = bl->id; targetbl = bl; };

	return 1;
}

int targethighestlevel(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;

	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting

	int dist = distance_bl(&sd2->bl, bl);
	if ((md->level > targetdistance) && (isreachabletarget(bl->id))) { targetdistance = md->level; foundtargetID = bl->id; targetbl = &md->bl; targetmd = md; };

	return 1;
}

int asuratarget(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;

	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting

	if (md->status.def_ele == ELE_GHOST) return 0;  // Ghosts are immune
	if (!(status_get_class_(bl) == CLASS_BOSS)) return 0; // Boss monsters only
	if (md->status.hp < 600 * sd2->status.base_level) return 0; // Must be strong enough monster
	if (targetdistance > md->status.hp) return 0; // target strongest first
	if (isreachabletarget(bl->id)) { targetdistance = md->status.hp; foundtargetID = bl->id; targetbl = &md->bl; targetmd = md; };

	return 1;
}

// base damage = currenthp + ((atk * currenthp * skill level) / maxhp)
// final damage = base damage + ((mirror image count + 1) / 5 * base damage) - (edef + sdef)
int finaltarget(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;

	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting

	int64 damage = (pc_rightside_atk(sd2) * 10 + sd2->battle_status.max_hp) * 2; // Assume 1 mirror image is missing

	if (md->status.def_ele == ELE_GHOST) return 0;  // Ghosts are immune
	if (md->status.hp < 0.6 * damage) return 0; // Must be strong enough monster
	if (targetdistance < md->status.hp) return 0; // target weakest first if it's strong enough
	if (isreachabletarget(bl->id)) { targetdistance = md->status.hp; foundtargetID = bl->id; targetbl = &md->bl; targetmd = md; };

	return 1;
}

int targetturnundead(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;

	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	// Affects undead only
	if (md->status.def_ele != ELE_UNDEAD) return 0;

	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting

	// target the highest hp, not the nearest enemy
	int dist = md->status.hp;
	if ((dist > targetdistance) && (isreachabletarget(bl->id))) { targetdistance = dist; foundtargetID = bl->id; targetbl = &md->bl; targetmd = md; };

	return 1;
}

int targeteska(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;

	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting

	// MDEF has to be above 75 to care. Reduced ASPD and movement is nice but if it increases MDEf vs a magic reliant party that's bad. If it lowers it that's awesome though!
	if (md->status.mdef2<=90) return 0;
	// Already affected, skip!
	if (md->sc.data[SC_SKA]) return 0;
	// Must be a significant enough monster to care
	if (md->status.max_hp < sd2->battle_status.matk_min * 40) return 0;

	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting

	// target the highest mdeF!
	int dist = md->status.mdef2;
	if ((dist > targetdistance) && (isreachabletarget(bl->id))) { targetdistance = dist; foundtargetID = bl->id; targetbl = &md->bl; targetmd = md; };

	return 1;
}

int targetdispel(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;

	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	// Only use on bosses
	if (!((status_get_class_(bl) == CLASS_BOSS))) return 0;

	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting

	
	if (!(isreachabletarget(bl->id))) return 0;
	
	if (
		(md->sc.data[SC_ASSUMPTIO]) || //**Note** I customized Assumptio to be removed by Dispell. If you did not, remove this line!
		(md->sc.data[SC_INCFLEERATE]) ||
		(md->sc.data[CR_REFLECTSHIELD])
		)
	{
		foundtargetID = bl->id; targetbl = &md->bl; targetmd = md; return 1;
	};

	return 0;
}

int targetdispel2(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;

	struct map_session_data *sd = (struct map_session_data*)bl;

	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;

	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting

	// Dispel Berserk from LKs when they are low on remaning HP
	if (
		((sd->sc.data[SC_BERSERK]) && (sd->status.hp<sd->status.max_hp*0.2))
		)
	{
		foundtargetID = bl->id; targetbl = &sd->bl; return 1;
	};

	return 0;
}


int signumcount(block_list * bl, va_list ap)
{

	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	if (md->sc.data[SC_SIGNUMCRUCIS]) return 0;

	if ((battle_check_undead(md->status.race, md->status.def_ele) || md->status.race == RC_DEMON))
	{
		if ((status_get_class_(bl) == CLASS_BOSS)) return 3; else return 1;
	}
	return 0;
}

// Elemental property decisions for picking an attack spell. 50% or below = not allowed, 125% or more = good
bool elemstrong(struct mob_data *md, int ele)
{
	if (ele == ELE_GHOST) {
		if ((md->status.def_ele == ELE_UNDEAD) && (md->status.ele_lv >= 2)) return 1;
		if (md->status.def_ele == ELE_GHOST) return 1;
		return 0;
	}
	if (ele == ELE_FIRE) {
		if (md->status.def_ele == ELE_UNDEAD) return 1;
		if (md->status.def_ele == ELE_EARTH) return 1;
		return 0;
	}
	if (ele == ELE_WATER) {
		if ((md->status.def_ele == ELE_UNDEAD) && (md->status.ele_lv >= 3)) return 1;
		if (md->status.def_ele == ELE_FIRE) return 1;
		return 0;
	}

	if (ele == ELE_WIND) {
		if (md->status.def_ele == ELE_WATER) return 1;
		return 0;
	}
	if (ele == ELE_EARTH) {
		if (md->status.def_ele == ELE_WIND) return 1;
		return 0;
	}
	if (ele == ELE_HOLY) {
		if ((md->status.def_ele == ELE_POISON) && (md->status.ele_lv >= 3)) return 1;
		if (md->status.def_ele == ELE_DARK) return 1;
		if (md->status.def_ele == ELE_UNDEAD) return 1;
		return 0;
	}
	if (ele == ELE_DARK) {
		if (md->status.def_ele == ELE_HOLY) return 1;
		return 0;
	}
	if (ele == ELE_POISON) {
		if ((md->status.def_ele == ELE_UNDEAD) && (md->status.ele_lv >= 2)) return 1;
		if (md->status.def_ele == ELE_GHOST) return 1;
		return 0;
	}
	if (ele == ELE_UNDEAD) {
		if ((md->status.def_ele == ELE_HOLY) && (md->status.ele_lv >= 2)) return 1;
		return 0;
	}
	if (ele == ELE_NEUTRAL) {
		return 0;
	}
	return 0;
}


bool elemallowed(struct mob_data *md, int ele)
{
	if (ele == ELE_GHOST) {
		if ((md->status.def_ele == ELE_NEUTRAL) && (md->status.ele_lv >= 2)) return 0;
		if ((md->status.def_ele == ELE_FIRE) && (md->status.ele_lv >= 3)) return 0;
		if ((md->status.def_ele == ELE_WATER) && (md->status.ele_lv >= 3)) return 0;
		if ((md->status.def_ele == ELE_WIND) && (md->status.ele_lv >= 3)) return 0;
		if ((md->status.def_ele == ELE_EARTH) && (md->status.ele_lv >= 3)) return 0;
		if ((md->status.def_ele == ELE_POISON) && (md->status.ele_lv >= 3)) return 0;
		if ((md->status.def_ele == ELE_HOLY) && (md->status.ele_lv >= 2)) return 0;
		if ((md->status.def_ele == ELE_DARK) && (md->status.ele_lv >= 2)) return 0;
		return 1;

	}
	if (ele == ELE_FIRE) {
		if ((md->status.def_ele == ELE_FIRE)) return 0;
		if ((md->status.def_ele == ELE_HOLY) && (md->status.ele_lv >= 2)) return 0;
		if ((md->status.def_ele == ELE_DARK) && (md->status.ele_lv >= 3)) return 0;
		return 1;
	}
	if (ele == ELE_WATER) {
		if ((md->status.def_ele == ELE_WATER)) return 0;
		if ((md->status.def_ele == ELE_HOLY) && (md->status.ele_lv >= 2)) return 0;
		if ((md->status.def_ele == ELE_DARK) && (md->status.ele_lv >= 3)) return 0;
		return 1;
	}
	if (ele == ELE_WIND) {
		if ((md->status.def_ele == ELE_WIND)) return 0;
		if ((md->status.def_ele == ELE_HOLY) && (md->status.ele_lv >= 2)) return 0;
		if ((md->status.def_ele == ELE_DARK) && (md->status.ele_lv >= 3)) return 0;
		return 1;

	}
	if (ele == ELE_EARTH) {
		if ((md->status.def_ele == ELE_EARTH)) return 0;
		if ((md->status.def_ele == ELE_HOLY) && (md->status.ele_lv >= 2)) return 0;
		if ((md->status.def_ele == ELE_DARK) && (md->status.ele_lv >= 3)) return 0;
		if ((md->status.def_ele == ELE_UNDEAD) && (md->status.ele_lv >= 4)) return 0;
		return 1;

	}
	if (ele == ELE_HOLY) {
		if ((md->status.def_ele == ELE_HOLY)) return 0;
		return 1;

	}
	if (ele == ELE_DARK) {
		if ((md->status.def_ele == ELE_POISON)) return 0;
		if ((md->status.def_ele == ELE_DARK)) return 0;
		if ((md->status.def_ele == ELE_UNDEAD)) return 0;
		return 1;

	}
	if (ele == ELE_POISON) {
		if ((md->status.def_ele == ELE_WATER) && (md->status.ele_lv >= 3)) return 0;
		if ((md->status.def_ele == ELE_GHOST) && (md->status.ele_lv >= 3)) return 0;
		if ((md->status.def_ele == ELE_POISON)) return 0;
		if ((md->status.def_ele == ELE_UNDEAD)) return 0;
		if ((md->status.def_ele == ELE_HOLY) && (md->status.ele_lv >= 2)) return 0;
		if ((md->status.def_ele == ELE_DARK)) return 0;
		return 1;

	}
	if (ele == ELE_UNDEAD) {
		if ((md->status.def_ele == ELE_WATER) && (md->status.ele_lv >= 3)) return 0;
		if ((md->status.def_ele == ELE_FIRE) && (md->status.ele_lv >= 3)) return 0;
		if ((md->status.def_ele == ELE_WIND) && (md->status.ele_lv >= 3)) return 0;
		if ((md->status.def_ele == ELE_EARTH) && (md->status.ele_lv >= 3)) return 0;
		if ((md->status.def_ele == ELE_POISON) && (md->status.ele_lv >= 2)) return 0;
		if ((md->status.def_ele == ELE_UNDEAD)) return 0;
		if ((md->status.def_ele == ELE_DARK)) return 0;
		return 1;

	}
	if (ele == ELE_NEUTRAL) {
		if ((md->status.def_ele == ELE_GHOST) && (md->status.ele_lv >= 2)) return 0;
		return 1;

	}
	return 1;

}


int endowneed(block_list * bl, va_list ap)
{
	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	uint16 elem = va_arg(ap, int); // the element

	if (elemstrong(md, elem)) return 10; // This target is weak to it, good 
	if (!elemallowed(md, elem)) return -30; // And strong enemies cancel 3 weak ones
	return -8; // These however aren't which is bad
}

int Magnuspriority(block_list * bl, va_list ap)
{
	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	if (!((battle_check_undead(md->status.race, md->status.def_ele) || md->status.race == RC_DEMON))) return 0;

	uint16 elem = va_arg(ap, int); // the element

	if (!elemallowed(md, elem)) return 0; // This target won't be hurt by this element (holy demon omg?)
	if (elemstrong(md, elem)) return 3; // This target is weak to it so it's worth 50% more
	return 2; // Default
}



int AOEPriority(block_list * bl, va_list ap)
{
	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	uint16 elem = va_arg(ap, int); // the element

	if ((status_get_class_(bl) == CLASS_BOSS)) { if (elemstrong(md, elem)) return 50; else return 30; }; // Bosses, prioritize AOE and pick element based on boss alone, ignore slaves
	if (!elemallowed(md, elem)) return 0; // This target won't be hurt by this element enough to care
	if (elemstrong(md, elem)) return 3; // This target is weak to it so it's worth 50% more
	return 2; // Default
}

int AOEPrioritySandman(block_list * bl, va_list ap)
{
	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	uint16 elem = va_arg(ap, int); // the element

	if ((status_get_class_(bl) == CLASS_BOSS)) {
		return 0; // Bosses can't sleep
	}
	if (md->status.agi>60) return 0; // Too resistant to sleeping
	return 2; // Default
}

bool isdisabled(mob_data* md)
{
	if ((md->sc.data[SC_FREEZE])) return true;
	if ((md->sc.data[SC_STONE])) return true;
	if ((md->sc.data[SC_SPIDERWEB])) return true;
	return false;
}

int AOEPriorityfreeze(block_list * bl, va_list ap)
{
	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	uint16 elem = va_arg(ap, int); // the element

	if ((status_get_class_(bl) == CLASS_BOSS)) {
		return 0; // Bosses can't be frozen
	}
	if (md->status.def_ele == ELE_UNDEAD) return 0; // Undead can't be frozen
	if (isdisabled(md)) return 0; // already frozen/stunned/etc

	return 2; // Default
}

int AOEPriorityGrav(block_list * bl, va_list ap)
{
	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	uint16 elem = va_arg(ap, int); // the element

	int32 m = 100; // Scale up priority by enemy mdef (each 200 = +100% priority)
	if (md->status.mdef+ md->status.mdef2 > m) {m = (md->status.mdef+ md->status.mdef2)/2; }

	if ((status_get_class_(bl) == CLASS_BOSS)) { if (elemstrong(md, elem)) return (50*m)/100; else return (30*m)/100; }; // Bosses, prioritize AOE and pick element based on boss alone, ignore slaves
	if (!elemallowed(md, elem)) return 0; // This target won't be hurt by this element enough to care
	if (elemstrong(md, elem)) return (3*m)/100; // This target is weak to it so it's worth 50% more
	return 2; // Default
}

int Quagmirepriority(block_list * bl, va_list ap)
{
	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	uint16 elem = va_arg(ap, int); // the element

	if (md->sc.data[SC_QUAGMIRE]) return 0;
	if ((status_get_class_(bl) == CLASS_BOSS)) return 10;
	return 2; // Default
}

int AOEPrioritySG(block_list * bl, va_list ap)
{
	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	uint16 elem = va_arg(ap, int); // the element
	
	if (md->sc.data[SC_FREEZE]) return 0;
	if (!elemallowed(md, elem)) return 0; // This target won't be hurt by this element enough to care
	if ((status_get_class_(bl) == CLASS_BOSS)) { if (elemstrong(md, elem)) return 60; else return 40; }; // Prefer SG over other AOEs on boss if no element applies
	if (elemstrong(md, elem)) return 3; // This target is weak to it so it's worth 50% more
	return 2; // Default
}

int AOEPriorityIP(block_list * bl, va_list ap)
{
	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	uint16 elem = va_arg(ap, int); // the element

	if (md->sc.data[SC_FREEZE]) return 0;
	if (!elemallowed(md, elem)) return 0; // This target won't be hurt by this element enough to care
	if (elemstrong(md, elem)) return 3; // This target is weak to it so it's worth 50% more
	return 2; // Default
}


int targetthischar(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	struct map_session_data *sd2;
	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting
	if ((sd->status.char_id == targetthis) && (path_search(NULL, sd2->bl.m, sd2->bl.x, sd2->bl.y, bl->x, bl->y, 0, CELL_CHKNOPASS))) { targetbl = bl; foundtargetID = bl->id; };

	return 0;
}

int targetDetoxify(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (sd->sc.data[SC_POISON]) { targetbl = bl; foundtargetID = sd->bl.id; };

	return 0;
}
int targetSlowPoison(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!(sd->sc.data[SC_SLOWPOISON])) if (sd->sc.data[SC_POISON]) { targetbl = bl; foundtargetID = sd->bl.id; };

	return 0;
}

int targetCure(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (sd->sc.data[SC_SILENCE]) { targetbl = bl; foundtargetID = sd->bl.id; };
	if (sd->sc.data[SC_CONFUSION]) { targetbl = bl; foundtargetID = sd->bl.id; };
	if (sd->sc.data[SC_BLIND]) { targetbl = bl; foundtargetID = sd->bl.id; };

	return 0;
}

int targetstatusrecovery(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (sd->sc.data[SC_FREEZE]) { targetbl = bl; foundtargetID = sd->bl.id; };
	if (sd->sc.data[SC_STONE]) { targetbl = bl; foundtargetID = sd->bl.id; };
	if (sd->sc.data[SC_STUN]) { targetbl = bl; foundtargetID = sd->bl.id; };

	return 0;
}

int targetlexdivina(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (sd->sc.data[SC_SILENCE]) { targetbl = bl; foundtargetID = sd->bl.id; };

	return 0;
}

int epiclesispriority(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;
	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 4;
	int abc = 0;
	if ((sd->battle_status.hp < sd->battle_status.max_hp*0.55)) abc++;
	if ((sd->battle_status.sp < sd->battle_status.max_sp*0.55)) abc++;
	return abc;

}


int targethealing(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;
	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting
	
//	if (sd2->bl.id == foundtargetID) return 0; // If can heal self, prioritize that

	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	// Always heal below 55% hp
	// or if we have a lot of sp
	// or if target is a ninja with Final Strike then go above 95% hp
	if ((sd->battle_status.hp<sd->battle_status.max_hp*0.55)
		|| ((sd2->battle_status.sp * 100) / sd2->battle_status.max_sp>(100 * sd->battle_status.hp) / sd->battle_status.max_hp+12)
		|| ((sd->battle_status.hp < sd->battle_status.max_hp*0.95) && (pc_checkskill(sd, NJ_ISSEN) >= 10))
		)
	{
		// prioritize lowest hp percentage player
		if (targetdistance > 100 * sd->battle_status.hp / sd->battle_status.max_hp) {
			targetdistance = 100 * sd->battle_status.hp / sd->battle_status.max_hp;
			targetbl = bl; foundtargetID = sd->bl.id;
		}
		return 1;
	}
	return 0;
}

int targetpneuma(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;
	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting
	if (pc_isdead(sd2)) return 0;

	struct mob_data *md;
	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	if (sd2->bl.id == foundtargetID) return 0; // If can pneuma self, prioritize that

	// monster has to be ranged
	if (md->status.rhw.range <= 3) return 0;
	// monster has to target a player
	struct block_list *tgtbl;
	if (!md->target_id) { return 0; }
	tgtbl = map_id2bl(md->target_id);
	if (tgtbl->type != BL_PC) return 0;
	//ShowError("Pneuma target found");
	// monster has to be far enough???
	foundtargetID = md -> target_id;
	targetbl = tgtbl;
	return 0;
}

int64 targetsoullink;

int targetlinks(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	struct map_session_data *sd2;
	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting

	if (!ispartymember(sd)) return 0;
	if (sd->sc.data[SC_SPIRIT]) return 0; // Already has link
	if (sd->bl.id == sd2->bl.id) return 0; // Can't link self

	targetsoullink = -1;
	if (pc_checkskill(sd2, SL_ALCHEMIST) > 0) if ((sd->class_ & MAPID_UPPERMASK) == MAPID_ALCHEMIST)
		targetsoullink = SL_ALCHEMIST;
	if (pc_checkskill(sd2, SL_MONK) > 0) if ((sd->class_ & MAPID_UPPERMASK) == MAPID_MONK)
		targetsoullink = SL_MONK;
	if (pc_checkskill(sd2, SL_STAR) > 0) if ((sd->class_ & MAPID_UPPERMASK) == MAPID_STAR_GLADIATOR)
		targetsoullink = SL_STAR;
	if (pc_checkskill(sd2, SL_SAGE) > 0) if ((sd->class_ & MAPID_UPPERMASK) == MAPID_SAGE)
		targetsoullink = SL_SAGE;
	if (pc_checkskill(sd2, SL_CRUSADER) > 0) if ((sd->class_ & MAPID_UPPERMASK) == MAPID_CRUSADER)
		targetsoullink = SL_CRUSADER;
	if (pc_checkskill(sd2, SL_SUPERNOVICE) > 0) if ((sd->class_ & MAPID_UPPERMASK) == MAPID_SUPER_NOVICE)
		targetsoullink = SL_SUPERNOVICE;
	if (pc_checkskill(sd2, SL_KNIGHT) > 0) if ((sd->class_ & MAPID_UPPERMASK) == MAPID_KNIGHT)
		targetsoullink = SL_KNIGHT;
	if (pc_checkskill(sd2, SL_WIZARD) > 0) if ((sd->class_ & MAPID_UPPERMASK) == MAPID_WIZARD)
		targetsoullink = SL_WIZARD;
	if (pc_checkskill(sd2, SL_PRIEST) > 0) if ((sd->class_ & MAPID_UPPERMASK) == MAPID_PRIEST)
		targetsoullink = SL_PRIEST;
	if (pc_checkskill(sd2, SL_BARDDANCER) > 0) if ((sd->class_ & MAPID_UPPERMASK) == MAPID_BARDDANCER)
		targetsoullink = SL_BARDDANCER;
	if (pc_checkskill(sd2, SL_ROGUE) > 0) if ((sd->class_ & MAPID_UPPERMASK) == MAPID_ROGUE)
		targetsoullink = SL_ROGUE;
	if (pc_checkskill(sd2, SL_ASSASIN) > 0) if ((sd->class_ & MAPID_UPPERMASK) == MAPID_ASSASSIN)
		targetsoullink = SL_ASSASIN;
	if (pc_checkskill(sd2, SL_BLACKSMITH) > 0) if ((sd->class_ & MAPID_UPPERMASK) == MAPID_BLACKSMITH)
		targetsoullink = SL_BLACKSMITH;
	if (pc_checkskill(sd2, SL_HUNTER) > 0) if ((sd->class_ & MAPID_UPPERMASK) == MAPID_HUNTER)
		targetsoullink = SL_HUNTER;
	if (pc_checkskill(sd2, SL_SOULLINKER) > 0) if ((sd->class_ & MAPID_UPPERMASK) == MAPID_SOUL_LINKER)
		targetsoullink = SL_SOULLINKER;
	if (pc_checkskill(sd2, SL_HIGH) > 0) if ((sd && (sd->class_&JOBL_UPPER) && !(sd->class_&JOBL_2) && sd->status.base_level < 70)) 
		targetsoullink = SL_HIGH;


	if (targetsoullink > 0) {
		targetbl = bl; foundtargetID = sd->bl.id;
		return 1;
	}

	return 0;
}

int targetincagi(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if (!sd->sc.data[SC_INCREASEAGI]) { targetbl = bl; foundtargetID = sd->bl.id; return 1; };

	return 0;
}

int targetexpiatio(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if (sd->state.autopilotmode == 3) return 0;
	if (!sd->sc.data[SC_EXPIATIO]) { targetbl = bl; foundtargetID = sd->bl.id; return 1; };

	return 0;
}


int targetbless(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if (!sd->sc.data[SC_BLESSING]) { targetbl = bl; foundtargetID = sd->bl.id; return 1; };
	if (sd->sc.data[SC_CURSE]) { targetbl = bl; foundtargetID = sd->bl.id; return 1; };

	return 0;
}

int targetkaahi(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;
	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if (((sd->class_ & MAPID_UPPERMASK) != MAPID_SOUL_LINKER) && (!sd2->sc.data[SC_SPIRIT])) return 0;  // Must be linker or linked
	if (!sd->sc.data[SC_KAAHI]) { targetbl = bl; foundtargetID = sd->bl.id; };

	return 0;
}

int targetkaizel(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;
	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if (((sd->class_ & MAPID_UPPERMASK) != MAPID_SOUL_LINKER) && (!sd2->sc.data[SC_SPIRIT])) return 0;  // Must be linker or linked
	if (!sd->sc.data[SC_KAIZEL]) { targetbl = bl; foundtargetID = sd->bl.id; };

	return 0;
}

int targetkaupe(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;
	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if (((sd->class_ & MAPID_UPPERMASK) != MAPID_SOUL_LINKER) && (!sd2->sc.data[SC_SPIRIT])) return 0;  // Must be linker or linked
	if (!sd->sc.data[SC_KAUPE]) { targetbl = bl; foundtargetID = sd->bl.id; };

	return 0;
}

int targetangelus(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if (!sd->sc.data[SC_ANGELUS]) { targetbl = bl; foundtargetID = sd->bl.id; };

	return 0;
}

int targetwindwalk(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if (!sd->sc.data[SC_WINDWALK]) { targetbl = bl; foundtargetID = sd->bl.id; };

	return 0;
}

int targetadrenaline(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if (!(((sd->status.weapon == W_MACE) || (sd->status.weapon == W_1HAXE) || (sd->status.weapon == W_2HAXE)))) return 0;
	if ((!sd->sc.data[SC_ADRENALINE]) && (!sd->sc.data[SC_ADRENALINE2])) { targetbl = bl; foundtargetID = sd->bl.id; };

	return 0;
}

int targetadrenaline2(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if (sd->status.weapon == W_BOW) return 0;
	if ((!sd->sc.data[SC_ADRENALINE]) && (!sd->sc.data[SC_ADRENALINE2])) { targetbl = bl; foundtargetID = sd->bl.id; };

	return 0;
}

int targetwperfect(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if (!sd->sc.data[SC_WEAPONPERFECTION]) { targetbl = bl; foundtargetID = sd->bl.id; };

	return 0;
}

int targetovert(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if ((!sd->sc.data[SC_OVERTHRUST]) && (!sd->sc.data[SC_MAXOVERTHRUST])){ targetbl = bl; foundtargetID = sd->bl.id; };

	return 0;
}

int targetmagnificat(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if (!sd->sc.data[SC_MAGNIFICAT]) { targetbl = bl; foundtargetID = sd->bl.id; };

	return 0;
}

int targetrenovatio(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if (!sd->sc.data[SC_RENOVATIO]) { targetbl = bl; foundtargetID = sd->bl.id; };

	return 0;
}

int targetgloria(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if (!sd->sc.data[SC_GLORIA]) { targetbl = bl; foundtargetID = sd->bl.id; };

	return 0;
}

int targetloud(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if (!(sd->sc.data[SC_LOUD])) { targetbl = bl; foundtargetID = sd->bl.id; };

	return 0;
}


int targetlauda1(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if (!sd->sc.data[SC_LAUDAAGNUS]) { targetbl = bl; foundtargetID = sd->bl.id; return 1; };
	if (sd->sc.data[SC_LAUDAAGNUS]) if (sd->sc.data[SC_LAUDAAGNUS]->timer<=2000) { targetbl = bl; foundtargetID = sd->bl.id; return 1; };
	if (sd->sc.data[SC_FREEZE]) { targetbl = bl; foundtargetID = sd->bl.id;  return 1;	};
	if (sd->sc.data[SC_FREEZING]) { targetbl = bl; foundtargetID = sd->bl.id;  return 1; };
	if (sd->sc.data[SC_STONE]) { targetbl = bl; foundtargetID = sd->bl.id;  return 1;	};
	if (sd->sc.data[SC_BURNING]) { targetbl = bl; foundtargetID = sd->bl.id;  return 1; };
	if (sd->sc.data[SC_CRYSTALIZE]) { targetbl = bl; foundtargetID = sd->bl.id;  return 1; };

	return 0;
}

int targetlauda2(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if (!sd->sc.data[SC_LAUDARAMUS]) { targetbl = bl; foundtargetID = sd->bl.id; return 1; };
	if (sd->sc.data[SC_STUN]) { targetbl = bl; foundtargetID = sd->bl.id;  return 1; };
	if (sd->sc.data[SC_SLEEP]) { targetbl = bl; foundtargetID = sd->bl.id;  return 1; };
	if (sd->sc.data[SC_SILENCE]) { targetbl = bl; foundtargetID = sd->bl.id;  return 1; };
	if (sd->sc.data[SC_DEEPSLEEP]) { targetbl = bl; foundtargetID = sd->bl.id;  return 1; };
	if (sd->sc.data[SC_FEAR]) { targetbl = bl; foundtargetID = sd->bl.id;  return 1; };


	return 0;
}

int targetassumptio(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if (!sd->sc.data[SC_ASSUMPTIO]) { targetbl = bl; foundtargetID = sd->bl.id; };

	return 0;
}

int targetkyrie(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if ((!sd->sc.data[SC_KYRIE]) && (!sd->sc.data[SC_ASSUMPTIO])) { targetbl = bl; foundtargetID = sd->bl.id; return 1; };
	return 0;
}

int targetsacrament(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if ((!sd->sc.data[SC_SECRAMENT])) { targetbl = bl; foundtargetID = sd->bl.id; return 1; };
	return 0;
}

bool canendow(map_session_data *sd) {
	return ((!sd->sc.data[SC_ASPERSIO]) && (!sd->sc.data[SC_FIREWEAPON])
		&& (!sd->sc.data[SC_WATERWEAPON]) && (!sd->sc.data[SC_WINDWEAPON]) && (!sd->sc.data[SC_EARTHWEAPON])
		&& (!sd->sc.data[SC_ENCPOISON]) && (!sd->sc.data[SC_SEVENWIND]) && (!sd->sc.data[SC_GHOSTWEAPON]) && (!sd->sc.data[SC_SHADOWWEAPON])
		&& ((sd->battle_status.batk > sd->status.base_level) || (sd->battle_status.batk > 120)));
}

int targetendow(block_list * bl, va_list ap)
{
	// Not already endowed, and is a physical attack class (base atk high enough)
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if (canendow(sd)) {
		targetbl = bl; foundtargetID = sd->bl.id;
		return 1;
	};

	return 0;
}

int targetresu(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) { targetbl = bl; foundtargetID = sd->bl.id; return 1; };

	return 0;
}

int targetmanus(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if (sd->state.autopilotmode == 3) return 0;
	if ((!sd->sc.data[SC_IMPOSITIO]) && ((sd->battle_status.batk>sd->status.base_level) || (sd->battle_status.batk>120))) { targetbl = bl; foundtargetID = sd->bl.id; };

	return 0;
}

int targetsuffragium(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if ((!sd->sc.data[SC_SUFFRAGIUM]) && ((sd->battle_status.int_*2 > sd->status.base_level) || (sd->battle_status.rhw.matk > 120))) { targetbl = bl; foundtargetID = sd->bl.id; };

	return 0;
}

int targetrepair(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;

	for ( int i = 0; i < MAX_INVENTORY; i++){
		if (((sd->inventory.u.items_inventory[i].nameid) > 0) && (sd->inventory.u.items_inventory[i].attribute != 0)){
			targetbl = bl; foundtargetID = sd->bl.id;
		}
	}
	return 0;
}



int countprovidence(block_list * bl, va_list ap)
{
	struct mob_data *md;
	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);
	if (md->status.def_ele == ELE_HOLY) return 1;
	if (md->status.race == RC_DEMON) return 1;
	return 0;
}

int targetprovidence(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	// Crusaders are invalid targets
	if ((sd->class_&MAPID_UPPERMASK) == MAPID_CRUSADER) return 0;
		// Must have at least 3 appropriate enemies neabry to cast
	if (map_foreachinrange(countprovidence, &sd->bl, 25, BL_MOB, sd) < 3) return 0;

	if ((!sd->sc.data[SC_PROVIDENCE])) { targetbl = bl; foundtargetID = sd->bl.id; return 1; };

	return 0;
}


int sgn(int x)
{
	if (x > 0) return 1;
	if (x < 0) return -1;
	return 0;
}

int finddanger(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;

	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting

	if (md->target_id != sd2->bl.id) return 0; // Monster isn't targeting us, skip rest to save CPU 
	// Will assume the monster can reach us and is actual danger if it managed to pick us as target
	// So no path checking is done.

	// Already protected from this monster by pneuma or safety wall? ignore!
	struct status_change *sc;
	sc = status_get_sc(bl);
	if ((sc->data[SC_PNEUMA]) && (md->status.rhw.range > 3)) return 0;
	if ((sc->data[SC_SAFETYWALL]) && (md->status.rhw.range <= 3)) return 0;
	if ((sc->data[SC_TATAMIGAESHI]) && (md->status.rhw.range > 3)) return 0;
	// Are we protected by Firewall between?
	if (md->status.rhw.range <= 3) {
		struct unit_data *ud2;
		ud2 = unit_bl2ud(&sd2->bl);
	int i;
	for (i = 0; i < MAX_SKILLUNITGROUP && ud2->skillunit[i]; i++) {
		if (ud2->skillunit[i]->skill_id == MG_FIREWALL)
			if (((abs(ud2->skillunit[i]->unit->bl.x - ((sd2->bl.x + bl->x) / 2)) < 3) && (abs(ud2->skillunit[i]->unit->bl.y - ((sd2->bl.y + bl->y) / 2)) < 3))) return 0;
		}
	}

	int dist = distance_bl(&sd2->bl, bl) - md->status.rhw.range;
	if ((dist < dangerdistancebest)) { 
		dangerdistancebest = dist; founddangerID = bl->id; dangerbl = &md->bl; dangermd = md; 
		return 1;
	};

	return 0;
}

// for deciding if safe to go near leader or there are enemies
int finddanger2(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;

	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	sd2 = va_arg(ap, struct map_session_data *); 

	if (md->target_id != sd2->bl.id) return 0; // Monster isn't targeting us, skip rest to save CPU 

	struct status_change *sc;
	sc = status_get_sc(bl);

	int dist = distance_bl(&sd2->bl, bl) - md->status.rhw.range;
	if ((dist < dangerdistancebest)) {
		dangerdistancebest = dist; founddangerID = bl->id; dangerbl = &md->bl; dangermd = md;
		return 1;
	};

	return 0;
}

// Returns how many tiles the fewest an enemy targeting us has to walk to
int inDanger(struct map_session_data * sd)
{
	// Effectst that prevent damage mean we are not in danger.
	if (sd->sc.data[SC_KYRIE]) return 999;

	founddangerID = -1; dangerdistancebest = 999;
	dangercount=map_foreachinrange(finddanger, &sd->bl, 14, BL_MOB, sd);
	return dangerdistancebest;
}

// Same as indanger but ignores protection effects. Used to decide if nontanks should move near leader
int inDangerLeader(struct map_session_data * sd)
{
	founddangerID = -1; dangerdistancebest = 999;
	dangercount = map_foreachinrange(finddanger2, &sd->bl, 14, BL_MOB, sd);
	return dangerdistancebest;
}


int provokethis(block_list * bl, va_list ap)
{
	struct map_session_data *sd2;
	struct map_session_data *sd3;

	struct mob_data *md;

	nullpo_ret(bl);
	nullpo_ret(md = (struct mob_data *)bl);

	sd2 = va_arg(ap, struct map_session_data *); // the player autopiloting

	// Can change target from skills?
	//if (!battle_config.mob_changetarget_byskill) return 0;
	// Already provoked, has no target or targeting us, no need
	if (md->state.provoke_flag == sd2->bl.id) return 0;
	if (!md->target_id) return 0;
	if (md->target_id == sd2->bl.id) return 0;
	if (md->status.def_ele == ELE_UNDEAD) return 0;
	if ((status_get_class_(bl) == CLASS_BOSS)) return 0;

	struct block_list *tgtbl;
	tgtbl = map_id2bl(md->target_id);
	nullpo_ret(sd3 = (struct map_session_data *)tgtbl);
	// ignore if monster is targeting another tanking mode character
	if (sd3->state.autopilotmode == 1) return 0;

	// want nearest anyway
	int dist = distance_bl(&sd2->bl, bl);
	if ((dist < targetdistance) && (path_search(NULL, sd2->bl.m, sd2->bl.x, sd2->bl.y, bl->x, bl->y, 0, CELL_CHKNOPASS,14))) { targetdistance = dist; foundtargetID = bl->id; targetbl = &md->bl; targetmd = md; };

	return 0;
}

int unit_skilluse_ifable(struct block_list *src, int target_id, uint16 skill_id, uint16 skill_lv)
{
	struct map_session_data *sd = (struct map_session_data*)src;
	int inf = skill_get_inf(skill_id);
	unsigned int tick = gettick();

	if (skill_get_sp(skill_id, skill_lv)>sd->battle_status.sp) return 0;

	if (battle_config.idletime_option&IDLE_USESKILLTOID)
		sd->idletime = last_tick;
	if ((pc_cant_act2(sd) || sd->chatID) && skill_id != RK_REFRESH && !(skill_id == SR_GENTLETOUCH_CURE &&
		(sd->sc.opt1 == OPT1_STONE || sd->sc.opt1 == OPT1_FREEZE || sd->sc.opt1 == OPT1_STUN)) &&
		sd->state.storage_flag && !(inf&INF_SELF_SKILL)) //SELF skills can be used with the storage open, issue: 8027
		return 0;
	if (pc_issit(sd))
		return 0;

	if (skill_isNotOk(skill_id, sd))
		return 0;

	if (sd->bl.id != target_id && inf&INF_SELF_SKILL)
		target_id = sd->bl.id; // never trust the client

	if (target_id < 0 && -target_id == sd->bl.id) // for disguises [Valaris]
		target_id = sd->bl.id;

	if (sd->ud.skilltimer != INVALID_TIMER) {
		if (skill_id != SA_CASTCANCEL && skill_id != SO_SPELLFIST)
			return 0;
	}
	else if (DIFF_TICK(tick, sd->ud.canact_tick) < 0) {
		if (sd->skillitem != skill_id) {
			//clif_skill_fail(sd, skill_id, USESKILL_FAIL_SKILLINTERVAL, 0);
			return 0;
		}
	}

	if (sd->sc.option&OPTION_COSTUME)
		return 0;

	if (sd->menuskill_id) {
		if (sd->menuskill_id == SA_TAMINGMONSTER) {
			clif_menuskill_clear(sd); //Cancel pet capture.
		}
		else if (sd->menuskill_id != SA_AUTOSPELL)
			return 0; //Can't use skills while a menu is open.
	}

	if (sd->skillitem == skill_id) {
		if (skill_lv != sd->skillitemlv)
			skill_lv = sd->skillitemlv;
		if (!(inf&INF_SELF_SKILL))
			pc_delinvincibletimer(sd); // Target skills thru items cancel invincibility. [Inkfish]
		unit_skilluse_id(&sd->bl, target_id, skill_id, skill_lv);
		return 0;
	}
	sd->skillitem = sd->skillitemlv = 0;

	if (SKILL_CHK_GUILD(skill_id)) {
		if (sd->state.gmaster_flag)
			skill_lv = guild_checkskill(sd->guild, skill_id);
		else
			skill_lv = 0;
	}
	else {
		skill_lv = min(pc_checkskill(sd, skill_id), skill_lv); //never trust client
	}

	pc_delinvincibletimer(sd);


//	unit_stop_walking(src, 1); // Important! If trying to skill while walking and out of range, skill gets queued 
	return unit_skilluse_id(src, target_id, skill_id, skill_lv, false);
}

void unit_skilluse_ifablexy(struct block_list *src, int target_id, uint16 skill_id, uint16 skill_lv)
{
	unsigned int tick = gettick();
	struct map_session_data *sd = (struct map_session_data*)src;

	if (!(skill_get_inf(skill_id)&INF_GROUND_SKILL))
		return; //Using a target skill on the ground? WRONG.

#ifdef RENEWAL
	if (pc_hasprogress(sd, WIP_DISABLE_SKILLITEM)) {
		clif_msg(sd, WORK_IN_PROGRESS);
		return;
	}
#endif

	//Whether skill fails or not is irrelevant, the char ain't idle. [Skotlex]
	if (battle_config.idletime_option&IDLE_USESKILLTOPOS)
		sd->idletime = last_tick;

	if (skill_isNotOk(skill_id, sd))
		return;
		if (pc_issit(sd)) {
			//clif_skill_fail(sd, skill_id, USESKILL_FAIL_LEVEL, 0);
			return;
		}

	if (sd->ud.skilltimer != INVALID_TIMER)
		return;

	if (DIFF_TICK(tick, sd->ud.canact_tick) < 0) {
		if (sd->skillitem != skill_id) {
			//clif_skill_fail(sd, skill_id, USESKILL_FAIL_SKILLINTERVAL, 0);
			return;
		}
	}

	if (sd->sc.option&OPTION_COSTUME)
		return;

	if (sd->menuskill_id) {
		if (sd->menuskill_id != SA_AUTOSPELL)
			return; //Can't use skills while a menu is open.
	}

	pc_delinvincibletimer(sd);


//	unit_stop_walking(src, 1); // Important! If trying to skill while walking and out of range, skill gets queued 
	struct block_list *tgtbl;
	tgtbl = map_id2bl(target_id);
	int tgtx, tgty; tgtx = tgtbl->x; tgty = tgtbl->y;

	// Pneuma is special. Always put it on tiles with coordinates divisible by 3 to avoid unintended overlap, target is still in range.
	if (skill_id == AL_PNEUMA) {
		tgtx = 3 * trunc((tgtx + 1) / 3);
		tgty = 3 * trunc((tgty + 1) / 3);
	}

	if (sd->skillitem == skill_id) {
		if (skill_lv != sd->skillitemlv)
			skill_lv = sd->skillitemlv;
		unit_skilluse_pos(&sd->bl, tgtx, tgty, skill_id, skill_lv, false);
	}
	else {
		int lv;
		sd->skillitem = sd->skillitemlv = 0;
		if ((lv = pc_checkskill(sd, skill_id)) > 0) {
			if (skill_lv > lv)
				skill_lv = lv;
			unit_skilluse_pos(&sd->bl, tgtx, tgty, skill_id, skill_lv, false);
		}
	}

}

void unit_skilluse_ifablebetween(struct block_list *src, int target_id, uint16 skill_id, uint16 skill_lv)
{
	unsigned int tick = gettick();
	struct map_session_data *sd = (struct map_session_data*)src;

	if (!(skill_get_inf(skill_id)&INF_GROUND_SKILL))
		return; //Using a target skill on the ground? WRONG.

#ifdef RENEWAL
	if (pc_hasprogress(sd, WIP_DISABLE_SKILLITEM)) {
		clif_msg(sd, WORK_IN_PROGRESS);
		return;
	}
#endif

	//Whether skill fails or not is irrelevant, the char ain't idle. [Skotlex]
	if (battle_config.idletime_option&IDLE_USESKILLTOPOS)
		sd->idletime = last_tick;

	if (skill_isNotOk(skill_id, sd))
		return;
	if (pc_issit(sd)) {
		//clif_skill_fail(sd, skill_id, USESKILL_FAIL_LEVEL, 0);
		return;
	}

	if (sd->ud.skilltimer != INVALID_TIMER)
		return;

	if (DIFF_TICK(tick, sd->ud.canact_tick) < 0) {
		if (sd->skillitem != skill_id) {
			//clif_skill_fail(sd, skill_id, USESKILL_FAIL_SKILLINTERVAL, 0);
			return;
		}
	}

	if (sd->sc.option&OPTION_COSTUME)
		return;

	if (sd->menuskill_id) {
		if (sd->menuskill_id != SA_AUTOSPELL)
			return; //Can't use skills while a menu is open.
	}

	pc_delinvincibletimer(sd);

	//unit_stop_walking(src, 1); // Important! If trying to skill while walking and out of range, skill gets queued 
	struct block_list *tgtbl;
	tgtbl = map_id2bl(target_id);
	if (sd->skillitem == skill_id) {
		if (skill_lv != sd->skillitemlv)
			skill_lv = sd->skillitemlv;
		unit_skilluse_pos(&sd->bl, (src->x+tgtbl->x)/2, (src->y+tgtbl->y)/2, skill_id, skill_lv, false);
	}
	else {
		int lv;
		sd->skillitem = sd->skillitemlv = 0;
		if ((lv = pc_checkskill(sd, skill_id)) > 0) {
			if (skill_lv > lv)
				skill_lv = lv;
			unit_skilluse_pos(&sd->bl, (src->x + tgtbl->x) / 2, (src->y + tgtbl->y) / 2, skill_id, skill_lv, false);
		}
	}

}

void saythis(struct map_session_data * src, char* message, int i)
{
	if ((rand() % i) != 1) return;
	/*
	send_target target = PARTY;
	struct StringBuf sbuf;

	StringBuf_Init(&sbuf);
	StringBuf_Printf(&sbuf, "%s", message);
	clif_disp_overhead_(src, StringBuf_Value(&sbuf), target);
	StringBuf_Destroy(&sbuf);
	*/
	char* msg = new char[300];
	strcpy(msg, src->status.name);
	strcat(msg, ":");
	strcat(msg, message);

	party_send_message(src, msg, strlen(msg) + 1);

}

bool duplicateskill(struct party_data *p, int skillID) {

	if (!p) return false;
	int i;
	unit_data * ud;
	for (i = 0; i < MAX_PARTY; i++) {
		struct map_session_data * sd = p->data[i].sd;
		if (sd) {
			ud = unit_bl2ud(&sd->bl);
			if (ud->skill_id == skillID) return true;
		}
	}
	return false;
}


int shurikenchange(map_session_data * sd, mob_data *targetmd)
{
	unsigned short skens[] = {
		 13295,13250,13251,13252,13253,13254,13255
	};
	unsigned short skenlevel[] = {
		 1,1,20,40,60,80
	};

	if (DIFF_TICK(sd->canequip_tick, gettick()) > 0) return 0;

	int16 index = -1; int j = -1;
	int i;
	for (i = 0; i < ARRAYLENGTH(skens); i++) {
		if ((index = pc_search_inventory(sd, skens[i])) >= 0) {
			if (sd->status.base_level >= skenlevel[i]) j = index;
		}
	}
		if (j > -1) {
			pc_equipitem(sd, j, EQP_AMMO);
			return 1;
		}
		else {
			char* msg = "I have no shurikens to throw!";
			saythis(sd, msg, 50);
			return 0;
		}
}


int arrowchange(map_session_data * sd, mob_data *targetmd)
{
	unsigned short arrows[] = {
		1750, 1751, 1752, 1753, 1754, 1755, 1756, 1757, 1762, 1765, 1766, 1767 ,1770, 1772, 1773, 1774
	};
	unsigned short arrowelem[] = {
		ELE_NEUTRAL, ELE_HOLY, ELE_FIRE, ELE_NEUTRAL, ELE_WATER, ELE_WIND, ELE_EARTH, ELE_GHOST, ELE_NEUTRAL, ELE_POISON, ELE_HOLY, ELE_DARK, ELE_NEUTRAL, ELE_HOLY, ELE_NEUTRAL, ELE_NEUTRAL
	};
	unsigned short arrowatk[] = {
		25,30,30,40,30,30,30,30,30,50,50,30,30,50,45,35
	};

	if (DIFF_TICK(sd->canequip_tick, gettick()) > 0) return 0;

	int16 index = -1;
	int i,j;
	int best = -1; int bestprio = -1;
	bool eqp = false;

	for (i = 0; i < ARRAYLENGTH(arrows); i++) {
		if ((index = pc_search_inventory(sd, arrows[i])) >= 0) {
		j = arrowatk[i];
		if (elemstrong(targetmd, arrowelem[i])) j += 500;
		if (elemallowed(targetmd, arrowelem[i])) if (j>bestprio) {
			bestprio = j; best = index; eqp = pc_checkequip2(sd, arrows[i], EQI_AMMO, EQI_AMMO+1);
		}
		}
	}
	if (best > -1) {
		if (!eqp) pc_equipitem(sd, best, EQP_AMMO);
		return 1;
	}
	else {
		char* msg = "I have no arrows to shoot my target!";
		saythis(sd, msg, 50);
		return 0;
	}

}

/* These are no longer a thing it seems
int grenchange(map_session_data * sd, mob_data *targetmd)
{
	unsigned short arrows[] = {
		13203,13204,13205,13206,13207,
		13223,13224,13225,13226,13227
	};
	unsigned short arrowelem[] = {
		ELE_FIRE, ELE_WIND, ELE_POISON, ELE_DARK, ELE_WATER,
		ELE_FIRE, ELE_WIND, ELE_POISON, ELE_DARK, ELE_WATER
	};
	unsigned short arrowatk[] = {
		50,50,50,50,50,50,50,50,50,50
	};

	if (DIFF_TICK(sd->canequip_tick, gettick()) > 0) return 0;

	int16 index = -1;
	int i, j;
	int best = -1; int bestprio = -1;

	for (i = 0; i < ARRAYLENGTH(arrows); i++) {
		if ((index = pc_search_inventory(sd, arrows[i])) >= 0) {
			j = arrowatk[i];
			if (elemstrong(targetmd, arrowelem[i])) j += 500;
			if (elemallowed(targetmd, arrowelem[i])) if (j > bestprio) {
				bestprio = j; best = index;
			}
		}
	}
	if (best > -1) {
		pc_equipitem(sd, best, EQP_AMMO);
		return 1;
	}
	else {
		char* msg = "I have no grenades to shoot my target!";
		saythis(sd, msg, 50);
		return 0;
	}

} */

int ammochange(map_session_data * sd, mob_data *targetmd)
{
	unsigned short arrows[] = {
		13200, 13201, 13215, 13216, 13217,
		13218, 13219, 13220, 13221, 13228,
		13229, 13230, 13231, 13232
	};
	unsigned short arrowelem[] = {
		ELE_NEUTRAL, ELE_HOLY, ELE_NEUTRAL, ELE_FIRE, ELE_WATER,
		ELE_WIND, ELE_EARTH, ELE_HOLY, ELE_HOLY, ELE_FIRE,
		ELE_WIND, ELE_WATER, ELE_POISON, ELE_DARK
	};
	unsigned short arrowatk[] = {
		25,15,50,40,40,
		40,40,40,15,20,
		20,20,20,20
	};
	unsigned short arrowlvl[] = {
		1,1,100,100,100,
		100,100,100,1,1,
		1,1,1,1
	};

	if (DIFF_TICK(sd->canequip_tick, gettick()) > 0) return 0;

	int16 index = -1;
	int i, j;
	int best = -1; int bestprio = -1;
	bool eqp = false;

	for (i = 0; i < ARRAYLENGTH(arrows); i++) {
		if ((index = pc_search_inventory(sd, arrows[i])) >= 0) {
			j = arrowatk[i];
			if (elemstrong(targetmd, arrowelem[i])) j += 500;
			if (elemallowed(targetmd, arrowelem[i])) if (j > bestprio) if (sd->status.base_level >= arrowlvl[i])  {
				bestprio = j; best = index;  eqp = pc_checkequip2(sd, arrows[i], EQI_AMMO, EQI_AMMO + 1);
			}
		}
	}
	if (best > -1) {
		if (!eqp) pc_equipitem(sd, best, EQP_AMMO);
		return 1;
	}
	else {
		char* msg = "I have no bullets to shoot my target!";
		saythis(sd, msg, 50);
		return 0;
	}

}

int kunaichange(map_session_data * sd, mob_data *targetmd)
{
	unsigned short arrows[] = {
		13255,13256,13257,13258,13259,13294
	};
	unsigned short arrowelem[] = {
		ELE_WATER, ELE_EARTH, ELE_WIND, ELE_FIRE, ELE_POISON, ELE_NEUTRAL
	};
	unsigned short arrowatk[] = {
		30,30,30,30,30,50
	};

	if (DIFF_TICK(sd->canequip_tick, gettick()) > 0) return 0;

	int16 index = -1;
	int i, j;
	int best = -1; int bestprio = -1;
	bool eqp = false;

	for (i = 0; i < ARRAYLENGTH(arrows); i++) {
		if ((index = pc_search_inventory(sd, arrows[i])) >= 0) {
			j = arrowatk[i];
			if (elemstrong(targetmd, arrowelem[i])) j += 500;
			if (elemallowed(targetmd, arrowelem[i])) if (j > bestprio)
				// Explosive Kunai has a level requirement
				if ((arrows[i]!=13294) || (sd->status.base_level>=100)){
				bestprio = j; best = index; eqp = pc_checkequip2(sd, arrows[i], EQI_AMMO, EQI_AMMO + 1);
			}
		}
	}
	if (best > -1) {
		if (!eqp) pc_equipitem(sd, best, EQP_AMMO);
		return 1;
	}
	else {
		char* msg = "I have no kunai left to throw!";
		saythis(sd, msg, 50);
		return 0;
	}

}


void recoversp(map_session_data *sd, int goal)
{
	// SP Recovery disabled
	if (sd->sc.data[SC_EXTREMITYFIST2]) return;
	if (sd->sc.data[SC_NORECOVER_STATE]) return;

	int16 index = -1;

	// Ancilla is special, always use it even if not set to use sp items
	if ((index = pc_search_inventory(sd,12333)) >= 0)
		if ((sd->battle_status.sp < (goal*sd->battle_status.max_sp) / 100)
			|| (sd->battle_status.sp < (25*sd->battle_status.max_sp) / 100))
		pc_useitem(sd, index);

	if (sd->battle_status.sp <  (goal*sd->battle_status.max_sp) / 100) {
		//ShowError("Need to heal");

		unsigned short spotions[] = {
			533, // Grape Juice
			518, // Honey
			514, // Grape
			578, // Strawberry
			582, // Orange
			505, // Blue Potion
			11502, // Light Blue Pot
			608, // Ygg Seed
			607 // Ygg Berry

		};

		index = -1;
		int i;

		for (i = 0; i < ARRAYLENGTH(spotions); i++) {
			if ((index = pc_search_inventory(sd, spotions[i])) >= 0) {
				//ShowError("Found a potion to use");
				if (pc_isUseitem(sd, index)) {
					pc_useitem(sd, index);
					break;
				}
			}
		}

	}

}

int ammochange2(map_session_data * sd, mob_data *targetmd) {
	if (sd->status.weapon == W_REVOLVER) { return ammochange(sd, targetmd); }
	if (sd->status.weapon == W_RIFLE) { return ammochange(sd, targetmd); }
	if (sd->status.weapon == W_GATLING) { return ammochange(sd, targetmd); }
	if (sd->status.weapon == W_SHOTGUN) { return ammochange(sd, targetmd); }
	if (sd->status.weapon == W_GRENADE) { return ammochange(sd, targetmd); }
	// if (sd->status.weapon == W_GRENADE) { return grenchange(sd, targetmd); }
	return 0;
}


void skillwhenidle(struct map_session_data *sd) {

	// Fury
	// Use if tanking mode only, otherwise unlikely to be normal attacking so crit doesn't matter.
	// For Asura preparation, it is used in the asura strike logic instead
	// **Note** : I modded this to not reduce SP regen. If it reduces SP regen, it might be better if the AI never uses it.
		if ((pc_checkskill(sd, MO_EXPLOSIONSPIRITS) > 0) ){
		if (!(sd->sc.data[SC_EXPLOSIONSPIRITS]))
		if ((sd->spiritball>=5) && (sd->state.autopilotmode==1)) {
			unit_skilluse_ifable(&sd->bl, SELF, MO_EXPLOSIONSPIRITS, pc_checkskill(sd, MO_EXPLOSIONSPIRITS));
		}
	}

	// Dangerous Soul Collect (Zen)
	if (pc_checkskill(sd, CH_SOULCOLLECT) > 0) {
		int radra = 0; if (sd->sc.data[SC_RAISINGDRAGON]) { radra = sd->sc.data[SC_RAISINGDRAGON]->val1; }
		if (4 + radra>sd->spiritball) {
			unit_skilluse_ifable(&sd->bl, SELF, CH_SOULCOLLECT, pc_checkskill(sd, CH_SOULCOLLECT));
		}
	}

	// Defending Aura
	// Turn it off when not actively fighting. This ensures it won't be accidentaly left on when leaving the area with ranged monsters.
	if ((pc_checkskill(sd, CR_DEFENDER) > 0) &&
		(sd->sc.data[SC_DEFENDER]))
	{
		unit_skilluse_ifable(&sd->bl, SELF, CR_DEFENDER, pc_checkskill(sd, CR_DEFENDER));
	}

	// Flip Coin
	if ((pc_checkskill(sd, GS_GLITTERING) > 4)) {
		if ((sd->spiritball < 10)) {
				unit_skilluse_ifable(&sd->bl, SELF, GS_GLITTERING, pc_checkskill(sd, GS_GLITTERING));
			}
	}

	// Magical Bullet
	if ((pc_checkskill(sd, GS_MAGICALBULLET) > 0)) {
		if (!(sd->sc.data[SC_MAGICALBULLET]))
			// Must have high ASPD and INT
			if (sd->battle_status.agi>=0.6*sd->status.base_level)
			if (sd->battle_status.matk_min >= 1.2*sd->status.base_level)
			if ((sd->spiritball >= 10)) {
				unit_skilluse_ifable(&sd->bl, SELF, GS_MAGICALBULLET, pc_checkskill(sd, GS_MAGICALBULLET));
			}
	}

	// Summon Spirit Sphere
	if (pc_checkskill(sd, MO_CALLSPIRITS) > 0) {
		int radra = 0; if (sd->sc.data[SC_RAISINGDRAGON]) { radra = sd->sc.data[SC_RAISINGDRAGON]->val1; }
		if (pc_checkskill(sd, MO_CALLSPIRITS) + radra>sd->spiritball) {
			unit_skilluse_ifable(&sd->bl, SELF, MO_CALLSPIRITS, pc_checkskill(sd, MO_CALLSPIRITS));
		}
	}

	// Sightblaster
	if (pc_checkskill(sd, WZ_SIGHTBLASTER) > 0) {
		if (!(sd->sc.data[SC_SIGHTBLASTER])) {
			unit_skilluse_ifable(&sd->bl, SELF, WZ_SIGHTBLASTER, pc_checkskill(sd, WZ_SIGHTBLASTER));
		}
	}

	// Pick Stone
	if (pc_checkskill(sd, TF_PICKSTONE) > 0) {
		if (pc_inventory_count(sd, 7049) < 12) {
			unit_skilluse_ifable(&sd->bl, SELF, TF_PICKSTONE, pc_checkskill(sd, TF_PICKSTONE));
		}
	}
	// Aqua Benedicta
	if (pc_checkskill(sd, AL_HOLYWATER) > 0) {
		if ((pc_inventory_count(sd, 523) < 40) && (pc_inventory_count(sd, ITEMID_EMPTY_BOTTLE)>0)
			&& (skill_produce_mix(sd, AL_HOLYWATER, ITEMID_HOLY_WATER, 0, 0, 0, 1, -1))) {
			unit_skilluse_ifable(&sd->bl, SELF, AL_HOLYWATER, pc_checkskill(sd, AL_HOLYWATER));
		}
	}
	// Energy Coat
	if (pc_checkskill(sd, MG_ENERGYCOAT) > 0) {
		if (!(sd->sc.data[SC_ENERGYCOAT])) {
			unit_skilluse_ifable(&sd->bl, SELF, MG_ENERGYCOAT, pc_checkskill(sd, MG_ENERGYCOAT));
		}
	}

	// Double Casting
	if (pc_checkskill(sd, PF_DOUBLECASTING) > 0) {
		if (!(sd->sc.data[SC_DOUBLECAST])) {
			unit_skilluse_ifable(&sd->bl, SELF, PF_DOUBLECASTING, pc_checkskill(sd, PF_DOUBLECASTING));
		}
	}

	// AUTOSPELL
	if (pc_checkskill(sd, SA_AUTOSPELL) > 0)
	if (sd->state.autopilotmode==1) { // Tanking mode only, this triggers on normal physical atacks
		if (!(sd->sc.data[SC_AUTOSPELL])) {
			unit_skilluse_ifable(&sd->bl, SELF, SA_AUTOSPELL, pc_checkskill(sd, SA_AUTOSPELL));
		}
	}

	// Memorize
	if (pc_checkskill(sd, PF_MEMORIZE) > 0) {
		if (!(sd->sc.data[SC_MEMORIZE])) {
			unit_skilluse_ifable(&sd->bl, SELF, PF_MEMORIZE, pc_checkskill(sd, PF_MEMORIZE));
		}
	}

	// Cart Boost
	if (pc_checkskill(sd, WS_CARTBOOST) > 0) {
		if (!(sd->sc.data[SC_CARTBOOST])) 
			if  (pc_iscarton(sd)) {
			unit_skilluse_ifable(&sd->bl, SELF, WS_CARTBOOST, pc_checkskill(sd, WS_CARTBOOST));
		}
	}
	
	// Weapon Repair
	if (pc_checkskill(sd, BS_REPAIRWEAPON) > 0) {
		if ((pc_search_inventory(sd, 998) >= 0) && (pc_search_inventory(sd, 1002) >= 0) && (pc_search_inventory(sd, 999) >= 0)
			&& (pc_search_inventory(sd, 756) >= 0))
		{
			resettargets();
			map_foreachinrange(targetrepair, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, foundtargetID, BS_REPAIRWEAPON, pc_checkskill(sd, BS_REPAIRWEAPON));
			}
		} else {
			char* msg = "My repair material set is incomplete! (Iron Ore, Iron, Steel, Rough Oridecon)";
			saythis(sd, msg, 50);

		}
	}

	// Amplify Magic Power
	if (pc_checkskill(sd, HW_MAGICPOWER) > 0) {
		if (!(sd->sc.data[SC_MAGICPOWER])) {
			unit_skilluse_ifable(&sd->bl, SELF, HW_MAGICPOWER, pc_checkskill(sd, HW_MAGICPOWER));
		}
	}

	// Guns - Increase Accuracy
	if (pc_checkskill(sd, GS_INCREASING) > 0) if (sd->spiritball >= 4) {
		if (!(sd->sc.data[SC_INCREASING])) {
			unit_skilluse_ifable(&sd->bl, SELF, GS_INCREASING, pc_checkskill(sd, GS_INCREASING));
		}
	}
	
	// Ninja Aura
	if (pc_checkskill(sd, NJ_NEN) > 0) {
		if (!(sd->sc.data[SC_NEN])) {
			unit_skilluse_ifable(&sd->bl, SELF, NJ_NEN, pc_checkskill(sd, NJ_NEN));
		}
	}

	// Taekwon Sprint
	if (pc_checkskill(sd, TK_RUN) >= 7) {
		if ((sd->status.weapon == W_FIST))
			if (!(sd->sc.data[SC_SPURT]))
		{
			unit_skilluse_ifable(&sd->bl, SELF, TK_RUN, pc_checkskill(sd, TK_RUN));
		}
	}

	// Duple Light
	if (pc_checkskill(sd, AB_DUPLELIGHT) > 0) if (sd->state.autopilotmode == 1) {
		if (!(sd->sc.data[SC_DUPLELIGHT])) {
			unit_skilluse_ifable(&sd->bl, SELF, AB_DUPLELIGHT, pc_checkskill(sd, AB_DUPLELIGHT));
		}
	}

	// Ancilla - create when high on sp and idle
	if (pc_checkskill(sd, AB_ANCILLA) > 0) 
		if (pc_inventory_count(sd, ITEMID_BLUE_GEMSTONE) >= 12)
		if (pc_inventory_count(sd, 12333) <3)
			if (sd->battle_status.sp >= 0.9*sd->battle_status.max_sp)
				unit_skilluse_ifable(&sd->bl, SELF, AB_ANCILLA, pc_checkskill(sd, AB_ANCILLA));
	
	// Turn off Gatling Fever if stopped fighting
	if (pc_checkskill(sd, GS_GATLINGFEVER) > 0) if (sd->sc.data[SC_GATLINGFEVER]) unit_skilluse_ifable(&sd->bl, SELF, GS_GATLINGFEVER, pc_checkskill(sd, GS_GATLINGFEVER));

	return;
}

// Reduce lag - do not try using skills if already decided to use one and started it
// Checking this ahead of time instead of executing all the logic and targeting for all the skills only do fail them is better
bool canskill(struct map_session_data *sd)
{
	return ((sd->ud.skilltimer == INVALID_TIMER) && (DIFF_TICK(gettick(), sd->ud.canact_tick) >= 0));

};

void sitdown(struct map_session_data *sd) {
	if (pc_checkskill(sd, LK_TENSIONRELAX) > 0)
	{
		unit_skilluse_ifable(&sd->bl, SELF, LK_TENSIONRELAX, pc_checkskill(sd, LK_TENSIONRELAX));
	}
	else {
		pc_setsit(sd);
		skill_sit(sd, 1);
		clif_sitting(&sd->bl);
	}
}

void aspdpotion(struct map_session_data *sd)
{	int index;
	// Berserk potion
	if ((index = pc_search_inventory(sd, 657)) >= 0)
	if (!(sd->sc.data[SC_ASPDPOTION0])) if (!(sd->sc.data[SC_ASPDPOTION1])) if (!(sd->sc.data[SC_ASPDPOTION2])) {
		if (pc_isUseitem(sd, index)) pc_useitem(sd, index);
	}
	// Awakening Potion potion
	if ((index = pc_search_inventory(sd, 656)) >= 0)
		if (!(sd->sc.data[SC_ASPDPOTION0])) if (!(sd->sc.data[SC_ASPDPOTION1])) if (!(sd->sc.data[SC_ASPDPOTION2])) {
			if (pc_isUseitem(sd, index)) pc_useitem(sd, index);
		}
	// Concentration potion
	if ((index = pc_search_inventory(sd, 645)) >= 0)
		if (!(sd->sc.data[SC_ASPDPOTION0])) if (!(sd->sc.data[SC_ASPDPOTION1])) if (!(sd->sc.data[SC_ASPDPOTION2])) {
			if (pc_isUseitem(sd, index)) pc_useitem(sd, index);
		}


}

// used by Berserk Pitcher
int targetberserkpotion(block_list * bl, va_list ap)
{
	struct map_session_data *sd = (struct map_session_data*)bl;
	if (pc_isdead(sd)) return 0;
	if (!ispartymember(sd)) return 0;
	if (sd->status.base_level >= 85) if (!sd->sc.data[SC_ASPDPOTION2]) { targetbl = bl; foundtargetID = sd->bl.id; return 1; };
	return 0;
}


bool hasgun(struct map_session_data *sd)
{
	if (sd->status.weapon == W_REVOLVER) { return true; }
	if (sd->status.weapon == W_RIFLE) { return true; }
	if (sd->status.weapon == W_GATLING) { return true; }
	if (sd->status.weapon == W_SHOTGUN) { return true; }
	if (sd->status.weapon == W_GRENADE) { return true; }
	return false;
}

void usehpitem(struct map_session_data *sd, int hppercentage)
{
	// Don't if you are going to throw potions
	if ((pc_checkskill(sd, AM_POTIONPITCHER) >= 5) && (sd->battle_status.sp>=50)) return;

	// Use potions if low on health?
	if ((status_get_hp(&sd->bl) < status_get_max_hp(&sd->bl) * hppercentage * 0.01) &&
		(!(sd->sc.data[SC_NORECOVER_STATE])) && (!(sd->sc.data[SC_BITESCAR]))
		)
	{
		//ShowError("Need to heal");

		unsigned short potions[] = {
			569,  // Novice Potion
			11567, // Novice Potion
			501,
			502,
			503,
			504,
			512,
			515,
			513, // Banana
			520,
			521,
			522, // Mastela Fruit			
			529, // Candy
			530, // Candy Cane
			538, // Cookie
			539 // Cake
		};

		int16 index = -1;
		int i;

		for (i = 0; i < ARRAYLENGTH(potions); i++) {
			if ((index = pc_search_inventory(sd, potions[i])) >= 0) {
				//ShowError("Found a potion to use");
				if (pc_isUseitem(sd, index)) {
					pc_useitem(sd, index);
					break;
				}
			}
		}

	}

}

int homu_skilluse_ifable(struct block_list *src, int target_id, uint16 skill_id, uint16 skill_lv)
{
	struct unit_data *ud;
	unsigned int tick = gettick();
	ud = unit_bl2ud(src);

	if (!ud)
		return 0;

	struct map_session_data *sd = (struct map_session_data*)src;

	struct homun_data *hd = (struct homun_data *)src;

	if (!hd)
		return 0;

	if (skill_get_sp(skill_id, skill_lv) >= hd->battle_status.sp) return 0;

	if (skill_isNotOk_hom(hd, skill_id, skill_lv)) {
		clif_emotion(&hd->bl, ET_THINK);
		return 0;
	}
	if (hd->bl.id != target_id && skill_get_inf(skill_id)&INF_SELF_SKILL)
		target_id = hd->bl.id;
	if (hd->ud.skilltimer != INVALID_TIMER) {
		if (skill_id != SA_CASTCANCEL && skill_id != SO_SPELLFIST) return 0;
	}
	else if (DIFF_TICK(tick, hd->ud.canact_tick) < 0) {
		clif_emotion(&hd->bl, ET_THINK);
		if (hd->master)
			clif_skill_fail(hd->master, skill_id, USESKILL_FAIL_SKILLINTERVAL, 0);
		return 0;
	}

	int lv = hom_checkskill(hd, skill_id);
	if (skill_lv > lv)
		skill_lv = lv;
	if (skill_lv)
		return unit_skilluse_id(&hd->bl, target_id, skill_id, skill_lv); else return 0;
}

// Homunculus autopilot timer
// @autopilot timer
TIMER_FUNC(unit_autopilot_homunculus_timer)
{
	struct block_list *bl;
	struct unit_data *ud;

	bl = map_id2bl(id);

	if (!bl)
		return 0;

	ud = unit_bl2ud(bl);

	if (!ud)
		return 0;

	struct map_session_data *sd = (struct map_session_data*)bl;

	struct homun_data *hd = (struct homun_data *)bl;

	if (hd->autopilotmode == 0) { return 0; }

	if (hd->battle_status.hp == 0) { return 0; }
	if (hd->homunculus.vaporize) { return 0; }

	int party_id, type = 0, i = 0;
	block_list * leaderbl;
	int leaderID, leaderdistance;
	struct map_session_data *leadersd;

	struct map_session_data *mastersd = unit_get_master(bl);

	party_id = mastersd->status.party_id;
	p = party_search(party_id);

	if (p) //Search leader
		for (i = 0; i < MAX_PARTY && !p->party.member[i].leader; i++);

	if (!p || i == MAX_PARTY) { //leader not found
		// Follow the master if there is no party
		leadersd = mastersd; leaderID = mastersd->bl.id; leaderbl = &mastersd->bl;
		leaderdistance = distance_bl(leaderbl, bl); 
	}
	else {
		targetthis = p->party.member[i].char_id;
		resettargets(); leaderdistance = 999; leaderID = -1;
		map_foreachinmap(targetthischar, sd->bl.m, BL_PC, sd);
		leaderID = foundtargetID;  
		if (leaderID > -1) { leaderbl = targetbl; leadersd = (struct map_session_data*)targetbl; leaderdistance = distance_bl(leaderbl, bl);
		}
		else {
			// Follow the master if party leader is absent
			leadersd = mastersd; leaderID = mastersd->bl.id; leaderbl = &mastersd->bl;
			leaderdistance = distance_bl(leaderbl, bl);
		}
	}

	getreachabletargets(sd);
	// leadersd is the person we position ourselves to : the party leader, or lacking one, the owner of the homunculus.

	// Support skills
	// Lif - Urgent Escape
	if (canskill(sd)) if (hom_checkskill(hd, HLIF_AVOID) > 0) if (leaderdistance <= 2) // seems to have limited range? Not sure how much?
		if (!(sd->sc.data[SC_AVOID])) {
			homu_skilluse_ifable(&sd->bl, SELF, HLIF_AVOID, hom_checkskill(hd, HLIF_AVOID));
		}
	// Amistr - Bulwark
	if (canskill(sd)) if (hom_checkskill(hd, HAMI_DEFENCE) > 0) if (leaderdistance <= 2)
		if (!(sd->sc.data[SC_DEFENCE])) {
			homu_skilluse_ifable(&sd->bl, SELF, HAMI_DEFENCE, hom_checkskill(hd, HAMI_DEFENCE));
		}
	// Amistr - Bloodlust
	// It won't be activated unless an enemy is nearby - due to the cooldown that would be wasteful.
	if (foundtargetID > -1) if (canskill(sd)) if (hom_checkskill(hd, HAMI_BLOODLUST) > 0)
		if (!(sd->sc.data[SC_BLOODLUST])) {
			homu_skilluse_ifable(&sd->bl, SELF, HAMI_BLOODLUST, hom_checkskill(hd, HAMI_BLOODLUST));
		}
	// Lif - Mental Change
	// **Note** : I modded this skill to not reduce hp/sp when it ends.
	// You might want to disable it for the AI and activate it manually instead.
	// Also it won't be activated unless an enemy is nearby - due to the cooldown that would be wasteful.
	if (foundtargetID>-1) if (canskill(sd)) if (hom_checkskill(hd, HLIF_CHANGE) > 0)
		if (!(sd->sc.data[SC_CHANGE])) {
			homu_skilluse_ifable(&sd->bl, SELF, HLIF_CHANGE, hom_checkskill(hd, HLIF_CHANGE));
		}
	// Filir Flitting
	if (foundtargetID > -1) if (canskill(sd)) if (hom_checkskill(hd, HFLI_FLEET) > 0)
		if (!(sd->sc.data[SC_FLEET])) {
			homu_skilluse_ifable(&sd->bl, SELF, HFLI_FLEET, hom_checkskill(hd, HFLI_FLEET));
		}
	// Filir Accelerated Flight
	if (foundtargetID > -1) if (canskill(sd)) if (hom_checkskill(hd, HFLI_SPEED) > 0)
		if (!(sd->sc.data[SC_SPEED])) {
			homu_skilluse_ifable(&sd->bl, SELF, HFLI_SPEED, hom_checkskill(hd, HFLI_SPEED));
		}

	// Lif - Healing Hands
	if (canskill(sd)) if (hom_checkskill(hd, HLIF_HEAL)>0)
		if ((leadersd->battle_status.hp < leadersd->battle_status.max_hp*0.4))
			if (pc_search_inventory(leadersd, 545) >= 0) {
				homu_skilluse_ifable(&sd->bl, leadersd->bl.id, HLIF_HEAL, hom_checkskill(hd, HLIF_HEAL));

	}
	// Attack skills
	// and other skills requiring an enemy target check
	resettargets();
	map_foreachinrange(targetnearest, &sd->bl, 9, BL_MOB, sd);
	// Vanil Caprice
	if (hd->autopilotmode!=3) if (canskill(sd))
		if (hom_checkskill(hd, HVAN_CAPRICE) > 0)
			homu_skilluse_ifable(&sd->bl, foundtargetID, HVAN_CAPRICE, hom_checkskill(hd, HVAN_CAPRICE));

	// Vanil Chaotic Blessings
	// No enemies nearby so heal is 50-50% to be self or owner
	// Must use at level 5 otherwise healing target selection is bad
	if (foundtargetID == -1) if (canskill(sd)) if (hom_checkskill(hd, HVAN_CHAOTIC) >= 5)
		// owner's hp low or own hp very low
		if ((leadersd->battle_status.hp < leadersd->battle_status.max_hp*0.5) ||
			(sd->battle_status.hp < sd->battle_status.max_hp*0.32)) {
				homu_skilluse_ifable(&sd->bl, leadersd->bl.id, HVAN_CHAOTIC, hom_checkskill(hd, HVAN_CHAOTIC));
			}


		// 1 - Tanking mode
	if (hd->autopilotmode == 1) {
		resettargets();
		// Target in leader's range, not ours to avoid going too far
		map_foreachinrange(targetnearestwalkto, leaderbl, AUTOPILOT_RANGE_CAP, BL_MOB, sd);

		if (foundtargetID > -1) {
			// Use normal melee attack
			if (targetdistance <= 1) {
				// Use tanking mode skills
				// Filir Moonlight
				if (canskill(sd)) if (hom_checkskill(hd, HFLI_MOON) > 0) homu_skilluse_ifable(&sd->bl, foundtargetID, HFLI_MOON, hom_checkskill(hd, HFLI_MOON));
				unit_attack(&sd->bl, foundtargetID, 1);
			}
			else
			{
				struct walkpath_data wpd1;
				if (path_search(&wpd1, sd->bl.m, bl->x, bl->y, targetbl->x, targetbl->y, 0, CELL_CHKNOPASS, MAX_WALKPATH))
					newwalk(&sd->bl, bl->x + dirx[wpd1.path[0]], bl->y + diry[wpd1.path[0]], 8);
				return 0;
			}
		}
		else {
			// If there is a leader and we haven't found a target in their area, stay near them.
			if ((leaderID > -1) && (leaderID != sd->bl.id)) {
				int tankdestinationx = leaderbl->x + 2 * dirx[leadersd->ud.dir];
				int tankdestinationy = leaderbl->y + 2 * diry[leadersd->ud.dir];
				if ((abs(tankdestinationx - sd->bl.x) >= 2) || (abs(tankdestinationy - sd->bl.y) >= 2)) {
					newwalk(&sd->bl, tankdestinationx + rand() % 3 - 1, tankdestinationy + rand() % 3 - 1, 8);
				}
			}
		}
	}
	// Not tanking mode
	else {
		int Dangerdistance = inDangerLeader(leadersd);

		// If party leader not under attack, get in range of 2
		if (Dangerdistance >= 900) {
			if ((abs(sd->bl.x - leaderbl->x) > 2) || abs(sd->bl.y - leaderbl->y) > 2) {
				if (!leadersd->ud.walktimer)
				newwalk(&sd->bl, leaderbl->x + rand() % 5 - 2, leaderbl->y + rand() % 5 - 2, 8);
				else newwalk(&sd->bl, leaderbl->x, leaderbl->y, 8);
				return 0;
			}
		}
		// but if they are under attack, as we are not in tanking mode, maintain a distance of 6 by taking only 1 step at a time closer
		else
		{
			// If either leader or nearest monster attacking them is not directly shootable, go closer
			// This is necessary to avoid the party stuck behind a corner, unable to attack 
			if ((abs(sd->bl.x - leaderbl->x) > 6) || (abs(sd->bl.y - leaderbl->y) > 6)
				|| !(path_search_long(NULL, leadersd->bl.m, bl->x, bl->y, leaderbl->x, leaderbl->y, CELL_CHKNOPASS,7))
				|| !(path_search_long(NULL, leadersd->bl.m, bl->x, bl->y, dangerbl->x, dangerbl->y, CELL_CHKNOPASS,7))
				) {

				struct walkpath_data wpd1;
				if (path_search(&wpd1, leadersd->bl.m, bl->x, bl->y, leaderbl->x, leaderbl->y, 0, CELL_CHKNOPASS))
					newwalk(&sd->bl, bl->x + dirx[wpd1.path[0]], bl->y + diry[wpd1.path[0]], 8);
				return 0;
			}

		}
	}
	return 0;
}


//===============================================================================
//===============================================================================
//===============================================================================
// Main autopilot function starts here
//===============================================================================
//===============================================================================
//===============================================================================

// @autopilot timer
TIMER_FUNC(unit_autopilot_timer)
//int unit_autopilot_timer(int tid, unsigned int tick, int id, intptr_t data)
{
	struct block_list *bl;
	struct unit_data *ud;

	bl = map_id2bl(id);

	if (!bl)
		return 0;

	ud = unit_bl2ud(bl);

	if (!ud)
		return 0;

	struct map_session_data *sd = (struct map_session_data*)bl;

	if (sd->state.autopilotmode == 0) {

		return 0;
	}

	if (bl->type != BL_PC) 
	{
		ShowError("Nonplayer set to autopilot!");
		return 0;
	}

	if (status_isdead(bl)) { return 0; }
	if pc_cant_act(sd) { return 0; }

	int party_id, type = 0, i = 0;
	block_list * leaderbl;
	int leaderID, leaderdistance;
	struct map_session_data *leadersd;
	int partycount = 1;

	party_id = sd->status.party_id;
	p = party_search(party_id);

	if (p) partycount = p->party.count;

	if (p) //Search leader
		for (i = 0; i < MAX_PARTY && !p->party.member[i].leader; i++);

	if (!p || i == MAX_PARTY) { //leader not found
		//ShowError("No party leader to follow!");
		leaderID = -1; leaderdistance = 0;
	}
	else {
		targetthis = p->party.member[i].char_id;
		resettargets(); leaderdistance = 999; leaderID = -1;
		map_foreachinmap(targetthischar, sd->bl.m, BL_PC, sd);
		leaderID = foundtargetID;  leaderbl = targetbl;
		leadersd = (struct map_session_data*)targetbl;
		if (leaderID > -1) { leaderdistance = distance_bl(leaderbl, bl); }
	}

	// Stand up if sitting and leader isn't
	if (leaderID>-1) if (!pc_issit(leadersd)) {
		if (pc_issit(sd) && pc_setstand(sd, false)) {
			skill_sit(sd, 0);
			clif_standing(&sd->bl);
		}

	}

	getreachabletargets(sd);

	// Find Warp to enter
	warpx = -9999; warpy = -9999;
	map_foreachinrange(warplocation, &sd->bl, MAX_WALKPATH, BL_PC, sd);
	if (warpx != -9999) {
		newwalk(&sd->bl, warpx, warpy, 0);
		return 0;
	}

	if pc_issit(sd) { return 0; }
	recoversp(sd, sd->state.autospgoal);
	sd->state.asurapreparation = false;

	// Say in party chat if something is wrong!
	if (sd->sc.data[SC_WEIGHT50]) {
		char* msg = "I can't carry all this by myself, please help!";
		saythis(sd, msg, 50);
	}
	else if (sd->battle_status.sp < 0.1*sd->battle_status.max_sp)
	{
		char* msg = "Please let me rest, I need SP!";
			saythis(sd, msg, 50);
	}

	if (pc_checkequip(sd,EQP_ARMOR)==-1)
	{
		char* msg = "Omg, I'm not wearing armor, that's dangerous!";
		saythis(sd, msg, 100);
	}

	if (sd->class_ != MAPID_TAEKWON)
		if (pc_checkequip(sd, EQP_HAND_R) == -1)
	{
		char* msg = "I need a weapon to fight!";
		saythis(sd, msg, 100);
	}

	if (sd->class_ != MAPID_TAEKWON)
	if (pc_checkequip(sd, EQP_HAND_L) == -1)
	{
		char* msg = "Using a shield might be a good idea?";
		saythis(sd, msg, 100);
	}

	int Dangerdistance = inDanger(sd);

	usehpitem(sd, 50);

	/////////////////////////////////////////////////////////////////////////////////////
	// Skills that aren't tanking mode exclusive (nonmelee skills generally)
	/////////////////////////////////////////////////////////////////////////////////////
		/// Acid Demonstration
		if (canskill(sd)) if (pc_checkskill(sd, CR_ACIDDEMONSTRATION)>0) if (sd->state.autopilotmode == 2) {
			resettargets2();
			map_foreachinrange(asuratarget, &sd->bl, 12, BL_MOB, sd);
			if (!targetmd->sc.data[SC_PNEUMA])
				if (foundtargetID > -1) {
					unit_skilluse_ifable(&sd->bl, foundtargetID, CR_ACIDDEMONSTRATION, pc_checkskill(sd, CR_ACIDDEMONSTRATION));
				}
		}
		/// Asura Strike
		if (canskill(sd)) if (pc_checkskill(sd, MO_EXTREMITYFIST)>0) if (sd->state.autopilotmode == 2) {
			resettargets2();
			map_foreachinrange(asuratarget, &sd->bl, 12, BL_MOB, sd);
			if (foundtargetID > -1) {
			// if target exists, check for Spheres, then Fury, then SP, then use
				if (sd->spiritball<5) {
					// Dangerous Soul Collect (Zen)
					if (pc_checkskill(sd, CH_SOULCOLLECT) > 0) {
						int radra = 0; if (sd->sc.data[SC_RAISINGDRAGON]) { radra = sd->sc.data[SC_RAISINGDRAGON]->val1; }
						if (4 + radra>sd->spiritball) {
							unit_skilluse_ifable(&sd->bl, SELF, CH_SOULCOLLECT, pc_checkskill(sd, CH_SOULCOLLECT));
						}
					}

					// Summon Spirit Sphere
					if (pc_checkskill(sd, MO_CALLSPIRITS) > 0) {
						int radra = 0; if (sd->sc.data[SC_RAISINGDRAGON]) { radra = sd->sc.data[SC_RAISINGDRAGON]->val1; }
						if (pc_checkskill(sd, MO_CALLSPIRITS) + radra>sd->spiritball) {
							unit_skilluse_ifable(&sd->bl, SELF, MO_CALLSPIRITS, pc_checkskill(sd, MO_CALLSPIRITS));
						}
					}
				}
				else {
					// Fury
					if (((pc_checkskill(sd, MO_EXPLOSIONSPIRITS) > 0)) && (!(sd->sc.data[SC_EXPLOSIONSPIRITS]))) {
						if ((sd->spiritball >= 5)) {
							unit_skilluse_ifable(&sd->bl, SELF, MO_EXPLOSIONSPIRITS, pc_checkskill(sd, MO_EXPLOSIONSPIRITS));
						}
					}
					else {
						// At least 80% SP required to use. Important : this amount should be no more than the threshhold for using Soul Exchange
						// Note : If SP items are not available and SP isn't restored by Professor, character will be stuck doing nothing.
						if (sd->battle_status.sp < 0.8*sd->battle_status.max_sp) { recoversp(sd, 100); sd->state.asurapreparation = true; return 0; }
						// Walk near mvp only if SP already available
						if (distance_bl(bl, targetbl)> 1) { unit_walktoxy(bl, targetbl->x, targetbl->y, 8); return 0; }
						unit_skilluse_ifable(&sd->bl, foundtargetID, MO_EXTREMITYFIST, pc_checkskill(sd, MO_EXTREMITYFIST));
					}
				}
			}
		}

		// LEX ATHENA
		if (canskill(sd)) if (pc_checkskill(sd, PR_LEXAETERNA) > 0) if (p) {

			int lextarget = -1;
			int i;
			unit_data * lud;
			for (i = 0; i < MAX_PARTY; i++) {
				struct map_session_data * psd = p->data[i].sd;
				if (psd) {
					lud = unit_bl2ud(&psd->bl);
					struct block_list *lbl;

					lbl = map_id2bl(lud->skilltarget);
					struct map_session_data *lsd = (struct map_session_data*)lbl;
					if (!((lsd) && (lsd->sc.data[SC_AETERNA])))
					{
						if (lud->skill_id == MO_EXTREMITYFIST) lextarget = lud->skilltarget; 
						if (lud->skill_id == CR_ACIDDEMONSTRATION) lextarget = lud->skilltarget;
					}
				}
			}
			if (lextarget > -1){
				unit_skilluse_ifable(&sd->bl, lextarget, PR_LEXAETERNA, pc_checkskill(sd, PR_LEXAETERNA));
			}

		}

		bool havepriest = false;
		int64 partymagicratio = 0;
		if (p) for (i = 0; i < MAX_PARTY; i++) if (p->data[i].sd) if (!status_isdead(&p->data[i].sd->bl)) {
			if (pc_checkskill(p->data[i].sd, ALL_RESURRECTION) >= 4) havepriest = true;
			if (p->data[i].sd->state.autopilotmode != 3) { // add matk, subtract atk. Might not be exact but should give a rough impression on which type of damage the party relies on most. 
				partymagicratio += p->data[i].sd->battle_status.matk_min
					- p->data[i].sd->battle_status.rhw.atk - p->data[i].sd->battle_status.batk;
			}
		}
		// Final Strike
		// base damage = currenthp + ((atk * currenthp * skill level) / maxhp)
		// final damage = base damage + ((mirror image count + 1) / 5 * base damage) - (edef + sdef)

		if (pc_checkskill(sd, NJ_ISSEN) >= 10) // Skill level must be maxed
		if ((pc_search_inventory(sd, 7524) >= 0) || (sd->sc.data[SC_BUNSINJYUTSU])) // requires Shadow Orb for Mirror Image
		if (sd->state.autopilotmode == 2) {

			resettargets2();
			targetdistance = 99999999;
			map_foreachinrange(finaltarget, &sd->bl, 12, BL_MOB, sd);
			if (foundtargetID > -1) 
			{
					if (havepriest) {
					// Ninja Aura to enable Mirror Image
					if (pc_checkskill(sd, NJ_NEN) > 0)
						if (pc_checkskill(sd, NJ_BUNSINJYUTSU) > 0)
								if (!(sd->sc.data[SC_NEN])) {
									unit_skilluse_ifable(&sd->bl, SELF, NJ_NEN, pc_checkskill(sd, NJ_NEN));
							}

					// Mirror Image
					// Use this for tanking instead of cicada because no backwards movement.
					// Interruptable though so need Phen or equipvalent while actually tanking a monster.
					if (pc_checkskill(sd, NJ_BUNSINJYUTSU) > 0)
						
								if (!(sd->sc.data[SC_BUNSINJYUTSU])) {
									unit_skilluse_ifable(&sd->bl, SELF, NJ_BUNSINJYUTSU, pc_checkskill(sd, NJ_BUNSINJYUTSU));
								}

					// We have both buffs, ready to strike but need high hp!
					if ((sd->sc.data[SC_BUNSINJYUTSU]) && (sd->sc.data[SC_NEN])) {
						usehpitem(sd, 91);
						if (status_get_hp(&sd->bl) >= status_get_max_hp(&sd->bl) * 0.91) {
							if (distance_bl(bl, targetbl) > 5) { unit_walktoxy(bl, targetbl->x, targetbl->y, 8); return 0; }
							unit_skilluse_ifable(&sd->bl, foundtargetID, NJ_ISSEN, pc_checkskill(sd, NJ_ISSEN));
						}
					}
				}
			}

		}

		/// Dispell
		if (canskill(sd)) if ((pc_checkskill(sd, SA_DISPELL)>0) && (pc_search_inventory(sd, 715)>=0)) {
			resettargets();
			map_foreachinrange(targetdispel, &sd->bl, 9, BL_MOB, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, foundtargetID, SA_DISPELL, pc_checkskill(sd, SA_DISPELL));
			}
		}

		/// Dispell friendly
		if (canskill(sd)) if ((pc_checkskill(sd, SA_DISPELL) > 0) && (pc_search_inventory(sd, 715) >= 0)) {
			resettargets();
			map_foreachinrange(targetdispel2, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, foundtargetID, SA_DISPELL, pc_checkskill(sd, SA_DISPELL));
			}
		}

		// Indulge
		// Use if below 80% SP and above 60% HP. We want as close to max SP as possible for soul exchanges
		if (canskill(sd)) if (pc_checkskill(sd, PF_HPCONVERSION)>0) 
		if (sd->battle_status.sp<sd->battle_status.max_sp*0.8) if
			(sd->battle_status.hp>sd->battle_status.max_hp*0.6) {
			unit_skilluse_ifable(&sd->bl, SELF, PF_HPCONVERSION, pc_checkskill(sd, PF_HPCONVERSION));
		}
		// Soul Exchange
		if (canskill(sd)) if ((pc_checkskill(sd, PF_SOULCHANGE)>0)) {
			resettargets2(); 
			map_foreachinrange(targetsoulexchange, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, foundtargetID, PF_SOULCHANGE, pc_checkskill(sd, PF_SOULCHANGE));
			}
		}
		/// Potion Pitcher Blue
		if (canskill(sd)) if (pc_checkskill(sd, AM_POTIONPITCHER) >= 5) {
			resettargets();
			map_foreachinrange(targetbluepitcher, &sd->bl, 9, BL_PC, sd);
			// HP must be below 40% to ensure we don't waste items when other ways to heal are available
			if (foundtargetID > -1) {
				if (pc_search_inventory(sd, 504) >= 0)	unit_skilluse_ifable(&sd->bl, foundtargetID, AM_POTIONPITCHER, 5);
			}
		}

		/// Pneuma
		if (canskill(sd)) if  (pc_checkskill(sd, AL_PNEUMA)>0) {
			resettargets();
			map_foreachinrange(targetpneuma, &sd->bl, 12, BL_MOB, sd);
			if (foundtargetID > -1) {
				// Not if pneuma already exists on target and also not if safety wall exists, they are mutually exclusive
				struct status_change *sc;
				sc = status_get_sc(targetbl);
				if (!(sc->data[SC_PNEUMA]) && !(sc->data[SC_SAFETYWALL])) { 
					unit_skilluse_ifablexy(&sd->bl, foundtargetID, AL_PNEUMA, pc_checkskill(sd, AL_PNEUMA)); }
			} 
		}
		/// Flip Tatami
		// **Note** : I modded this skill to have a skill reuse cooldown instead of global delay.
		// Without that mod it's better to remove this and never allow the AI to use it.
		if (canskill(sd)) if (pc_checkskill(sd, NJ_TATAMIGAESHI) > 0) {
			struct status_change *sc;
			sc = status_get_sc(&sd->bl);
			if (!(sc->data[SC_PNEUMA]) && !(sc->data[SC_TATAMIGAESHI])) {
				resettargets();
				map_foreachinrange(targetpneuma, &sd->bl, 12, BL_MOB, sd);
				if (foundtargetID == sd->bl.id) {
					unit_skilluse_ifable(&sd->bl, SELF, NJ_TATAMIGAESHI, pc_checkskill(sd, NJ_TATAMIGAESHI));
				}
			}
		}

		/// Redemptio
		if (canskill(sd)) if (pc_checkskill(sd, PR_REDEMPTIO)>0) {
			resettargets();
			if (map_foreachinrange(targetresu, &sd->bl, 6, BL_PC, sd)>=4)	{
				if (!duplicateskill(p, PR_REDEMPTIO)) unit_skilluse_ifable(&sd->bl, foundtargetID, PR_REDEMPTIO, pc_checkskill(sd, PR_REDEMPTIO));
			}
		}

		/// Epiclesis
		if (canskill(sd)) if (pc_checkskill(sd, AB_EPICLESIS) > 0) if ((Dangerdistance > 900) || (sd->special_state.no_castcancel))
			if (pc_inventory_count(sd, 12333) > 0)
				if (pc_inventory_count(sd, 523) > 0) {
					int epictargetid = -1;
					int tid2;
					int j;
					if (p) //Search leader
						for (j = 0; j < MAX_PARTY; j++) {

							resettargets();
							targetbl = map_id2bl(p->party.member[j].account_id);
							tid2 = foundtargetID;
							if (targetbl) if (distance_bl(bl, targetbl) < 9) {
								resettargets();
								if (map_foreachinrange(epiclesispriority, &sd->bl, 6, BL_PC, sd) >= 8)
									epictargetid = tid2;
							}
						}
				if (epictargetid>0)	unit_skilluse_ifablexy(&sd->bl, epictargetid, AB_EPICLESIS, pc_checkskill(sd, AB_EPICLESIS));
				}

		/// If Resurrection known, warn when low on gems!
		if ((pc_checkskill(sd, ALL_RESURRECTION)>0)) if (pc_inventory_count(sd, ITEMID_BLUE_GEMSTONE) < 8)
			saythis(sd, "I'm low on Blue Gemstones!", 600); // Once per minute
		/// Resurrection
		if (canskill(sd)) if ((pc_checkskill(sd, ALL_RESURRECTION)>0)) {
			resettargets();
			map_foreachinrange(targetresu, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				if (pc_search_inventory(sd, ITEMID_BLUE_GEMSTONE) >= 0) {
					unit_skilluse_ifable(&sd->bl, foundtargetID, ALL_RESURRECTION, pc_checkskill(sd, ALL_RESURRECTION));
				}
				else saythis(sd, "I'm out of Blue Gemstones!",5); // Twice per second
			}
		}

		/// Coluceo Heal
		if (canskill(sd)) if ((pc_checkskill(sd, AB_CHEAL) > 0) && ((Dangerdistance > 900) || (sd->special_state.no_castcancel))) {
			resettargets();
			// is a waste to cast on fewer than 4 people
			if (map_foreachinrange(targethealing, &sd->bl, 7, BL_PC, sd) >= 4) {
				unit_skilluse_ifable(&sd->bl, SELF, AB_CHEAL, pc_checkskill(sd, AB_CHEAL));
			}
		}

		/// Highness Heal
		if (canskill(sd)) if (pc_checkskill(sd, AB_HIGHNESSHEAL) > 0) {
			resettargets();
			map_foreachinrange(targethealing, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, foundtargetID, AB_HIGHNESSHEAL, pc_checkskill(sd, AB_HIGHNESSHEAL));
			}
		}

		/// Heal
		if (canskill(sd)) if (pc_checkskill(sd, AL_HEAL)>0) {
			resettargets();
			map_foreachinrange(targethealing, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, foundtargetID, AL_HEAL, pc_checkskill(sd, AL_HEAL));
			}
		}
		/// Slim Potion Pitcher
		// Note : used as if it was single target, wasteful. This should be improved!
		if (canskill(sd)) if (pc_checkskill(sd, CR_SLIMPITCHER) >=10) {
			resettargets();
			map_foreachinrange(targethealing, &sd->bl, 9, BL_PC, sd);
			// HP must be below 40% to ensure we don't waste items when other ways to heal are available
			if (foundtargetID > -1) if (targetdistance<40) {
				if (pc_search_inventory(sd, 547) >= 0)	unit_skilluse_ifablexy(&sd->bl, foundtargetID, CR_SLIMPITCHER, 10); else
					if (pc_search_inventory(sd, 546) >= 0)	unit_skilluse_ifablexy(&sd->bl, foundtargetID, CR_SLIMPITCHER, 9); else
						if (pc_search_inventory(sd, 545) >= 0)	unit_skilluse_ifablexy(&sd->bl, foundtargetID, CR_SLIMPITCHER, 5); 
			}
		}
		/// Potion Pitcher
		if (canskill(sd)) if (pc_checkskill(sd, AM_POTIONPITCHER)>=4) {
			resettargets();
			map_foreachinrange(targethealing, &sd->bl, 9, BL_PC, sd);
			// HP must be below 40% to ensure we don't waste items when other ways to heal are available
			if (foundtargetID > -1) if (targetdistance<40) {
				if (pc_search_inventory(sd, 504) >= 0)	unit_skilluse_ifable(&sd->bl, foundtargetID, AM_POTIONPITCHER, 4); else
				if (pc_search_inventory(sd, 503) >= 0)	unit_skilluse_ifable(&sd->bl, foundtargetID, AM_POTIONPITCHER, 3); else
				if (pc_search_inventory(sd, 502) >= 0)	unit_skilluse_ifable(&sd->bl, foundtargetID, AM_POTIONPITCHER, 2); else
				if (pc_search_inventory(sd, 501) >= 0)	unit_skilluse_ifable(&sd->bl, foundtargetID, AM_POTIONPITCHER, 1); 
			}
		}
		/// Status Recovery
		if (canskill(sd)) if (pc_checkskill(sd, PR_STRECOVERY)>0) {
			resettargets();
			map_foreachinrange(targetstatusrecovery, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, foundtargetID, PR_STRECOVERY, pc_checkskill(sd, PR_STRECOVERY));
			}
		}
		/// LEX DIVINA to remove silence
		if (canskill(sd)) if (pc_checkskill(sd, PR_LEXDIVINA)>0) {
			resettargets();
			map_foreachinrange(targetlexdivina, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				if (!duplicateskill(p, PR_LEXDIVINA)) unit_skilluse_ifable(&sd->bl, foundtargetID, PR_LEXDIVINA, pc_checkskill(sd, PR_LEXDIVINA));
			}
		}
		/// Cure
		if (canskill(sd)) if (pc_checkskill(sd, AL_CURE)>0) {
			resettargets();
			map_foreachinrange(targetCure, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, foundtargetID, AL_CURE, pc_checkskill(sd, AL_CURE));
			}
		}
		/// Detoxify
		if (canskill(sd)) if (pc_checkskill(sd, TF_DETOXIFY)>0) {
			resettargets();
			map_foreachinrange(targetDetoxify, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, foundtargetID, TF_DETOXIFY, pc_checkskill(sd, TF_DETOXIFY));
			}
		}
		/// Slow Poison
		if (canskill(sd)) if (pc_checkskill(sd, PR_SLOWPOISON)>0) {
			resettargets();
			map_foreachinrange(targetSlowPoison, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, foundtargetID, PR_SLOWPOISON, pc_checkskill(sd, PR_SLOWPOISON));
			}
		}

		/// MAGNIFICAT
		if (canskill(sd)) if ((pc_checkskill(sd, PR_MAGNIFICAT)>0) && ((Dangerdistance >900) || (sd->special_state.no_castcancel))) {
			resettargets();
			map_foreachinrange(targetmagnificat, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				if (!duplicateskill(p, PR_MAGNIFICAT)) unit_skilluse_ifable(&sd->bl, SELF, PR_MAGNIFICAT, pc_checkskill(sd, PR_MAGNIFICAT));
			}
		}

		/// Renovatio
		if (canskill(sd)) if ((pc_checkskill(sd, AB_RENOVATIO) > 0) && ((Dangerdistance > 900) || (sd->special_state.no_castcancel))) {
			resettargets();
			map_foreachinrange(targetrenovatio, &sd->bl, 11, BL_PC, sd);
			if (foundtargetID > -1) {
				if (!duplicateskill(p, AB_RENOVATIO)) unit_skilluse_ifable(&sd->bl, SELF, AB_RENOVATIO, pc_checkskill(sd, AB_RENOVATIO));
			}
		}

		// Cicada Skin Shedding
		if (pc_checkskill(sd, NJ_UTSUSEMI) > 0) if (sd->state.autopilotmode != 1) { // Do not use in tanking mode, due to moving back it's bad
			if (!(sd->sc.data[SC_UTSUSEMI])) {
				unit_skilluse_ifable(&sd->bl, SELF, NJ_UTSUSEMI, pc_checkskill(sd, NJ_UTSUSEMI));
			}
		}

		// Star Gladiator Protections
		if (pc_checkskill(sd, SG_SUN_COMFORT) > 0) {
			i = SG_SUN_COMFORT - SG_SUN_COMFORT;
			if ((sd->bl.m == sd->feel_map[i].m) || (sd->sc.data[SC_MIRACLE])) 
				if (!(sd->sc.data[SC_SUN_COMFORT])) {
					unit_skilluse_ifable(&sd->bl, SELF, SG_SUN_COMFORT, pc_checkskill(sd, SG_SUN_COMFORT));
				}
		}
		if (pc_checkskill(sd, SG_MOON_COMFORT) > 0) {
			i = SG_MOON_COMFORT - SG_SUN_COMFORT;
			if ((sd->bl.m == sd->feel_map[i].m) || (sd->sc.data[SC_MIRACLE]))
				if (!(sd->sc.data[SC_MOON_COMFORT])) {
					unit_skilluse_ifable(&sd->bl, SELF, SG_MOON_COMFORT, pc_checkskill(sd, SG_MOON_COMFORT));
				}
		}
		if (pc_checkskill(sd, SG_STAR_COMFORT) > 0) {
			i = SG_STAR_COMFORT - SG_SUN_COMFORT;
			if ((sd->bl.m == sd->feel_map[i].m) || (sd->sc.data[SC_MIRACLE]))
				if (!(sd->sc.data[SC_STAR_COMFORT])) {
					unit_skilluse_ifable(&sd->bl, SELF, SG_STAR_COMFORT, pc_checkskill(sd, SG_STAR_COMFORT));
				}
		}

		// Tumbling
		if (pc_checkskill(sd, TK_DODGE) > 0) { // Do not use in tanking mode, due to moving back it's bad
			if (!(sd->sc.data[SC_DODGE])) {
				unit_skilluse_ifable(&sd->bl, SELF, TK_DODGE, pc_checkskill(sd, TK_DODGE));
			}
		}

		// Taekwon Stances
		if ((sd->class_ & MAPID_UPPERMASK) != MAPID_SOUL_LINKER) {
			if (pc_checkskill(sd, TK_READYSTORM) > 0) {
				if (!(sd->sc.data[SC_READYSTORM]))
				{
					unit_skilluse_ifable(&sd->bl, SELF, TK_READYSTORM, pc_checkskill(sd, TK_READYSTORM));
				}
			}

			if (pc_checkskill(sd, TK_READYDOWN) > 0) {
				if (!(sd->sc.data[SC_READYDOWN]))
				{
					unit_skilluse_ifable(&sd->bl, SELF, TK_READYDOWN, pc_checkskill(sd, TK_READYDOWN));
				}
			}

			if (pc_checkskill(sd, TK_READYTURN) > 0) {
				if (!(sd->sc.data[SC_READYTURN]))
				{
					unit_skilluse_ifable(&sd->bl, SELF, TK_READYTURN, pc_checkskill(sd, TK_READYTURN));
				}
			}

			if (pc_checkskill(sd, TK_READYCOUNTER) > 0) {
				if (!(sd->sc.data[SC_READYCOUNTER]))
				{
					unit_skilluse_ifable(&sd->bl, SELF, TK_READYCOUNTER, pc_checkskill(sd, TK_READYCOUNTER));
				}
			}

		}

		/// Angelus
		if (canskill(sd)) if (pc_checkskill(sd, AL_ANGELUS)>0) {
			resettargets();
			map_foreachinrange(targetangelus, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, SELF, AL_ANGELUS, pc_checkskill(sd, AL_ANGELUS));
			}
		}
		/// Advanced Adrenaline Rush
		if (canskill(sd)) if (pc_checkskill(sd, BS_ADRENALINE2)>0) {
			resettargets();
			map_foreachinrange(targetadrenaline2, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, SELF, BS_ADRENALINE2, pc_checkskill(sd, BS_ADRENALINE2));
			}
		}
		/// Adrenaline Rush
		if (canskill(sd)) if (pc_checkskill(sd, BS_ADRENALINE)>0) {
			resettargets();
			map_foreachinrange(targetadrenaline, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, SELF, BS_ADRENALINE, pc_checkskill(sd, BS_ADRENALINE));
			}
		}
		/// Weapon Perfection
		if (canskill(sd)) if (pc_checkskill(sd, BS_WEAPONPERFECT)>0) {
			resettargets();
			map_foreachinrange(targetwperfect, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, SELF, BS_WEAPONPERFECT, pc_checkskill(sd, BS_WEAPONPERFECT));
			}
		}
		/// Over Thrust
		if (canskill(sd)) if (pc_checkskill(sd, BS_OVERTHRUST)>0) {
			resettargets();
			map_foreachinrange(targetovert, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, SELF, BS_OVERTHRUST, pc_checkskill(sd, BS_OVERTHRUST));
			}
		}

		/// Wind Walking
		if (canskill(sd)) if (pc_checkskill(sd, SN_WINDWALK) > 0) {
			resettargets();
			map_foreachinrange(targetwindwalk, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, SELF, SN_WINDWALK, pc_checkskill(sd, SN_WINDWALK));
			}
		}
		/// Canto Candidus
		if (canskill(sd)) if (pc_checkskill(sd, AB_CANTO) > 0) {
			resettargets();
			if (map_foreachinrange(targetincagi, &sd->bl, 9, BL_PC, sd) >= 4) {
				if (!duplicateskill(p, AB_CANTO)) if (!duplicateskill(p, AL_INCAGI)) unit_skilluse_ifable(&sd->bl, SELF, AB_CANTO, pc_checkskill(sd, AB_CANTO));
			}
		}

		/// Inc Agi
		if (canskill(sd)) if (pc_checkskill(sd, AL_INCAGI)>0) {
			resettargets();
			map_foreachinrange(targetincagi, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				if (!duplicateskill(p, AL_INCAGI)) unit_skilluse_ifable(&sd->bl, foundtargetID, AL_INCAGI, pc_checkskill(sd, AL_INCAGI));
			}
		}
		/// Clementia
		if (canskill(sd)) if (pc_checkskill(sd, AB_CLEMENTIA) > 0) {
			resettargets();
			if (map_foreachinrange(targetbless, &sd->bl, 9, BL_PC, sd) >= 4) {
				if (!duplicateskill(p, AB_CLEMENTIA)) unit_skilluse_ifable(&sd->bl, foundtargetID, AB_CLEMENTIA, pc_checkskill(sd, AB_CLEMENTIA));
			}
		}
		/// Blessing
		if (canskill(sd)) if (pc_checkskill(sd, AL_BLESSING)>0) {
			resettargets();
			map_foreachinrange(targetbless, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, foundtargetID, AL_BLESSING, pc_checkskill(sd, AL_BLESSING));
			}
		}
		/// Berserk Pitcher
		if (canskill(sd)) if (pc_checkskill(sd, AM_BERSERKPITCHER) > 0) if (pc_inventory_count(sd, 657)>=2) {
			resettargets();
			map_foreachinrange(targetberserkpotion, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, foundtargetID, AM_BERSERKPITCHER, pc_checkskill(sd, AM_BERSERKPITCHER));
			}
		}
		/// Soul Link
		if (canskill(sd)) if ((sd->class_ & MAPID_UPPERMASK)== MAPID_SOUL_LINKER) {
			resettargets();
			map_foreachinrange(targetlinks, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, foundtargetID, targetsoullink, pc_checkskill(sd, targetsoullink));
			}
		}

		/// Kaizel
		if (canskill(sd)) if (pc_checkskill(sd, SL_KAIZEL) > 0) {
			resettargets();
			map_foreachinrange(targetkaizel, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, foundtargetID, SL_KAIZEL, pc_checkskill(sd, SL_KAIZEL));
			}
		}

		/// Kaahi
		if (canskill(sd)) if (pc_checkskill(sd, SL_KAAHI) > 0) {
			resettargets();
			map_foreachinrange(targetkaahi, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, foundtargetID, SL_KAAHI, pc_checkskill(sd, SL_KAAHI));
			}
		}

		/// Kaupe
		if (canskill(sd)) if (pc_checkskill(sd, SL_KAUPE) > 0) {
			resettargets();
			map_foreachinrange(targetkaupe, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, foundtargetID, SL_KAUPE, pc_checkskill(sd, SL_KAUPE));
			}
		}


		/// Aspersio
		if (canskill(sd)) if (pc_checkskill(sd, PR_ASPERSIO)>0) if (pc_search_inventory(sd, ITEMID_HOLY_WATER)>=0) {
			resettargets();
			if (map_foreachinmap(endowneed, sd->bl.m, BL_MOB, ELE_HOLY) > 0){
				resettargets();
				map_foreachinrange(targetendow, &sd->bl, 9, BL_PC, sd);
				if (foundtargetID > -1) {
					unit_skilluse_ifable(&sd->bl, foundtargetID, PR_ASPERSIO, pc_checkskill(sd, PR_ASPERSIO));
				}
			}
		}
		/// Fire weapon
		if (canskill(sd)) if (pc_checkskill(sd, SA_FLAMELAUNCHER)>0) if (pc_search_inventory(sd,990)>=0) {
			resettargets();
			if (map_foreachinmap(endowneed, sd->bl.m, BL_MOB, ELE_FIRE) > 0){
				resettargets();
				map_foreachinrange(targetendow, &sd->bl, 9, BL_PC, sd);
				if (foundtargetID > -1) {
					unit_skilluse_ifable(&sd->bl, foundtargetID, SA_FLAMELAUNCHER, pc_checkskill(sd, SA_FLAMELAUNCHER));
				}
			}
		}
		/// Ice weapon
		if (canskill(sd)) if (pc_checkskill(sd, SA_FROSTWEAPON)>0) if (pc_search_inventory(sd, 991 )>=0) {
			resettargets();
			if (map_foreachinmap(endowneed, sd->bl.m, BL_MOB, ELE_WATER) > 0){
				resettargets();
				map_foreachinrange(targetendow, &sd->bl, 9, BL_PC, sd);
				if (foundtargetID > -1) {
					unit_skilluse_ifable(&sd->bl, foundtargetID, SA_FROSTWEAPON, pc_checkskill(sd, SA_FROSTWEAPON));
				}
			}
		}
		///wind weapon
		if (canskill(sd)) if (pc_checkskill(sd, SA_LIGHTNINGLOADER)>0) if (pc_search_inventory(sd,992)>=0) {
			resettargets();
			if (map_foreachinmap(endowneed, sd->bl.m, BL_MOB, ELE_WIND) > 0){
				resettargets();
				map_foreachinrange(targetendow, &sd->bl, 9, BL_PC, sd);
				if (foundtargetID > -1) {
					unit_skilluse_ifable(&sd->bl, foundtargetID, SA_LIGHTNINGLOADER, pc_checkskill(sd, SA_LIGHTNINGLOADER));
				}
			}
		}
		/// Earth weapon
		if (canskill(sd)) if (pc_checkskill(sd, SA_SEISMICWEAPON)>0) if (pc_search_inventory(sd, 993)>=0) {
			resettargets();
			if (map_foreachinmap(endowneed, sd->bl.m, BL_MOB, ELE_EARTH) > 0){
				resettargets();
				map_foreachinrange(targetendow, &sd->bl, 9, BL_PC, sd);
				if (foundtargetID > -1) {
					unit_skilluse_ifable(&sd->bl, foundtargetID, SA_SEISMICWEAPON, pc_checkskill(sd, SA_SEISMICWEAPON));
				}
			}
		}
		/// Enchant Poison
		if (canskill(sd)) if (pc_checkskill(sd, AS_ENCHANTPOISON) > 0)  {
			resettargets();
			if (map_foreachinmap(endowneed, sd->bl.m, BL_MOB, ELE_POISON) > 0){
				resettargets();
				map_foreachinrange(targetendow, &sd->bl, 9, BL_PC, sd);
				if (foundtargetID > -1) {
					unit_skilluse_ifable(&sd->bl, foundtargetID, AS_ENCHANTPOISON, pc_checkskill(sd, AS_ENCHANTPOISON));
				}
			}
		}
		//
		// Mild Wind
		//
		if (canskill(sd)) if (pc_checkskill(sd, TK_SEVENWIND) > 0) {
			resettargets();
			if (map_foreachinmap(endowneed, sd->bl.m, BL_MOB, ELE_EARTH) > 0) if (canendow(sd)) {
			unit_skilluse_ifable(&sd->bl, SELF, TK_SEVENWIND, 1);
			}
		}
		if (canskill(sd)) if (pc_checkskill(sd, TK_SEVENWIND) > 1) {
			resettargets();
			if (map_foreachinmap(endowneed, sd->bl.m, BL_MOB, ELE_WIND) > 0) if (canendow(sd)) {
				unit_skilluse_ifable(&sd->bl, SELF, TK_SEVENWIND, 2);
			}
		}
		if (canskill(sd)) if (pc_checkskill(sd, TK_SEVENWIND) > 2) {
			resettargets();
			if (map_foreachinmap(endowneed, sd->bl.m, BL_MOB, ELE_WATER) > 0) if (canendow(sd)) {
				unit_skilluse_ifable(&sd->bl, SELF, TK_SEVENWIND, 3);
			}
		}
		if (canskill(sd)) if (pc_checkskill(sd, TK_SEVENWIND) > 3) {
			resettargets();
			if (map_foreachinmap(endowneed, sd->bl.m, BL_MOB, ELE_FIRE) > 0) if (canendow(sd)) {
				unit_skilluse_ifable(&sd->bl, SELF, TK_SEVENWIND, 4);
			}
		}
		if (canskill(sd)) if (pc_checkskill(sd, TK_SEVENWIND) > 4) {
			resettargets();
			if (map_foreachinmap(endowneed, sd->bl.m, BL_MOB, ELE_GHOST) > 0) if (canendow(sd)) {
				unit_skilluse_ifable(&sd->bl, SELF, TK_SEVENWIND, 5);
			}
		}
		if (canskill(sd)) if (pc_checkskill(sd, TK_SEVENWIND) > 5) {
			resettargets();
			if (map_foreachinmap(endowneed, sd->bl.m, BL_MOB, ELE_DARK) > 0) if (canendow(sd)) {
				unit_skilluse_ifable(&sd->bl, SELF, TK_SEVENWIND, 6);
			}
		}
		if (canskill(sd)) if (pc_checkskill(sd, TK_SEVENWIND) > 6) {
			resettargets();
			if (map_foreachinmap(endowneed, sd->bl.m, BL_MOB, ELE_HOLY) > 0) if (canendow(sd)) {
				unit_skilluse_ifable(&sd->bl, SELF, TK_SEVENWIND, 7);
			}
		}

		/// Assumptio
		if (canskill(sd)) if ((pc_checkskill(sd, HP_ASSUMPTIO)>0) && ((Dangerdistance >900) || (sd->special_state.no_castcancel))) {
			resettargets();
			map_foreachinrange(targetassumptio, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				if (!duplicateskill(p, HP_ASSUMPTIO)) unit_skilluse_ifable(&sd->bl, foundtargetID, HP_ASSUMPTIO, pc_checkskill(sd, HP_ASSUMPTIO));
			}
		}
		/// Praefatio
		if (canskill(sd)) if ((pc_checkskill(sd, AB_PRAEFATIO) > 0) && ((Dangerdistance > 900) || (sd->special_state.no_castcancel))) {
			resettargets();
			if (map_foreachinrange(targetkyrie, &sd->bl, 9, BL_PC, sd) >=4 ) {
				if (!duplicateskill(p, AB_PRAEFATIO)) unit_skilluse_ifable(&sd->bl, SELF, AB_PRAEFATIO, pc_checkskill(sd, AB_PRAEFATIO));
			}
		}


		/// Kyrie Elison
		if (canskill(sd)) if ((pc_checkskill(sd, PR_KYRIE)>0) && ((Dangerdistance >900) || (sd->special_state.no_castcancel))) {
			resettargets();
			map_foreachinrange(targetkyrie, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				if (!duplicateskill(p, PR_KYRIE)) unit_skilluse_ifable(&sd->bl, foundtargetID, PR_KYRIE, pc_checkskill(sd, PR_KYRIE));
			}
		}
		/// Lauda Agnus
		if (canskill(sd)) if (pc_checkskill(sd, AB_LAUDAAGNUS) > 0) {
			resettargets();
			map_foreachinrange(targetlauda1, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				if (!duplicateskill(p, AB_LAUDAAGNUS)) unit_skilluse_ifable(&sd->bl, SELF, AB_LAUDAAGNUS, pc_checkskill(sd, AB_LAUDAAGNUS));
			}
		}
		/// Lauda Ramus
		if (canskill(sd)) if (pc_checkskill(sd, AB_LAUDARAMUS) > 0) {
			resettargets();
			map_foreachinrange(targetlauda2, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				if (!duplicateskill(p, AB_LAUDARAMUS)) unit_skilluse_ifable(&sd->bl, SELF, AB_LAUDARAMUS, pc_checkskill(sd, AB_LAUDARAMUS));
			}
		}

		/// GLORIA
		if (canskill(sd)) if (pc_checkskill(sd, PR_GLORIA)>0) {
			resettargets();
			map_foreachinrange(targetgloria, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, SELF, PR_GLORIA, pc_checkskill(sd, PR_GLORIA));
			}
		}
		/// Impositio Manus
		if (canskill(sd)) if (pc_checkskill(sd, PR_IMPOSITIO)>0) {
			resettargets();
			map_foreachinrange(targetmanus, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				if (!duplicateskill(p, PR_IMPOSITIO)) unit_skilluse_ifable(&sd->bl, SELF, PR_IMPOSITIO, pc_checkskill(sd, PR_IMPOSITIO));
			}
		}
		/// Suffragium
		if (canskill(sd)) if (pc_checkskill(sd, PR_SUFFRAGIUM) > 0) {
			resettargets();
			map_foreachinrange(targetsuffragium, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				if (!duplicateskill(p, PR_SUFFRAGIUM)) unit_skilluse_ifable(&sd->bl, SELF, PR_SUFFRAGIUM, pc_checkskill(sd, PR_SUFFRAGIUM));
			}
		}
		/// Sacrament
		if (canskill(sd)) if ((pc_checkskill(sd, AB_SECRAMENT) > 0) && ((Dangerdistance > 900) || (sd->special_state.no_castcancel))) {
			resettargets();
			map_foreachinrange(targetsacrament, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				if (!duplicateskill(p, AB_SECRAMENT)) unit_skilluse_ifable(&sd->bl, foundtargetID, AB_SECRAMENT, pc_checkskill(sd, AB_SECRAMENT));
			}
		}
		/// Expiatio
		if (canskill(sd)) if ((pc_checkskill(sd, AB_EXPIATIO) > 0) && ((Dangerdistance > 900) || (sd->special_state.no_castcancel))) {
			resettargets();
			map_foreachinrange(targetexpiatio, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, foundtargetID, AB_EXPIATIO, pc_checkskill(sd, AB_EXPIATIO));
			}
		}
		// Aura Blade
		if (pc_checkskill(sd, LK_AURABLADE) > 0) {
			if (!(sd->sc.data[SC_AURABLADE])) {
				unit_skilluse_ifable(&sd->bl, SELF, LK_AURABLADE, pc_checkskill(sd, LK_AURABLADE));
			}
		}
		// Maximize Power
		// **Note** : I have changed this skill to not disable SP regen.
		// If you did not, you should probably restrict usage to high SP levels and make the AI turn it off below a certain amount.
		// Or disable entirely, don't think this effect is worth losing SP regen unless you have a way to refill it.
		if (pc_checkskill(sd, BS_MAXIMIZE) > 0) {
			if (!(sd->sc.data[SC_MAXIMIZEPOWER])) {
				unit_skilluse_ifable(&sd->bl, SELF, BS_MAXIMIZE, pc_checkskill(sd, BS_MAXIMIZE));
			}
		}
		// Maximum Over Thrust
		if (pc_checkskill(sd, WS_OVERTHRUSTMAX) > 0) {
			if (!(sd->sc.data[SC_MAXOVERTHRUST])) {
				unit_skilluse_ifable(&sd->bl, SELF, WS_OVERTHRUSTMAX, pc_checkskill(sd, WS_OVERTHRUSTMAX));
			}
		}
		// Auto Guard
		if (pc_checkskill(sd, CR_AUTOGUARD) > 0) {
			if (!(sd->sc.data[SC_AUTOGUARD]))
				if (sd->status.shield > 0) {
					unit_skilluse_ifable(&sd->bl, SELF, CR_AUTOGUARD, pc_checkskill(sd, CR_AUTOGUARD));
				}
		}
		// Reflect Shield
		if (pc_checkskill(sd, CR_REFLECTSHIELD) > 0) {
			if (!(sd->sc.data[SC_REFLECTSHIELD]))
				if (sd->status.shield > 0) {
					unit_skilluse_ifable(&sd->bl, SELF, CR_REFLECTSHIELD, pc_checkskill(sd, CR_REFLECTSHIELD));
				}
		}
		// Spear quicken
		// Only in tanking mode. There is no point in ASPD if not using normal attacks.
		if (pc_checkskill(sd, CR_SPEARQUICKEN) > 0) {
			if ((sd->status.weapon == W_2HSPEAR) || (sd->status.weapon == W_1HSPEAR))
				if (sd->state.autopilotmode == 1)
				if (!(sd->sc.data[SC_SPEARQUICKEN])) {
					unit_skilluse_ifable(&sd->bl, SELF, CR_SPEARQUICKEN, pc_checkskill(sd, CR_SPEARQUICKEN));
				}
		}

		// 2H quicken
		// Only in tanking mode. There is no point in ASPD if not using normal attacks.
		if (pc_checkskill(sd, KN_TWOHANDQUICKEN) > 0) {
			if ((sd->status.weapon == W_2HSWORD))
				if (sd->state.autopilotmode == 1)
					if (!(sd->sc.data[KN_TWOHANDQUICKEN])) {
						unit_skilluse_ifable(&sd->bl, SELF, KN_TWOHANDQUICKEN, pc_checkskill(sd, KN_TWOHANDQUICKEN));
					}
		}

		// 1H quicken
		// Only in tanking mode. There is no point in ASPD if not using normal attacks.
		if (pc_checkskill(sd, KN_ONEHAND) > 0) {
			if ((sd->status.weapon == W_1HSWORD))
				if (sd->state.autopilotmode == 1)
					if (!(sd->sc.data[KN_ONEHAND])) {
						unit_skilluse_ifable(&sd->bl, SELF, KN_ONEHAND, pc_checkskill(sd, KN_ONEHAND));
					}
		}
		// Parrying
		if (pc_checkskill(sd, LK_PARRYING) > 0) {
			if ((sd->status.weapon == W_2HSWORD))
				if (!(sd->sc.data[LK_PARRYING])) {
					unit_skilluse_ifable(&sd->bl, SELF, LK_PARRYING, pc_checkskill(sd, LK_PARRYING));
				}
		}
		// Concentration
		if (pc_checkskill(sd, LK_CONCENTRATION) > 0) if (sd->state.enableconc) {
			if (!(sd->sc.data[SC_CONCENTRATION])) {
				unit_skilluse_ifable(&sd->bl, SELF, LK_CONCENTRATION, pc_checkskill(sd, LK_CONCENTRATION));
			}
		}

		// Attention Concentrate
		if (pc_checkskill(sd, AC_CONCENTRATION) > 0) {
			if (!(sd->sc.data[SC_CONCENTRATE])) {
				unit_skilluse_ifable(&sd->bl, SELF, AC_CONCENTRATION, pc_checkskill(sd, AC_CONCENTRATION));
			}
		}

		// True Sight
		if (pc_checkskill(sd, SN_SIGHT) > 0) {
			if (!(sd->sc.data[SC_TRUESIGHT])) {
				unit_skilluse_ifable(&sd->bl, SELF, SN_SIGHT, pc_checkskill(sd, SN_SIGHT));
			}
		}

		// Preserve
		// **Note** I changed this a toggle skill with (near) infinite duration. If you did not, uncomment that timer part to allow recasting it.
		// The AI assumes it's supposed to maintain this on at all times and keep whatever skill the player learned manually.
		if (pc_checkskill(sd, ST_PRESERVE) > 0) {
			if (!(sd->sc.data[SC_PRESERVE]) /*|| (sd->sc.data[SC_PRESERVE]->timer<=10000)*/) {
				unit_skilluse_ifable(&sd->bl, SELF, ST_PRESERVE, pc_checkskill(sd, ST_PRESERVE));
			}
		}


		// Crazy Uproar
		if (pc_checkskill(sd, MC_LOUD) > 0) {
			resettargets();
			map_foreachinrange(targetloud, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, SELF, MC_LOUD, pc_checkskill(sd, MC_LOUD));
			}
		}
		// Providence
		if (canskill(sd)) if (Dangerdistance >= 900) if (pc_checkskill(sd, CR_PROVIDENCE)>0) {
			resettargets();
			map_foreachinrange(targetprovidence, &sd->bl, 9, BL_PC, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, foundtargetID, CR_PROVIDENCE, pc_checkskill(sd, CR_PROVIDENCE));
			}
		}

		// Homunculus
		if (canskill(sd)) if (Dangerdistance >=900) if (pc_checkskill(sd, AM_RESURRECTHOMUN) > 0)
			if (sd->status.hom_id) {
				if (!sd->hd) intif_homunculus_requestload(sd->status.account_id, sd->status.hom_id); else
				{
					if (status_isdead(&sd->hd->bl)) unit_skilluse_ifable(&sd->bl, SELF, AM_RESURRECTHOMUN, pc_checkskill(sd, AM_RESURRECTHOMUN));
				}
		}
		if (canskill(sd)) if (Dangerdistance >= 900) if (pc_checkskill(sd, AM_CALLHOMUN) > 0)
			if (sd->status.hom_id) {
				if (!sd->hd) intif_homunculus_requestload(sd->status.account_id, sd->status.hom_id);
				 else
				{
					if (sd->hd && sd->hd->homunculus.vaporize) unit_skilluse_ifable(&sd->bl, SELF, AM_CALLHOMUN, pc_checkskill(sd, AM_CALLHOMUN));
				}
			}

		


		//
		// Songs
		//
		// **Note** : I'm using "player_skill_partner_check: no" so ensembles do not check for a partner. If that option is yes, an additional trigger to walk next to a potential partner is necessary.
		// Must be in party with a leader to sing
		// **Note** : I'm not using the skill update that turns songs into basically normal AOE buff spells.
		// If you do, you'll need to rework this entirely section to adapt to that system.
		if (leaderID > -1) {
			// Use longing if doing ensemble to enable other skills
			if (sd->sc.data[SC_DANCING])
				// **Note** I changed Longing to also work on non-ensemble skills to enable skills/attacking. Uncomment below if you did not.
				// if (sd->sc.data[SC_DANCING]->val4)
				if (canskill(sd)) if (!(sd->sc.data[SC_LONGING]))
					if ((pc_checkskill(sd, CG_LONGINGFREEDOM) > 0)) unit_skilluse_ifable(&sd->bl, SELF, CG_LONGINGFREEDOM, pc_checkskill(sd, CG_LONGINGFREEDOM));
			// Close to leader, and not already performing (distance 6 is how far away we try to be if leader is tanking monsters
			// Distance will no longer be relevant once increased song range is implemented from official
			if ((leaderdistance <= 6) && (sd->state.autosong > 0) && !(sd->sc.data[SC_DANCING])) {
				if (canskill(sd) && ((sd->status.weapon == W_WHIP) || (sd->status.weapon == W_MUSICAL))) {
					if ((sd->skill_id_dance == sd->state.autosong) && (pc_checkskill(sd, BD_ENCORE) > 0)) unit_skilluse_ifable(&sd->bl, SELF, BD_ENCORE, pc_checkskill(sd, BD_ENCORE));
					else if ((pc_checkskill(sd, sd->state.autosong) > 0)) unit_skilluse_ifable(&sd->bl, SELF, sd->state.autosong, pc_checkskill(sd, sd->state.autosong));
				}
			}
			else { // Far from leader or song mode turned off, stop.
				if ((sd->sc.data[SC_DANCING]) && ((leaderdistance >= 10) || sd->state.autosong==0)) {
					// Do not use if ensemble, can walk away
					if (!(sd->sc.data[SC_DANCING]->val4)) if ((pc_checkskill(sd, BD_ADAPTATION) > 0)) unit_skilluse_ifable(&sd->bl, SELF, BD_ADAPTATION, pc_checkskill(sd, BD_ADAPTATION));
				}
				// Walk to leader is top priority in song mode, don't care about using other skills until in range.
				// Note : this only triggers if leader was found, otherwise normal fight/walk rules apply.
				if ((sd->state.autosong > 0) && (leaderdistance >= 7)) {
					goto followleader;

				}
			}
		}

		// Defending Aura
		// Activate if being targeted by a ranged enemy	
		if (Dangerdistance<900)	if (canskill(sd)) if ((pc_checkskill(sd, CR_DEFENDER)>0) && (dangermd->status.rhw.range > 3) &&
				!(sd->sc.data[SC_DEFENDER]))
			{			
					unit_skilluse_ifable(&sd->bl, SELF, CR_DEFENDER, pc_checkskill(sd, CR_DEFENDER));
			}

		// Gunslinger Adjustment
		if (Dangerdistance < 900) if (canskill(sd)) if ((pc_checkskill(sd, GS_ADJUSTMENT) > 0) && (dangermd->status.rhw.range > 3))
			if (!sd->sc.data[SC_MADNESSCANCEL]) if (!sd->sc.data[SC_ADJUSTMENT]) if (sd->spiritball >= 2)
		{
			unit_skilluse_ifablexy(&sd->bl, sd->bl.id, GS_ADJUSTMENT, pc_checkskill(sd, GS_ADJUSTMENT));
		}

		// No matter which mode if we are too far from leader, prioritize returning!
		if (p) if (leaderID > -1) if (leaderdistance >= 20) goto followleader;

		///////////////////////////////////////////////////////////////////////////////////////////////
		/// Emergency spells to use when in danger of being attacked (mostly useful for mage classes)
		///////////////////////////////////////////////////////////////////////////////////////////////
		// Free Casting? Walk away from enemy!
		if (Dangerdistance <= 6) if (pc_checkskill(sd, SA_FREECAST) > 0) if ((leaderID == -1) || (leaderdistance <= 10)) 
		// Not in tanking mode!
			if (sd->state.autopilotmode>1) {
				newwalk(&sd->bl, bl->x - sgn(dangerbl->x - bl->x), bl->y - sgn(dangerbl->y - bl->y), 0);
		}

		// Tanking? Use Poison React!
		if ((Dangerdistance <= 1) && (sd->state.autopilotmode==1))
			if (canskill(sd)) if ((pc_checkskill(sd, AS_POISONREACT)>0) && (dangermd->status.rhw.range <= 3))
				if (!sd->sc.data[SC_POISONREACT]) unit_skilluse_ifable(&sd->bl, SELF, AS_POISONREACT, pc_checkskill(sd, AS_POISONREACT));
		// Not tanking? Cloak!
		if ((Dangerdistance <= 3) && (sd->state.autopilotmode > 1))
			if (canskill(sd)) if ((pc_checkskill(sd, AS_CLOAKING)>=10) && (dangermd->status.rhw.range <= 3))
				if ((dangermd->status.race != RC_DEMON) && (dangermd->status.race != RC_INSECT) && (!((status_get_class_(dangerbl) == CLASS_BOSS))))
				if (!sd->sc.data[SC_CLOAKING]) unit_skilluse_ifable(&sd->bl, SELF, AS_CLOAKING, pc_checkskill(sd, AS_CLOAKING));


		// Safety Wall
		// At most 3 mobs, nearest must be close and melee.
		// Must be very powerful and a real threat!
		if ((Dangerdistance <= 3) && (dangercount<4)) {
			if (canskill(sd)) if ((pc_checkskill(sd, MG_SAFETYWALL)>0) && (dangermd->status.rhw.range <= 3)
				&& (dangermd->status.rhw.atk2>sd->battle_status.hp / 5) && (pc_search_inventory(sd, ITEMID_BLUE_GEMSTONE)>=0)
				&& (!sd->sc.data[SC_PNEUMA]) && (!sd->sc.data[SC_SAFETYWALL]))
			// If we are in tanking mode, distance must be 1, we will otherwise move towards monster!
			{ if ((sd->state.autopilotmode != 1) || (Dangerdistance <= 1))
					 unit_skilluse_ifablexy(&sd->bl, sd->bl.id, MG_SAFETYWALL, pc_checkskill(sd, MG_SAFETYWALL));
			}
		}
		// Steel Body
		// Tanking mode only, against very powerful enemies
		if ((Dangerdistance <= 10) || sd->state.specialtanking) {
			if (canskill(sd)) if ((pc_checkskill(sd, MO_STEELBODY)>0) && (sd->state.specialtanking || (dangermd->status.rhw.atk2>sd->battle_status.hp / 5)) && (!sd->sc.data[SC_STEELBODY]))
				if ((sd->spiritball >= 5) && (sd->state.autopilotmode == 1)) {
					unit_skilluse_ifable(&sd->bl, SELF, MO_STEELBODY, pc_checkskill(sd, MO_STEELBODY));
				}
		}

		// Fire Wall
		// Only if there still is enough distance to matter and enemy is not ranged
		// No more than 1 at the same position, max of 3 total
		// never use in tanking mode, meaningless
		if ((Dangerdistance >= 5) && (Dangerdistance <900)) {
			if (canskill(sd)) if ((pc_checkskill(sd, MG_FIREWALL)>0) && (dangermd->status.hp<2000)) {
				if (elemallowed(dangermd, skill_get_ele(MG_FIREWALL, pc_checkskill(sd, MG_FIREWALL))) && (dangermd->status.rhw.range <= 3)) {
					if (!((status_get_class_(dangerbl) == CLASS_BOSS))) if (sd->state.autopilotmode != 1) {
						int i,j = 0;
						for (i = 0; i < MAX_SKILLUNITGROUP && ud->skillunit[i]; i++) {
							if (ud->skillunit[i]->skill_id == MG_FIREWALL) {
								j++;
								if (((abs(ud->skillunit[i]->unit->bl.x-((targetbl->x+bl->x)/2))<2) && (abs(ud->skillunit[i]->unit->bl.y - ((targetbl->y+bl->y)/2))<2))) j = 999;
							}
						}
						if (j<3) unit_skilluse_ifablebetween(&sd->bl, founddangerID, MG_FIREWALL, pc_checkskill(sd, MG_FIREWALL));
					}
				}
			}
		}
		/// Frost Nova
		if (canskill(sd)) if (pc_checkskill(sd, WZ_FROSTNOVA) > 0) {
			if (Dangerdistance <= 2) {
				if (elemallowed(dangermd, skill_get_ele(WZ_FROSTNOVA, pc_checkskill(sd, WZ_FROSTNOVA)))) {
					if (!isdisabled(dangermd))
						if (!(dangermd->status.def_ele == ELE_UNDEAD)) {
							if (!((status_get_class_(dangerbl) == CLASS_BOSS)))
								unit_skilluse_ifable(&sd->bl, founddangerID, WZ_FROSTNOVA, pc_checkskill(sd, WZ_FROSTNOVA));
						}
				}
			}
		}

		/// Frost Joker
		if (canskill(sd)) if (pc_checkskill(sd, BA_FROSTJOKER) > 0) {
			// At least 5 enemies must be present
			if (map_foreachinrange(AOEPriorityfreeze, &sd->bl, 7, BL_MOB, ELE_NONE) >= 10)
				unit_skilluse_ifable(&sd->bl, SELF, BA_FROSTJOKER, pc_checkskill(sd, BA_FROSTJOKER));
		}
		/// Scream
		if (canskill(sd)) if (pc_checkskill(sd, DC_SCREAM) > 0) {
			// At least 5 enemies must be present
			if (map_foreachinrange(AOEPriorityfreeze, &sd->bl, 7, BL_MOB, ELE_NONE) >= 10)
				unit_skilluse_ifable(&sd->bl, SELF, DC_SCREAM, pc_checkskill(sd, DC_SCREAM));
		}

		// Fiber Lock
		// Only against dangerous enemies that are not in range to attack us, costs cobweb
		if (canskill(sd)) if (pc_checkskill(sd, PF_SPIDERWEB) > 0) {
			if ((Dangerdistance <= 7) && (dangermd->status.rhw.range < Dangerdistance)
				&& (dangermd->status.rhw.atk2>sd->battle_status.hp / 5) && (pc_search_inventory(sd, 1025)>=0)) {
				if (!isdisabled(dangermd)) {
					int maxcount = 99;
					if (BL_PC&battle_config.land_skill_limit) if (!((maxcount = skill_get_maxcount(PF_SPIDERWEB, pc_checkskill(sd, PF_SPIDERWEB)))==0)) maxcount = 99;

					int v;
					for (v = 0; v<MAX_SKILLUNITGROUP && sd->ud.skillunit[v] && maxcount; v++) {
						if (sd->ud.skillunit[v]->skill_id == PF_SPIDERWEB)
							maxcount--;
					}

					if (maxcount>0)	unit_skilluse_ifable(&sd->bl, founddangerID, PF_SPIDERWEB, pc_checkskill(sd, PF_SPIDERWEB));
				}
				}
			}
		



		// Don't bother with these suboptimal spells if casting is uninterruptable (note, they can be still cast as a damage spell, but not as an emergency reaction when fast cast time is needed)
		if (!(sd->special_state.no_castcancel)) {
			/// Napalm Beat
			// **Note** : This has been modded to be uninterruptable and faster to use. Unmodded the AI probably shouldn't ever cast it, it's that bad.
			// It is the spell to use in the worst emergencies only, when enemy is at most 2 steps from hitting us.
			if (Dangerdistance <= 2) {
				if (canskill(sd)) if ((pc_checkskill(sd, MG_NAPALMBEAT)>0) && (dangermd->status.hp < sd->battle_status.matk_max * 4)) {
					if (elemallowed(dangermd, skill_get_ele(MG_NAPALMBEAT, pc_checkskill(sd, MG_NAPALMBEAT)))) {
						unit_skilluse_ifable(&sd->bl, founddangerID, MG_NAPALMBEAT, pc_checkskill(sd, MG_NAPALMBEAT));
					}
				}
			}
			// Soul Strike
			// **Note** : This has been modded to be uninterruptable, but due to low cast time same logic should be fine anyway
			// Don't bother with this at low levels. Don't use if unlikely to kill target.
			if (canskill(sd)) if (pc_checkskill(sd, MG_SOULSTRIKE) > 5) {
				if ((Dangerdistance <= 4)) {
					if ((elemallowed(dangermd, skill_get_ele(MG_SOULSTRIKE, pc_checkskill(sd, MG_SOULSTRIKE)))) && (dangercount == 1) && (dangermd->status.hp < sd->battle_status.matk_max * 4)) {
						unit_skilluse_ifable(&sd->bl, founddangerID, MG_SOULSTRIKE, pc_checkskill(sd, MG_SOULSTRIKE));
					}
				}
			}
			/// Fireball
			// Reasonably fast to try and cast in an emergency
			// Perfect for multiple enemies due to decent AOE
			// Don't use if enemy still has a lot of hp
			if (canskill(sd)) if (pc_checkskill(sd, MG_FIREBALL) > 5) {
				if ((Dangerdistance <= 4)) {
					if ((elemallowed(dangermd, skill_get_ele(MG_FIREBALL, pc_checkskill(sd, MG_FIREBALL)))) && (dangercount > 1) && (dangermd->status.hp < sd->battle_status.matk_max * 6)) {
						unit_skilluse_ifable(&sd->bl, founddangerID, MG_FIREBALL, pc_checkskill(sd, MG_FIREBALL));
					}
				}
			}
			/// Frost Diver
			// Our best spell to use when under attack, if none of the other special conditions apply
			if (canskill(sd)) if (pc_checkskill(sd, MG_FROSTDIVER) > 0) {
				if (Dangerdistance <= 4) {
					if (elemallowed(dangermd, skill_get_ele(MG_FROSTDIVER, pc_checkskill(sd, MG_FROSTDIVER)))) {
						if (!isdisabled(dangermd))
							if (!(dangermd->status.def_ele == ELE_UNDEAD)) {
								if (!((status_get_class_(dangerbl) == CLASS_BOSS)))
									unit_skilluse_ifable(&sd->bl, founddangerID, MG_FROSTDIVER, pc_checkskill(sd, MG_FROSTDIVER));
							}
					}
				}
			}
			/// Dust
			if (canskill(sd)) if (pc_checkskill(sd, GS_DUST) > 0) {
					if (Dangerdistance <= 2) if (distance_bl(dangerbl,&sd->bl)<=3) {
						if (sd->status.weapon == W_SHOTGUN) if (elemallowed(dangermd, skill_get_ele(GS_DUST, pc_checkskill(sd, GS_DUST)))) {
							if (!isdisabled(dangermd))
								if (!((status_get_class_(dangerbl) == CLASS_BOSS)))
									unit_skilluse_ifable(&sd->bl, founddangerID, GS_DUST, pc_checkskill(sd, GS_DUST));
						}
					}
			}
			/// Cracker
			if (canskill(sd)) if (pc_checkskill(sd, GS_CRACKER) > 0) {
				if (sd->spiritball >= 1)
					if (Dangerdistance <= 2) {
					if (elemallowed(dangermd, skill_get_ele(GS_CRACKER, pc_checkskill(sd, GS_CRACKER)))) {
						if (!isdisabled(dangermd))
								if (!((status_get_class_(dangerbl) == CLASS_BOSS)))
									unit_skilluse_ifable(&sd->bl, founddangerID, GS_CRACKER, pc_checkskill(sd, GS_CRACKER));
					}
				}
			}
			/// Stone Curse
			// we can use a gem to petrify it too if the moster is really that dangerous and not in range for safety wall
			// **Note** : this skill was modded to have higher range and deal more percentage damage as well as cast faster. 
			// Otherwise it might be best to avoid using it by the AI altogether, it's just too useless?
			if (canskill(sd)) if (pc_checkskill(sd, MG_STONECURSE) > 0) {
				if (Dangerdistance <= 6) {
					if ((dangermd->status.rhw.atk2 > sd->battle_status.hp / 5) && (pc_search_inventory(sd, ITEMID_RED_GEMSTONE) >= 0))
						if (elemallowed(dangermd, skill_get_ele(MG_STONECURSE, pc_checkskill(sd, MG_STONECURSE)))) {
							if (!isdisabled(dangermd))
								if (!(dangermd->status.def_ele == ELE_UNDEAD)) {
									if (!((status_get_class_(dangerbl) == CLASS_BOSS)))
										unit_skilluse_ifable(&sd->bl, founddangerID, MG_STONECURSE, pc_checkskill(sd, MG_STONECURSE));
								}
						}
				}
			}
		}
		///////////////////////////////////////////////////////////////////////////////////////////////
		// Skills to use before attacking
		// Ruwach, Sight
		if (canskill(sd)) if ((pc_checkskill(sd, AL_RUWACH) > 0) || (pc_checkskill(sd, MG_SIGHT) > 0)){
			if (!((sd->sc.data[SC_RUWACH]) || (sd->sc.data[SC_SIGHT]))) {
				resettargets();
				map_foreachinrange(targetnearest, &sd->bl, 11, BL_MOB, sd);
				if ((targetdistance <= 3) && (targetdistance > -1) && (targetmd->sc.data[SC_HIDING] || targetmd->sc.data[SC_CLOAKING])) {
					if (pc_checkskill(sd, AL_RUWACH) > 0) unit_skilluse_ifable(&sd->bl, SELF, AL_RUWACH, pc_checkskill(sd, AL_RUWACH));
					if (pc_checkskill(sd, MG_SIGHT) > 0) unit_skilluse_ifable(&sd->bl, SELF, MG_SIGHT, pc_checkskill(sd, MG_SIGHT));
				}
			}
		}
		// Signum Cruxis
		if (canskill(sd)) if ((pc_checkskill(sd, AL_CRUCIS) > 0)){
			if (map_foreachinrange(signumcount, &sd->bl, 15, BL_MOB, sd) >= 3) if (!duplicateskill(p, AL_CRUCIS)) {
				unit_skilluse_ifable(&sd->bl, SELF, AL_CRUCIS, pc_checkskill(sd, AL_CRUCIS));
			}
		}
		// Last Stand, Gatling Fever
		if (canskill(sd)) if ((pc_checkskill(sd, GS_GATLINGFEVER) > 0) || (pc_checkskill(sd, GS_MADNESSCANCEL) > 0)) {
		targetdistance = 0;
		map_foreachinrange(counthp, &sd->bl, AUTOPILOT_RANGE_CAP, BL_MOB, sd);
		// Use this if nearby enemies are expected to take a while to beat.
		// This assumes damage output of characters are not too different from the gunslinger and party members are actually participating in the battle.
		if (targetdistance > pc_rightside_atk(sd) * 10 * partycount) {
			if (pc_checkskill(sd, GS_GATLINGFEVER) > 0) if (sd->status.weapon == W_GATLING) if (!sd->sc.data[SC_GATLINGFEVER]) unit_skilluse_ifable(&sd->bl, SELF, GS_GATLINGFEVER, pc_checkskill(sd, GS_GATLINGFEVER));
			if (pc_checkskill(sd, GS_MADNESSCANCEL) > 0) if (!sd->sc.data[SC_MADNESSCANCEL]) if (!sd->sc.data[SC_ADJUSTMENT]) if (sd->spiritball >= 4) unit_skilluse_ifable(&sd->bl, SELF, GS_MADNESSCANCEL, pc_checkskill(sd, GS_MADNESSCANCEL));
		}
		}

		///////////////////////////////////////////////////////////////////////////////////////////////
		/// Skills to prioritize based on elemental weakness
		///////////////////////////////////////////////////////////////////////////////////////////////
		// AOE skills go here, they should have higher priority than single target when able to hit multiple things
		// Only target around tank or human players. Using AOE with cast time on
		// untanked monsters will just miss them but we can rely on the human being smart enough to keep them in the AOE if they see it 
		// being cast around them.
		if (sd->state.autopilotmode == 2) {

			int spelltocast = -1;
			int bestpriority = -1;
			int priority;
			int IDtarget = -1;

			int j;
			if (p) //Search leader
				for (j = 0; j < MAX_PARTY; j++) {

					resettargets();
					
					// This is for finding if a walkable path exists. We don't need to waste CPU resources on that to target a spell.
					//targetthis = p->party.member[j].char_id;
					//map_foreachinmap(targetthischar, &sd->bl.m, BL_PC, sd);
					
					// This party member is in range?
					targetbl = map_id2bl(p->party.member[j].account_id);
					if (targetbl) {
						if (distance_bl(&sd->bl, targetbl) <= 9) // No more than 9 range and must be shootable to target
							if (path_search_long(NULL, sd->bl.m, sd->bl.x, sd->bl.y, targetbl->x, targetbl->y, CELL_CHKWALL,9)) { foundtargetID = p->party.member[j].account_id; }
					}

					if (foundtargetID > -1) {
						int foundtargetID2 = foundtargetID;  
						map_session_data *membersd = (struct map_session_data*)targetbl;
						int targetdistance2 = distance_bl(targetbl, bl); 
						block_list * targetbl2 = targetbl;

						// Don't try to use if member too far, must get closer first
						// member must be in tanking mode, or controlled by human player. Other characters are unlikely to keep monsters in the AOE while casting.
						// targeting self is also ok if we don't have the sage skill to walk away, in case of skills with short enough cast times and no interruption.
						if (((membersd->state.autopilotmode<=1) || ((membersd->bl.id==sd->bl.id) && (pc_checkskill(sd, SA_FREECAST)==0))) && (targetdistance2 <= 9)) {
							// obsolete now that leader has own variable

							// This assumes skill is updated to allow free moving and do good damage otherwise it's a suicide for AI
							// **Note** It also assumes it was modded to still ignore MDEF despite that update removing that property
							// Gravitational Field
							// Same priority as the big wizard spells minus one. So use only if those are resisted or subptimal
							if (canskill(sd)) if ((pc_checkskill(sd, HW_GRAVITATION) > 0) && (Dangerdistance > 900) && (pc_search_inventory(sd, ITEMID_BLUE_GEMSTONE)>0)) {
							int area = 2; // priority scale up by MDEf in AOEPriorityGrav
							priority = 3 * map_foreachinrange(AOEPriorityGrav, targetbl2, area, BL_MOB, ELE_NONE) -1;
							if ((priority>=6) && (priority>bestpriority)) {
							spelltocast = HW_GRAVITATION; bestpriority = priority;IDtarget = foundtargetID2;
							}
							}
							// Storm Gust
							if (canskill(sd)) if ((pc_checkskill(sd, WZ_STORMGUST) > 0) && (Dangerdistance > 900)) {
								int area = 5;
								priority = 3 * map_foreachinrange(AOEPrioritySG, targetbl2, area, BL_MOB, skill_get_ele(WZ_STORMGUST, pc_checkskill(sd, WZ_STORMGUST)));
								if ((priority >= 18) && (priority > bestpriority)) if (!duplicateskill(p, WZ_STORMGUST)) {
									spelltocast = WZ_STORMGUST; bestpriority = priority; IDtarget = foundtargetID2;
								}
							}
							// Quagmire
							if (canskill(sd)) if (pc_checkskill(sd, WZ_QUAGMIRE) > 0) {
								struct map_session_data *sd2 = (struct map_session_data*)targetbl2;
								// Only if flee based tank otherwise reduced hit doesn't matter on monster
								// Only if not amplified yet, wastes amplify
								if ((sd2->battle_status.flee - 1.75*sd2->status.base_level >= 100) && !(sd->sc.data[SC_MAGICPOWER])) {
									int area = 2;
									priority = map_foreachinrange(Quagmirepriority, targetbl2, area, BL_MOB, skill_get_ele(WZ_QUAGMIRE, pc_checkskill(sd, WZ_QUAGMIRE)));
									if ((priority >= 4) && (priority > bestpriority)) {
										spelltocast = WZ_QUAGMIRE; bestpriority = 500;  // do this first before the AOEs to help tank survive
										IDtarget = foundtargetID2;
									}
								}
							}
							// Lord of Vermillion
							if (canskill(sd)) if ((pc_checkskill(sd, WZ_VERMILION) > 0) && (Dangerdistance > 900)) {
								int area = 5;
								priority = 3 * map_foreachinrange(AOEPriority, targetbl2, area, BL_MOB, skill_get_ele(WZ_VERMILION, pc_checkskill(sd, WZ_VERMILION)));
								if ((priority >= 18) && (priority > bestpriority)) {
									spelltocast = WZ_VERMILION; bestpriority = priority; IDtarget = foundtargetID2;
								}
							}
							// Meteor Storm
							if (canskill(sd)) if ((pc_checkskill(sd, WZ_METEOR) > 0) && (Dangerdistance > 900)) {
								int area = 3;
								priority = 3 * map_foreachinrange(AOEPriority, targetbl2, area, BL_MOB, skill_get_ele(WZ_METEOR, pc_checkskill(sd, WZ_METEOR)));
								if ((priority >= 18) && (priority > bestpriority)) {
									spelltocast = WZ_METEOR; bestpriority = priority; IDtarget = foundtargetID2;
								}
							}

							// Thunderstorm
							if (canskill(sd)) if ((pc_checkskill(sd, MG_THUNDERSTORM) > 0) && (Dangerdistance > 900)) {
								// modded : 5x5 but 7x7 at level 6 or higher.
								int area = 2; if (pc_checkskill(sd, MG_THUNDERSTORM) > 5) area++;
								priority = map_foreachinrange(AOEPriority, targetbl2, area, BL_MOB, skill_get_ele(MG_THUNDERSTORM, pc_checkskill(sd, MG_THUNDERSTORM)));
								if ((priority >= 6) && (priority > bestpriority)) {
									spelltocast = MG_THUNDERSTORM; bestpriority = priority; IDtarget = foundtargetID2;
								}
							}

							// NIN : Lightning Jolt
							if (canskill(sd)) if ((pc_checkskill(sd, NJ_RAIGEKISAI) > 2) && (Dangerdistance > 900))
								if (pc_search_inventory(sd, 7523) >= 0) 
									if (pc_rightside_atk(sd) < sd->battle_status.matk_min) 
										{
								int area = 2; if (pc_checkskill(sd, NJ_RAIGEKISAI) >= 5) area++;
								priority = map_foreachinrange(AOEPriority, targetbl2, area, BL_MOB, skill_get_ele(NJ_RAIGEKISAI, pc_checkskill(sd, NJ_RAIGEKISAI)));
								if ((priority >= 6) && (priority > bestpriority)) {
									spelltocast = NJ_RAIGEKISAI; bestpriority = priority; IDtarget = foundtargetID2;
								}
							}
							// NIN : First Wind
							if (canskill(sd)) if ((pc_checkskill(sd, NJ_KAMAITACHI) > 2) && (Dangerdistance > 900))
								if (pc_search_inventory(sd, 7523) >= 0)
									if (pc_rightside_atk(sd) < sd->battle_status.matk_min)
									{  // Note : This hits in a line but has a cast time and tanking does not guarantee
									   // the targets will stay in that line plus the moving target moves the line itself.
									   // However all monsters on the same time are likely to still be together so pretend
									   // it's a 1x1 AOE. Priority is higher than Jolt. 
										foundtargetID = -1; targetdistance = 999;
										map_foreachinrange(targetnearest, targetbl2, 9, BL_MOB, sd); // Nearest to the tank, not us!
										if (foundtargetID > -1) {
											int area = 1;
											priority = 2 * map_foreachinrange(AOEPriority, targetbl, area, BL_MOB, skill_get_ele(NJ_KAMAITACHI, pc_checkskill(sd, NJ_KAMAITACHI)));
											if (((priority >= 12) && (priority > bestpriority)) && (distance_bl(targetbl, &sd->bl) <= 9)) {
												spelltocast = NJ_KAMAITACHI; bestpriority = priority; IDtarget = foundtargetID;
											}
										}
									}
							// Fireball
							// This is special - it targets a monster despite having AOE, not a ground skill
							if (canskill(sd)) if ((pc_checkskill(sd, MG_FIREBALL) > 0)) {
								foundtargetID = -1; targetdistance = 999;
								map_foreachinrange(targetnearest, targetbl2, 9, BL_MOB, sd);
								if (foundtargetID > -1) {
									int area = 2;
									priority = map_foreachinrange(AOEPriority, targetbl, area, BL_MOB, skill_get_ele(MG_FIREBALL, pc_checkskill(sd, MG_FIREBALL)));
									if (((priority >= 6) && (priority > bestpriority)) && (distance_bl(targetbl, &sd->bl) <= 9)) {
										spelltocast = MG_FIREBALL; bestpriority = priority; IDtarget = foundtargetID;
									}
								}
							}

							// Judex
							// This is special - it targets a monster despite having AOE, not a ground skill
							if (canskill(sd)) if ((pc_checkskill(sd, AB_JUDEX) > 0) && (Dangerdistance > 900)) {
								foundtargetID = -1; targetdistance = 999;
								map_foreachinrange(targetnearest, targetbl2, 9, BL_MOB, sd);
								if (foundtargetID > -1) {
									int area = 1;
									priority = map_foreachinrange(AOEPriority, targetbl, area, BL_MOB, skill_get_ele(AB_JUDEX, pc_checkskill(sd, AB_JUDEX)));
									if (((priority >= 6) && (priority > bestpriority)) && (distance_bl(targetbl, &sd->bl) <= 9)) {
										spelltocast = AB_JUDEX; bestpriority = priority; IDtarget = foundtargetID;
									}
								}
							}

							// Adoramus
							// This is special - it targets a monster despite having AOE, not a ground skill
							if (canskill(sd)) if ((pc_checkskill(sd, AB_ADORAMUS) > 0) && (Dangerdistance > 900))
								// save some gems for resurrection and whatever
								if (pc_inventory_count(sd, ITEMID_BLUE_GEMSTONE) > 10) {
								foundtargetID = -1; targetdistance = 999;
								map_foreachinrange(targetnearest, targetbl2, 9, BL_MOB, sd);
								if (foundtargetID > -1) {
									int area = 1; if (pc_checkskill(sd, AB_ADORAMUS) >= 7) area++;
									priority = 2*map_foreachinrange(AOEPriority, targetbl, area, BL_MOB, skill_get_ele(AB_ADORAMUS, pc_checkskill(sd, AB_ADORAMUS)));
									if (((priority >= 6) && (priority > bestpriority)) && (distance_bl(targetbl, &sd->bl) <= 9)) {
										spelltocast = AB_ADORAMUS; bestpriority = priority; IDtarget = foundtargetID;
									}
								}
							}


							// Spread Attack
							// This is special - it targets a monster despite having AOE, not a ground skill
							if (canskill(sd)) if ((pc_checkskill(sd, GS_SPREADATTACK) > 0))
								if ((sd->status.weapon == W_SHOTGUN) || (sd->status.weapon == W_GRENADE)) {
								foundtargetID = -1; targetdistance = 999;
								map_foreachinrange(targetnearest, targetbl2, 9 + pc_checkskill(sd, GS_SNAKEEYE), BL_MOB, sd);
								if (foundtargetID > -1) {
								int area = 1;
								if (pc_checkskill(sd, GS_SPREADATTACK) >= 4) area++;
								if (pc_checkskill(sd, GS_SPREADATTACK) >= 7) area++;
								if (pc_checkskill(sd, GS_SPREADATTACK) >= 10) area++;
								priority = map_foreachinrange(AOEPriority, targetbl, area, BL_MOB, skill_get_ele(GS_SPREADATTACK, pc_checkskill(sd, GS_SPREADATTACK)));
								if (((priority >= 6) && (priority > bestpriority)) && (distance_bl(targetbl, &sd->bl) <= 9 + pc_checkskill(sd, GS_SNAKEEYE))) {
									spelltocast = GS_SPREADATTACK; bestpriority = priority; IDtarget = foundtargetID;
								}
								}
							}

							// Sharp Shooting
							// Prefer this over Blitz Beat if available, does more damage
							if (canskill(sd)) if ((pc_checkskill(sd, SN_SHARPSHOOTING) > 0))
								if (sd->status.weapon == W_BOW) {
									foundtargetID = -1; targetdistance = 999;
									map_foreachinrange(targetnearest, targetbl2, 9, BL_MOB, sd);
									if (foundtargetID > -1) {
										int area = 1; // This skill hits more area than this but see First Wind comments.
										arrowchange(sd, targetmd);
										priority = 2*map_foreachinrange(AOEPriority, targetbl, area, BL_MOB, skill_get_ele(SN_SHARPSHOOTING, pc_checkskill(sd, SN_SHARPSHOOTING)));
										if (((priority >= 7) && (priority > bestpriority)) && (distance_bl(targetbl, &sd->bl) <= 9)) {
											spelltocast = SN_SHARPSHOOTING; bestpriority = priority; IDtarget = foundtargetID;
										}
									}
								}


							// Blitz Beat
							// **Note** I reduced the cast time on this.
							// Without that modification, consider if using Arrow Shower might be better.
							// I kept this skill to use INT, if you did apply the update that changed to AGI, replace that here too.
							if (canskill(sd)) if ((pc_checkskill(sd, HT_BLITZBEAT) > 0)) if (sd->status.int_>=30)
								if (pc_isfalcon(sd)) {
								foundtargetID = -1; targetdistance = 999;
								map_foreachinrange(targetnearest, targetbl2, 3 + pc_checkskill(sd, AC_VULTURE), BL_MOB, sd);
								if (foundtargetID > -1) {
									int area = 1;
									priority = 1+map_foreachinrange(AOEPriority, targetbl, area, BL_MOB, skill_get_ele(HT_BLITZBEAT, pc_checkskill(sd, HT_BLITZBEAT)));
									if (((priority >= 7) && (priority > bestpriority)) && (distance_bl(targetbl, &sd->bl) <= 3 + pc_checkskill(sd, AC_VULTURE))) {
										spelltocast = HT_BLITZBEAT; bestpriority = priority; IDtarget = foundtargetID;
									}
								}
							}

							// Arrow Shower
							if (canskill(sd)) if ((pc_checkskill(sd, AC_SHOWER) > 0)) if (sd->status.weapon == W_BOW)
							{
								foundtargetID = -1; targetdistance = 999;
								map_foreachinrange(targetnearest, targetbl2,9 + pc_checkskill(sd, AC_VULTURE), BL_MOB, sd);
								// knockback might hit monster outside range if further than this
								if (foundtargetID > -1) if (distance_bl(targetbl, &sd->bl) <= 10 ) {
									int area = 1; if (pc_checkskill(sd, AC_SHOWER) >= 6) area++;
									arrowchange(sd, targetmd);
									priority = map_foreachinrange(AOEPriority, targetbl, area, BL_MOB, skill_get_ele(AC_SHOWER, pc_checkskill(sd, AC_SHOWER)));
									if (((priority >= 6) && (priority > bestpriority))) {
										spelltocast = AC_SHOWER; bestpriority = priority; IDtarget = foundtargetID;
									}
								}
							}

							// NIN- Exploding Dragon
							// This is special - it targets a monster despite having AOE, not a ground skill
							if (canskill(sd)) if ((pc_checkskill(sd, NJ_BAKUENRYU) > 0)) if ((Dangerdistance > 900) || (sd->special_state.no_castcancel))
							if (pc_search_inventory(sd, 7521) >= 0) {
								if (pc_rightside_atk(sd) < sd->battle_status.matk_min) { 
									foundtargetID = -1; targetdistance = 999;
									map_foreachinrange(targetnearest, targetbl2, 9, BL_MOB, sd);
									if (foundtargetID > -1) {
										int area = 2;
										priority = 2 * map_foreachinrange(AOEPriority, targetbl, area, BL_MOB, skill_get_ele(NJ_BAKUENRYU, pc_checkskill(sd, NJ_BAKUENRYU)));
										if (((priority >= 12) && (priority > bestpriority)) && (distance_bl(targetbl, &sd->bl) <= 9)) {
											spelltocast = NJ_BAKUENRYU; bestpriority = priority; IDtarget = foundtargetID;
										}
									}
								}
							}

							// Throw Huuma
							if (canskill(sd)) if ((pc_checkskill(sd, NJ_HUUMA) >= 4))
								if (sd->status.weapon == W_HUUMA) {
								foundtargetID = -1; targetdistance = 999;
								map_foreachinrange(targetnearest, targetbl2, 9, BL_MOB, sd);
								if (foundtargetID > -1) {
								int area = 2;
								priority = 2 * map_foreachinrange(AOEPriority, targetbl, area, BL_MOB, skill_get_ele(NJ_HUUMA, pc_checkskill(sd, NJ_HUUMA)));
								if (((priority >= 12) && (priority > bestpriority)) && (distance_bl(targetbl, &sd->bl) <= 9)) {
									spelltocast = NJ_HUUMA; bestpriority = priority; IDtarget = foundtargetID;
								}
								}
							}

							// Magnus Exorcismus
							// **Note** Assumes it only works on Demons and Undead. If you want to include all enemies, replace Magnuspriority with AOEpriority
							if (canskill(sd)) if ((pc_checkskill(sd, PR_MAGNUS) > 0) && ((Dangerdistance > 900) || (sd->special_state.no_castcancel)) && (pc_search_inventory(sd, ITEMID_BLUE_GEMSTONE) >= 0)) {
								priority = 3 * map_foreachinrange(Magnuspriority, targetbl2, 3, BL_MOB, skill_get_ele(PR_MAGNUS, pc_checkskill(sd, PR_MAGNUS)));
								if ((priority >= 18) && (priority > bestpriority)) {
									spelltocast = PR_MAGNUS; bestpriority = priority; IDtarget = foundtargetID2;
								}
							}
							// Heaven's Drive
							if (canskill(sd)) if ((pc_checkskill(sd, WZ_HEAVENDRIVE) > 0) && (Dangerdistance > 900)) {
								int area = 2;
								priority = 1 + 2 * map_foreachinrange(AOEPriority, targetbl2, area, BL_MOB, skill_get_ele(WZ_HEAVENDRIVE, pc_checkskill(sd, WZ_HEAVENDRIVE)));
								if ((priority >= 13) && (priority > bestpriority)) {
									spelltocast = WZ_HEAVENDRIVE; bestpriority = priority; IDtarget = foundtargetID2;
								}
							}

						}
					}

					// Meteor Assault - always centered on user
					// **Note** uncomment below if you didn't make this skill uninterruptable like I did.
					if (canskill(sd)) if ((pc_checkskill(sd, ASC_METEORASSAULT) > 0)
						//		&& ((Dangerdistance > 900) || (sd->special_state.no_castcancel))
						) {
						int area = 2;
						priority = map_foreachinrange(AOEPriority, &sd->bl, area, BL_MOB, skill_get_ele(ASC_METEORASSAULT, pc_checkskill(sd, ASC_METEORASSAULT)));
						if ((priority >= 6) && (priority > bestpriority)) {
							spelltocast = ASC_METEORASSAULT; bestpriority = priority; IDtarget = sd->bl.id;
						}
					}

					// NIN Ice Meteor - always centered on user
					// **Note** : I modded this skill to be uninterruptable - a self targeted crowd control AOE is useless if it is interrupted. If yours is not modded, uncomment this line!
					if (canskill(sd)) if ((pc_checkskill(sd, NJ_HYOUSYOURAKU) >= 4)
						//					 &&	((Dangerdistance > 900) || (sd->special_state.no_castcancel)) 
						) {
						if (pc_search_inventory(sd, 7522) >= 0) {
							int area = 2;
							priority = 2 * map_foreachinrange(AOEPriorityIP, &sd->bl, area, BL_MOB, skill_get_ele(NJ_HYOUSYOURAKU, pc_checkskill(sd, NJ_HYOUSYOURAKU)));
							if ((priority >= 12) && (priority > bestpriority)) {
								spelltocast = NJ_HYOUSYOURAKU; bestpriority = priority; IDtarget = sd->bl.id;
							}
						}
					}

					// Desperado - always centered on user
					if (canskill(sd)) if (pc_checkskill(sd, GS_DESPERADO) > 0) if (sd->status.weapon == W_REVOLVER) {
						int area = 3;
						// Ammo? But is AOE we don't have a target to pick an element
						// Let's assume we already have some ammo equipped I guess, from using other skills
						// In worst case it fails and the AI uses the other skills anyway.
						priority = map_foreachinrange(AOEPriority, &sd->bl, area, BL_MOB, skill_get_ele(GS_DESPERADO, pc_checkskill(sd, GS_DESPERADO)));
						if ((priority >= 6) && (priority > bestpriority)) {
							spelltocast = GS_DESPERADO; bestpriority = priority; IDtarget = sd->bl.id;
						}
					}


					// Cast the chosen spell
					if (spelltocast > -1) {
						if ((spelltocast == NJ_HYOUSYOURAKU) // Skills that target SELF, not the ground 
							|| (spelltocast == ASC_METEORASSAULT)
							|| (spelltocast == GS_DESPERADO)
							) unit_skilluse_ifable(&sd->bl, SELF, spelltocast, pc_checkskill(sd, spelltocast));
						else
						if ((spelltocast == MG_FIREBALL) // Skills that target a monster, not the ground 
							|| (spelltocast == NJ_HUUMA)
							|| (spelltocast == NJ_BAKUENRYU)
							|| (spelltocast == NJ_KAMAITACHI)
							|| (spelltocast == AC_SHOWER)
							|| (spelltocast == AB_JUDEX)
							|| (spelltocast == AB_ADORAMUS)
							|| (spelltocast == GS_SPREADATTACK)
							|| (spelltocast == HT_BLITZBEAT)
							|| (spelltocast == SN_SHARPSHOOTING)
							) unit_skilluse_ifable(&sd->bl, IDtarget, spelltocast, pc_checkskill(sd, spelltocast));
						else
							unit_skilluse_ifablexy(&sd->bl, IDtarget, spelltocast, pc_checkskill(sd, spelltocast));
					}
				}
		}
		// Absorb Spirit Sphere to gain SP instead of attacking?
		// Only if below 20% SP
		if (canskill(sd)) if (pc_checkskill(sd, MO_ABSORBSPIRITS) > 0) if ((sd->state.autopilotmode == 2) && (Dangerdistance > 900)) 
			if (sd->battle_status.sp<0.2*sd->battle_status.max_sp) {
				resettargets2();
				map_foreachinrange(targethighestlevel, &sd->bl, 9, BL_MOB, sd);
				if ((foundtargetID > -1) && (targetdistance>=50)){
					unit_skilluse_ifable(&sd->bl, foundtargetID, MO_ABSORBSPIRITS, pc_checkskill(sd, MO_ABSORBSPIRITS));
				}

		}
		// Turn Undead, has special targeting restriction
		if (canskill(sd)) if (pc_checkskill(sd, PR_TURNUNDEAD) > 0) if (sd->state.autopilotmode == 2) {
			resettargets();
			map_foreachinrange(targetturnundead, &sd->bl, 9, BL_MOB, sd);
			if (foundtargetID > -1){
				unit_skilluse_ifable(&sd->bl, foundtargetID, PR_TURNUNDEAD, pc_checkskill(sd, PR_TURNUNDEAD));
			}
		}

		// Eska
		// Don't use if party relies on physical atk more than magical
		if (canskill(sd)) if (pc_checkskill(sd, SL_SKA) > 0) if (partymagicratio>0) {
			resettargets(); targetdistance = 0;
			map_foreachinrange(targeteska, &sd->bl, 9, BL_MOB, sd);
			if (foundtargetID > -1) {
				unit_skilluse_ifable(&sd->bl, foundtargetID, SL_SKA, pc_checkskill(sd, SL_SKA));
			}
		}

		// get target for single target spells only once - pick best skill to use on nearest enemy, not pick best enemy for best skill.
		// probably could do better but targeting too many times causes lags as it includes finding paths.
		/// Also fetch target for skills blocked by Pneuma separately
		resettargets();
		map_foreachinrange(targetnearestusingranged, &sd->bl, AUTOPILOT_RANGE_CAP, BL_MOB, sd);
		int foundtargetRA = foundtargetID;
		struct block_list * targetRAbl = targetbl;
		struct mob_data * targetRAmd = targetmd;
		int rangeddist = targetdistance;
		resettargets();
		map_foreachinrange(targetnearest, &sd->bl, 9, BL_MOB, sd);
		int foundtargetID2 = foundtargetID;
		int targetdistance2 = targetdistance;

		// Hunter TRAPS
		// These don't go into the AOEs because they are melee range and not interruptable
		// **Note** I modded these to be valid to place under enemies. If you did not, remove this entire block!
		// Use ranged skill instead if enemy isn't near enough
		if (foundtargetID2 > -1) if (sd->state.autopilotmode < 3)
		if (canskill(sd))
		if (pc_search_inventory(sd, ITEMID_TRAP) >= 0) {

			// Use Sandman for crowd control if mobbed
			// ...or not, I think placing a trap that kills the mob is better in most cases.
/*			if ((pc_checkskill(sd, HT_SANDMAN) >= 3) && (dangercount>=4)
				&& (distance_bl(dangerbl, &sd->bl) <= 3))
			{
				int area = 1;
				// At least one weak or multiple other targets to use
				if (map_foreachinrange(AOEPrioritySandman, dangerbl, area, BL_MOB, ELE_NONE)>=6)
				unit_skilluse_ifablexy(&sd->bl, founddangerID, HT_SANDMAN, pc_checkskill(sd, HT_SANDMAN));
			}*/

		// Need INT or no bow equipped! Otherwise just shoot at things, is better?
		if (distance_bl(targetbl, &sd->bl) <= 3) if ((sd->battle_status.int_>=50) || (sd->status.weapon != W_BOW)) {
			int spelltocast = -1;
			int bestpriority = -1;
			int priority;
			int IDtarget = -1;

			if ((pc_checkskill(sd, HT_CLAYMORETRAP) > 4))
				{	int area = 2;
				// At least one weak or multiple other targets to use
				priority = map_foreachinrange(AOEPriority, targetbl, area, BL_MOB, skill_get_ele(HT_CLAYMORETRAP, pc_checkskill(sd, HT_CLAYMORETRAP)));
				if ((priority >= 3) && (priority > bestpriority)) {
						spelltocast = HT_CLAYMORETRAP; bestpriority = priority; IDtarget = sd->bl.id;
					}
			}

			if ((pc_checkskill(sd, HT_LANDMINE) > 4))
			{
				int area = 1;
				priority = map_foreachinrange(AOEPriority, targetbl, area, BL_MOB, skill_get_ele(HT_LANDMINE, pc_checkskill(sd, HT_LANDMINE)));
				if ((priority >= 3) && (priority > bestpriority)) {
					spelltocast = HT_LANDMINE; bestpriority = priority; IDtarget = sd->bl.id;
				}
			}

			if ((pc_checkskill(sd, HT_BLASTMINE) > 4))
			{
				int area = 1;
				priority = map_foreachinrange(AOEPriority, targetbl, area, BL_MOB, skill_get_ele(HT_BLASTMINE, pc_checkskill(sd, HT_BLASTMINE)));
				if ((priority >= 3) && (priority > bestpriority)) {
					spelltocast = HT_BLASTMINE; bestpriority = priority; IDtarget = sd->bl.id;
				}
			}

			// **Note** I changed this skill to deal 70% of the damage the other 3 element traps do
			// If yours still does the default, super low damage, remove this block.
			if ((pc_checkskill(sd, HT_FREEZINGTRAP) > 4))
			{
				int area = 1;
				priority = map_foreachinrange(AOEPriority, targetbl, area, BL_MOB, skill_get_ele(HT_FREEZINGTRAP, pc_checkskill(sd, HT_FREEZINGTRAP)));
				if ((priority >= 3) && (priority > bestpriority)) {
					spelltocast = HT_FREEZINGTRAP; bestpriority = priority; IDtarget = sd->bl.id;
				}
			}

			if (spelltocast>-1) unit_skilluse_ifablexy(&sd->bl, foundtargetID2, spelltocast, pc_checkskill(sd, spelltocast));
		}


		}

		// Full Strip
		// Use on bosses or much higher level enemies.
		if (foundtargetID2 > -1) if ((status_get_class_(bl) == CLASS_BOSS) || (targetmd->level > sd->status.base_level + 30))
			if (canskill(sd)) if (pc_checkskill(sd, ST_FULLSTRIP) > 4)
				// Don't bother if we already stipped something.
				if (!(targetmd->sc.data[SC_STRIPHELM] || targetmd->sc.data[SC_STRIPSHIELD] || targetmd->sc.data[SC_STRIPWEAPON] || targetmd->sc.data[SC_STRIPARMOR]))
					// Don't bother with targets that have some protection
					if (!(targetmd->sc.data[SC_CP_WEAPON] || targetmd->sc.data[SC_CP_HELM] || targetmd->sc.data[SC_CP_ARMOR] || targetmd->sc.data[SC_CP_SHIELD]))
						// Must have at least enough DEX for 10% chance - nevermind, DEX can only add a bonus, but not reduce the chance
		//				if (sd->battle_status.dex>=targetmd->status.dex-25)
					{
						if (targetdistance2 > 1) {
							struct walkpath_data wpd1;
							if (path_search(&wpd1, sd->bl.m, bl->x, bl->y, targetbl->x, targetbl->y, 0, CELL_CHKNOPASS, MAX_WALKPATH))
								newwalk(&sd->bl, bl->x + dirx[wpd1.path[0]], bl->y + diry[wpd1.path[0]], 8);
							return 0;
						}
						else
							// Assumes the update to allow using this from the front is already included
							unit_skilluse_ifable(&sd->bl, foundtargetID2, ST_FULLSTRIP, pc_checkskill(sd, ST_FULLSTRIP));
						return 0;
					}

		// Rogue - use while hiding type skills
		// Don't even think about it without maxed Tunnel Drive or if in tanking mode
		if (pc_checkskill(sd, RG_TUNNELDRIVE) >= 5) if (foundtargetID2 > -1)
		if (sd->state.autopilotmode == 2) {
		// Hide when targeted near leader
			if (pc_checkskill(sd, TF_HIDING) > 0) if (sd->battle_status.sp > 50) if (canskill(sd))
				if (leaderdistance<8) if (Dangerdistance<=3) if (!sd->sc.data[SC_HIDING])
					unit_skilluse_ifable(&sd->bl, SELF, TF_HIDING, pc_checkskill(sd, TF_HIDING));
		// Unhide if far from leader to walk faster and catch up, or if bow rogue
			if (sd->sc.data[SC_HIDING]) {
				if (pc_checkskill(sd, TF_HIDING) > 0) if ((leaderdistance>12) || (sd->status.weapon == W_BOW)) if (canskill(sd))
				unit_skilluse_ifable(&sd->bl, SELF, TF_HIDING, pc_checkskill(sd, TF_HIDING));
				// Move to enemy while hidden
				if (targetdistance2 > 1) {
					struct walkpath_data wpd1;
					if (path_search(&wpd1, sd->bl.m, bl->x, bl->y, targetbl->x, targetbl->y, 0, CELL_CHKNOPASS, MAX_WALKPATH))
						newwalk(&sd->bl, bl->x + dirx[wpd1.path[0]], bl->y + diry[wpd1.path[0]], 8);
					return 0;
				}

				// Raid, use if able to hit at least three targets
				// ** Note ** I modded this to hit an aoe of 4, even though the update should have reduced it to 2. Change that number if you did not.
				if (canskill(sd)) if (pc_checkskill(sd, RG_RAID) > 0)
					if (6<=map_foreachinrange(AOEPriority, &sd->bl, 4, BL_MOB, skill_get_ele(RG_RAID, pc_checkskill(sd, RG_RAID))))
						unit_skilluse_ifable(&sd->bl, SELF, RG_RAID, pc_checkskill(sd, RG_RAID));
			}
		}
		// Backstab
		// Use this if we're not bow rogues, and/or already hiding.
		// Yes, this is a melee skill that can be used outside tanking mode. The Rogue can hide in reaction and avoid harm.
		if ((sd->sc.data[SC_HIDING]) || (sd->status.weapon != W_BOW))
		if (foundtargetID2 > -1)
		if (canskill(sd)) if (pc_checkskill(sd, RG_BACKSTAP) > 0) if (sd->state.autopilotmode == 2) {
			if (targetdistance2 > 1) {
				struct walkpath_data wpd1;
				if (path_search(&wpd1, sd->bl.m, bl->x, bl->y, targetbl->x, targetbl->y, 0, CELL_CHKNOPASS, MAX_WALKPATH))
					newwalk(&sd->bl, bl->x + dirx[wpd1.path[0]], bl->y + diry[wpd1.path[0]], 8);
				return 0;
			} else
			// Assumes the update to allow using this from the front is already included
			unit_skilluse_ifable(&sd->bl, foundtargetID2, RG_BACKSTAP, pc_checkskill(sd, RG_BACKSTAP));
		}

		/// Charge Arrow
		/// Repel extremely close enemy
		/// Avoid if not in danger of getting hit due to high delay
		/// **Note** : this skill was customized to have no cast time and high delay
		if (canskill(sd)) if (pc_checkskill(sd, AC_CHARGEARROW) > 0) {
			if (Dangerdistance <= 2) 
				if (!isdisabled(dangermd))
					if (rangeddist <= 9 + pc_checkskill(sd, AC_VULTURE)) {
						if (sd->status.weapon == W_BOW)
							if (!((status_get_class_(dangerbl) == CLASS_BOSS))) {
								arrowchange(sd, targetRAmd);
								unit_skilluse_ifable(&sd->bl, founddangerID, AC_CHARGEARROW, pc_checkskill(sd, AC_CHARGEARROW));
							}
					}
			}

		// Estin, Estun, Esma on vulnerable enemy
		int windelem = 0;
		if (sd->sc.data[SC_SEVENWIND]) windelem= skill_get_ele(TK_SEVENWIND, sd->sc.data[SC_SEVENWIND]->val1);
		if (foundtargetID2 > -1) if (canskill(sd))
			if (elemstrong(targetmd, windelem)) {

				if (sd->sc.data[SC_SMA]) if (pc_checkskill(sd, SL_SMA) > 0) {
					if (((sd->state.autopilotmode == 2)) && (Dangerdistance > 900)) {
						unit_skilluse_ifable(&sd->bl, foundtargetID2, SL_SMA, pc_checkskill(sd, SL_SMA));
					}
				}
				if (pc_checkskill(sd, SL_STIN) > 0) if (targetmd->status.size == SZ_SMALL) {
					if (((sd->state.autopilotmode == 2)) && (Dangerdistance > 900)) {
						unit_skilluse_ifable(&sd->bl, foundtargetID2, SL_STIN, pc_checkskill(sd, SL_STIN));
					}
				}
				if (pc_checkskill(sd, SL_STUN) > 0) {
					if (((sd->state.autopilotmode == 2)) && (Dangerdistance > 900)) {
						unit_skilluse_ifable(&sd->bl, foundtargetID2, SL_STUN, pc_checkskill(sd, SL_STUN));
					}
				}
			}

		// Jupitel Thunder on vulnerable enemy
		if (foundtargetID2 > -1) if (canskill(sd)) if (pc_checkskill(sd, WZ_JUPITEL) > 0) {
				if (((sd->state.autopilotmode == 2)) && (Dangerdistance > 900)) {
					if (elemstrong(targetmd, skill_get_ele(WZ_JUPITEL, pc_checkskill(sd, WZ_JUPITEL)))) {
						unit_skilluse_ifable(&sd->bl, foundtargetID2, WZ_JUPITEL, pc_checkskill(sd, WZ_JUPITEL));
					}
				}
			}
			// Napalm Vulcan on vulnerable enemy
		if (foundtargetID2 > -1) if (canskill(sd)) if (pc_checkskill(sd, HW_NAPALMVULCAN) > 0) {
				if (((sd->state.autopilotmode == 2)) && (Dangerdistance > 900)) {
					if (elemstrong(targetmd, skill_get_ele(HW_NAPALMVULCAN, pc_checkskill(sd, HW_NAPALMVULCAN)))) {
						unit_skilluse_ifable(&sd->bl, foundtargetID2, HW_NAPALMVULCAN, pc_checkskill(sd, HW_NAPALMVULCAN));
					}
				}
			}
			// Earth Spike on vulnerable enemy
		if (foundtargetID2 > -1) if (canskill(sd)) if (pc_checkskill(sd, WZ_EARTHSPIKE) > 0) {
				if (((sd->state.autopilotmode == 2)) && (Dangerdistance > 900)) {
					if (elemstrong(targetmd, skill_get_ele(WZ_EARTHSPIKE, pc_checkskill(sd, WZ_EARTHSPIKE)))) {
						unit_skilluse_ifable(&sd->bl, foundtargetID2, WZ_EARTHSPIKE, pc_checkskill(sd, WZ_EARTHSPIKE));
					}
				}
			}
			// Fire Bolt on vulnerable enemy
		if (foundtargetID2 > -1) if (canskill(sd)) if (pc_checkskill(sd, MG_FIREBOLT) > 0) {
				if (((sd->state.autopilotmode == 2)) && (Dangerdistance > 900)) {
					if (elemstrong(targetmd, skill_get_ele(MG_FIREBOLT, pc_checkskill(sd, MG_FIREBOLT)))) {
						unit_skilluse_ifable(&sd->bl, foundtargetID2, MG_FIREBOLT, pc_checkskill(sd, MG_FIREBOLT));
					}
				}
			}
			// NIN : Crimson Fire Petal
		if (foundtargetID2 > -1) if (canskill(sd)) if (pc_checkskill(sd, NJ_KOUENKA) > 0)
			if (pc_rightside_atk(sd)*1.2 <sd->battle_status.matk_min) { // Note : if skill is unmodded, a higher multiplier is needed
				if (((sd->state.autopilotmode == 2)) && (Dangerdistance > 900)) {
					if (elemstrong(targetmd, skill_get_ele(NJ_KOUENKA, pc_checkskill(sd, NJ_KOUENKA)))) {
						unit_skilluse_ifable(&sd->bl, foundtargetID2, NJ_KOUENKA, pc_checkskill(sd, NJ_KOUENKA));
					}
				}
			}
			// Cold Bolt on vulnerable enemy
		if (foundtargetID2 > -1) if (canskill(sd)) if (pc_checkskill(sd, MG_COLDBOLT) > 0) {
				if (((sd->state.autopilotmode == 2)) && (Dangerdistance > 900)) {
					if (elemstrong(targetmd, skill_get_ele(MG_COLDBOLT, pc_checkskill(sd, MG_COLDBOLT)))) {
						unit_skilluse_ifable(&sd->bl, foundtargetID2, MG_COLDBOLT, pc_checkskill(sd, MG_COLDBOLT));
					}
				}
			}
			// NIN : Spear of Ice
			if (foundtargetID2 > -1) if (canskill(sd)) if (pc_checkskill(sd, NJ_HYOUSENSOU) > 0)
				if (pc_rightside_atk(sd)*1.2 < sd->battle_status.matk_min) { // Note : if skill is unmodded, a higher multiplier is needed
				if (((sd->state.autopilotmode == 2)) && (Dangerdistance > 900)) {
					if (elemstrong(targetmd, skill_get_ele(NJ_HYOUSENSOU, pc_checkskill(sd, NJ_HYOUSENSOU)))) {
						unit_skilluse_ifable(&sd->bl, foundtargetID2, NJ_HYOUSENSOU, pc_checkskill(sd, NJ_HYOUSENSOU));
					}
				}
			}
			// Lightning Bolt on vulnerable enemy
			if (foundtargetID2 > -1) if (canskill(sd)) if (pc_checkskill(sd, MG_LIGHTNINGBOLT) > 0) {
				if (((sd->state.autopilotmode == 2)) && (Dangerdistance > 900)) {
					if (elemstrong(targetmd, skill_get_ele(MG_LIGHTNINGBOLT, pc_checkskill(sd, MG_LIGHTNINGBOLT)))) {
						unit_skilluse_ifable(&sd->bl, foundtargetID2, MG_LIGHTNINGBOLT, pc_checkskill(sd, MG_LIGHTNINGBOLT));
					}
				}
			}
			// NIN : Wind Blade
			if (foundtargetID2 > -1) if (canskill(sd)) if (pc_checkskill(sd, NJ_HUUJIN) > 0)
				if (pc_rightside_atk(sd)*1.2 < sd->battle_status.matk_min) { // Note : if skill is unmodded, a higher multiplier is needed
				if (((sd->state.autopilotmode == 2)) && (Dangerdistance > 900)) {
					if (elemstrong(targetmd, skill_get_ele(NJ_HUUJIN, pc_checkskill(sd, NJ_HUUJIN)))) {
						unit_skilluse_ifable(&sd->bl, foundtargetID2, NJ_HUUJIN, pc_checkskill(sd, NJ_HUUJIN));
					}
				}
			}
			// Soul Strike on vulnerable enemy
			if (foundtargetID2 > -1) if (canskill(sd)) if (pc_checkskill(sd, MG_SOULSTRIKE) > 0) {
				if (((sd->state.autopilotmode == 2)) && (Dangerdistance > 900)) {
					if (elemstrong(targetmd, skill_get_ele(MG_SOULSTRIKE, pc_checkskill(sd, MG_SOULSTRIKE)))) {
						unit_skilluse_ifable(&sd->bl, foundtargetID2, MG_SOULSTRIKE, pc_checkskill(sd, MG_SOULSTRIKE));
					}
				}
			}
			///////////////////////////////////////////////////////////////////////////////////////////////
			/// Skills for general use
			///////////////////////////////////////////////////////////////////////////////////////////////

			// Falcon Assault
			// Double Strafe does more damage in less time for less SP no thanks to the high delay
			// making this skill useless, even with higher INT, unless you are not wearing a bow.
			if (foundtargetRA > -1) if (canskill(sd)) if ((pc_checkskill(sd, SN_FALCONASSAULT) > 0)) if (sd->state.autopilotmode != 3)
				if (pc_isfalcon(sd))
				// Don't bother if equipped by a good bow or having low INT
				// **Note** I modded my skill delay to be slightly lower.
				// If you did not, you should disable the skill when a bow with any half decent (read 100 will do) atk is equipped.
				if ((sd->battle_status.rhw.atk>=sd->battle_status.int_*1.5) || (sd->status.weapon != W_BOW))
					if (rangeddist <= 3 + pc_checkskill(sd, AC_VULTURE)) {
						if ((targetRAmd->status.hp > (12 - (sd->battle_status.sp * 10 / sd->battle_status.max_sp)) * pc_rightside_atk(sd))
							|| (status_get_hp(bl) < status_get_max_hp(bl) / 3)) {
							unit_skilluse_ifable(&sd->bl, foundtargetRA, SN_FALCONASSAULT, pc_checkskill(sd, SN_FALCONASSAULT));
						}
				}

			// Beast Strafing
			// **Note** I changed this skill to work on any monster. If you did not, you have to add a check for the target's race here!
			if (foundtargetRA > -1) if (canskill(sd)) if ((pc_checkskill(sd, HT_POWER) > 0)) if (sd->state.autopilotmode != 3)
				if (rangeddist <= 9 + pc_checkskill(sd, AC_VULTURE))
					if ((sd->sc.data[SC_COMBO]) && (sd->sc.data[SC_COMBO]->val1 == AC_DOUBLE))
					{
					if (sd->battle_status.str>=35) // need STR to use this!
					if (sd->status.weapon == W_BOW)
						{
							arrowchange(sd, targetRAmd);
							unit_skilluse_ifable(&sd->bl, foundtargetRA, HT_POWER, pc_checkskill(sd, HT_POWER));
						}
				}

			// Double Strafe
			if (foundtargetRA > -1) if (canskill(sd)) if ((pc_checkskill(sd, AC_DOUBLE) > 0)) if (sd->state.autopilotmode != 3)
			if (rangeddist<= 9 + pc_checkskill(sd, AC_VULTURE)) {
				if (sd->status.weapon == W_BOW)
					if ((targetRAmd->status.hp > (12 - (sd->battle_status.sp * 10 / sd->battle_status.max_sp)) * pc_rightside_atk(sd))
						|| (status_get_hp(bl) < status_get_max_hp(bl) / 3)) {
						arrowchange(sd, targetRAmd);
						unit_skilluse_ifable(&sd->bl, foundtargetRA, AC_DOUBLE, pc_checkskill(sd, AC_DOUBLE));
					}
			}

			// Bull's Eye
			// Note : I removed the casting time and reduced the delay on this, without those modifications it's better if the AI doesn't use this skill.
			if (foundtargetRA > -1) if (canskill(sd)) if ((pc_checkskill(sd, GS_BULLSEYE) > 0)) if (sd->state.autopilotmode != 3) {
				if (rangeddist <= 9) if (hasgun(sd)) if (sd->spiritball >= 1)
					if ((targetRAmd->status.race == RC_DEMIHUMAN) || (targetRAmd->status.race == RC_BRUTE)) {
						ammochange2(sd, targetRAmd);
						unit_skilluse_ifable(&sd->bl, foundtargetRA, GS_BULLSEYE, pc_checkskill(sd, GS_BULLSEYE));
					}
			}

			// Tracking
			if (foundtargetRA > -1) if (canskill(sd)) if ((pc_checkskill(sd, GS_TRACKING) > 0)) if (sd->state.autopilotmode != 3) {
				if (rangeddist <= 9) if (((sd->status.weapon == W_REVOLVER) && (pc_checkskill(sd, GS_RAPIDSHOWER)*2 <= pc_checkskill(sd, GS_TRACKING))) || (sd->status.weapon == W_RIFLE)) {
					ammochange2(sd, targetRAmd);
					unit_skilluse_ifable(&sd->bl, foundtargetRA, GS_TRACKING, pc_checkskill(sd, GS_TRACKING));
				}
			}

			// Full Buster
			if (foundtargetRA > -1) if (canskill(sd)) if ((pc_checkskill(sd, GS_FULLBUSTER) > 0)) if (sd->state.autopilotmode != 3) {
				if (rangeddist <= 9 + pc_checkskill(sd, GS_SNAKEEYE)) if ((sd->status.weapon == W_SHOTGUN)) {
					ammochange2(sd, targetRAmd);
					unit_skilluse_ifable(&sd->bl, foundtargetRA, GS_FULLBUSTER, pc_checkskill(sd, GS_FULLBUSTER));
				}
			}

			// Piercing Shot
			// Higher range than tracking so useful for very distant enemies when using a Rifle.
			// Otherwise don't bother, damage is way too low
			// Would be useful for armor piercing maybe but under typical uses I don't think monsters ever have that much armor to make it worth doing.
			// Actually do not use. multiple normal attacks will be more effective and will aggro the monster so it comes into range of higher damage skills.
			// Normal attacks do benefit from the extra range this skill gets, afterall.
			/*if (foundtargetRA > -1) if (canskill(sd)) if ((pc_checkskill(sd, GS_PIERCINGSHOT) > 0)) if (sd->state.autopilotmode != 3) {
				if (rangeddist <= 9 + pc_checkskill(sd, GS_SNAKEEYE)) if (rangeddist>15) if ((sd->status.weapon == W_RIFLE)) {
					ammochange2(sd, targetRAmd);
					unit_skilluse_ifable(&sd->bl, foundtargetRA, GS_PIERCINGSHOT, pc_checkskill(sd, GS_PIERCINGSHOT));
				}
			}*/

			// Rapid Shower
			if (foundtargetRA > -1) if (canskill(sd)) if ((pc_checkskill(sd, GS_RAPIDSHOWER) > 0)) if (sd->state.autopilotmode != 3) {
				if (rangeddist <= 9 + pc_checkskill(sd, GS_SNAKEEYE)) if (sd->status.weapon == W_REVOLVER)
				if ((Dangerdistance > 900) || (sd->special_state.no_castcancel)) {
					ammochange2(sd, targetRAmd);
					unit_skilluse_ifable(&sd->bl, foundtargetRA, GS_RAPIDSHOWER, pc_checkskill(sd, GS_RAPIDSHOWER));
				}
			}

			// Triple Action
			if (canskill(sd)) if ((pc_checkskill(sd, GS_TRIPLEACTION) > 0)) if (sd->state.autopilotmode != 3) {
				if (foundtargetRA > -1) if (rangeddist <= 9) if (hasgun(sd)) if (sd->spiritball >= 1) {
					ammochange2(sd, targetRAmd);
					unit_skilluse_ifable(&sd->bl, foundtargetRA, GS_TRIPLEACTION, pc_checkskill(sd, GS_TRIPLEACTION));
				}
			}

			// Finger Offensive
			if (foundtargetRA > -1) if (canskill(sd)) if ((pc_checkskill(sd, MO_FINGEROFFENSIVE) > 0)) if (sd->spiritball >= pc_checkskill(sd, MO_FINGEROFFENSIVE)) {
				if (rangeddist <= 9) if ((sd->state.autopilotmode == 2)) {
						unit_skilluse_ifable(&sd->bl, foundtargetRA, MO_FINGEROFFENSIVE, pc_checkskill(sd, MO_FINGEROFFENSIVE));
				}
			}
			// Soul Breaker
			if (foundtargetRA > -1) if (canskill(sd)) if ((pc_checkskill(sd, ASC_BREAKER) > 0)){
				if (rangeddist <= 9) if ((sd->state.autopilotmode == 2)) {
					unit_skilluse_ifable(&sd->bl, foundtargetRA, ASC_BREAKER, pc_checkskill(sd, ASC_BREAKER));
				}
			}

			// Throw Kunai
			if (foundtargetRA > -1) if (canskill(sd)) if ((pc_checkskill(sd, NJ_KUNAI) > 0))
				// If enemy has less than 2x ATK health left, more economic to use Shurikens.
				if (rangeddist <= 9) if (targetRAmd->status.hp>2* pc_rightside_atk(sd)) {
				if (kunaichange(sd, targetRAmd)==1) unit_skilluse_ifable(&sd->bl, foundtargetRA, NJ_KUNAI, pc_checkskill(sd, NJ_KUNAI));
			}

			// Flying Kick
			if ((sd->class_ & MAPID_UPPERMASK)!= MAPID_SOUL_LINKER) if (foundtargetID2 > -1) if (canskill(sd)) if ((pc_checkskill(sd, TK_JUMPKICK) > 0)) {
				// only use in tanking mode, if enemy is not already near!
				if (targetdistance2>2)
				if ((sd->state.autopilotmode == 1)) {
					unit_skilluse_ifable(&sd->bl, foundtargetID2, TK_JUMPKICK, pc_checkskill(sd, TK_JUMPKICK));
				}
			}


			// Shield Boomerang
			if (foundtargetRA > -1) if (canskill(sd)) if ((pc_checkskill(sd, CR_SHIELDBOOMERANG) > 0)) if (sd->status.shield > 0) {
				// not really strong enough to use if aleady engaged in melee in tanking mode
				if (rangeddist <= 9) if ((sd->state.autopilotmode == 2)) {
						unit_skilluse_ifable(&sd->bl, foundtargetRA, CR_SHIELDBOOMERANG, pc_checkskill(sd, CR_SHIELDBOOMERANG));
				}
			}
			// Spear Boomerang
			if (foundtargetRA > -1) if (canskill(sd)) if ((pc_checkskill(sd, KN_SPEARBOOMERANG) > 0))
				if ((sd->status.weapon == W_1HSPEAR) || (sd->status.weapon == W_2HSPEAR)) {
				// not really strong enough to use if aleady engaged in melee in tanking mode
				if (rangeddist <= 9) if ((sd->state.autopilotmode == 2)) {
					unit_skilluse_ifable(&sd->bl, foundtargetRA, KN_SPEARBOOMERANG, pc_checkskill(sd, KN_SPEARBOOMERANG));
				}
			}
			// Pressure
			// Only use if very low STR or having no equipped shield -> can't use Shield Chain effectively.
			// Uninterruptable so ok during tanking mode but in that case, don't use up all the SP, keep most of it for tanking skills like healing.
			// Characters with at least some STR are probably better off using their normal attack or bash in most cases, and Shield Chain if available.
			// also use against super high DEF targets
			if (foundtargetID2 > -1) if (canskill(sd)) if ((pc_checkskill(sd, PA_PRESSURE) > 1)) if ((sd->status.shield <= 0) || (sd->battle_status.str<30) || (targetmd->status.def+targetmd->status.def2>=500))
				if ((sd->state.autopilotmode == 2) || ((sd->state.autopilotmode == 1) && (sd->battle_status.sp >= 0.6*sd->battle_status.max_sp))){
				unit_skilluse_ifable(&sd->bl, foundtargetID2, PA_PRESSURE, pc_checkskill(sd, PA_PRESSURE));
			}
			// Shield Chain
			if (foundtargetRA > -1) if (canskill(sd)) if ((pc_checkskill(sd, PA_SHIELDCHAIN) > 0)) if (sd->status.shield > 0) {
				// casting time is interruptable so bad for tanking mode. Tanking mode should preserve sp for healing anyway.
				if (elemallowed(targetRAmd, ELE_NEUTRAL))
					if (rangeddist <= 9) if ((sd->state.autopilotmode == 2)) {
						unit_skilluse_ifable(&sd->bl, foundtargetRA, PA_SHIELDCHAIN, pc_checkskill(sd, PA_SHIELDCHAIN));
				}
			}

			// Spiral Pierce
			if (foundtargetRA > -1) if (canskill(sd)) if ((pc_checkskill(sd, LK_SPIRALPIERCE) > 0))  {
				// Unlike Paladin, this class can't heal so ok to use up SP even if in tanking mode, but skill is interruptable so be careful of that
				if (rangeddist <= 9) if (elemallowed(targetRAmd, ELE_NEUTRAL))
					if ((sd->state.autopilotmode == 2) || ((Dangerdistance > 900) || (sd->special_state.no_castcancel)))
					{
						unit_skilluse_ifable(&sd->bl, foundtargetRA, LK_SPIRALPIERCE, pc_checkskill(sd, LK_SPIRALPIERCE));
					}
			}

			// Estin, Estun, Esma
			windelem = 0;
			if (sd->sc.data[SC_SEVENWIND]) windelem = skill_get_ele(TK_SEVENWIND, sd->sc.data[SC_SEVENWIND]->val1);
			if (foundtargetID2 > -1) if (canskill(sd))
				if (elemallowed(targetmd, windelem)) {

					if (sd->sc.data[SC_SMA]) if (pc_checkskill(sd, SL_SMA) > 0) {
						if (((sd->state.autopilotmode == 2)) && (Dangerdistance > 900)) {
							unit_skilluse_ifable(&sd->bl, foundtargetID2, SL_SMA, pc_checkskill(sd, SL_SMA));
						}
					}
					if (pc_checkskill(sd, SL_STIN) > 0) if (targetmd->status.size == SZ_SMALL) {
						if (((sd->state.autopilotmode == 2)) && (Dangerdistance > 900)) {
							unit_skilluse_ifable(&sd->bl, foundtargetID2, SL_STIN, pc_checkskill(sd, SL_STIN));
						}
					}
					if (pc_checkskill(sd, SL_STUN) > 0) {
						if (((sd->state.autopilotmode == 2)) && (Dangerdistance > 900)) {
							unit_skilluse_ifable(&sd->bl, foundtargetID2, SL_STUN, pc_checkskill(sd, SL_STUN));
						}
					}
				}

			// Jupitel Thunder
			if (foundtargetID2 > -1) if (canskill(sd)) if ((pc_checkskill(sd, WZ_JUPITEL) > 0)) {
				if ((sd->state.autopilotmode == 2) && (Dangerdistance > 900)) {
					if (elemallowed(targetmd, skill_get_ele(WZ_JUPITEL, pc_checkskill(sd, WZ_JUPITEL)))) {
						unit_skilluse_ifable(&sd->bl, foundtargetID2, WZ_JUPITEL, pc_checkskill(sd, WZ_JUPITEL));
					}
				}
			}
			// Napalm Vulcan
			if (foundtargetID2 > -1) if (canskill(sd)) if ((pc_checkskill(sd, HW_NAPALMVULCAN) > 0)) {
				if ((sd->state.autopilotmode == 2)) {
					if (elemallowed(targetmd, skill_get_ele(HW_NAPALMVULCAN, pc_checkskill(sd, HW_NAPALMVULCAN)))) {
						unit_skilluse_ifable(&sd->bl, foundtargetID2, HW_NAPALMVULCAN, pc_checkskill(sd, HW_NAPALMVULCAN));
					}
				}
			}
			// Earth Spike, prefer over bolts if no weakess/strength applies
			// As it's 200% DMG per hit so less affected by MDEF
			if (foundtargetID2 > -1) if (canskill(sd)) if (pc_checkskill(sd, WZ_EARTHSPIKE) >= 5) {
				if (((sd->state.autopilotmode == 2)) && (Dangerdistance > 900)) {
					if (elemallowed(targetmd, skill_get_ele(WZ_EARTHSPIKE, pc_checkskill(sd, WZ_EARTHSPIKE)))) {
						unit_skilluse_ifable(&sd->bl, foundtargetID2, WZ_EARTHSPIKE, pc_checkskill(sd, WZ_EARTHSPIKE));
					}
				}
			}
			// bolts, use highest level
			if (foundtargetID2 > -1) if (canskill(sd)) if ((pc_checkskill(sd, MG_FIREBOLT) > 0) && (pc_checkskill(sd, MG_FIREBOLT) >= pc_checkskill(sd, MG_COLDBOLT))
				&& (pc_checkskill(sd, MG_FIREBOLT) >= pc_checkskill(sd, MG_LIGHTNINGBOLT))) {
				if ((sd->state.autopilotmode == 2) && (Dangerdistance > 900)) {
					if (elemallowed(targetmd, skill_get_ele(MG_FIREBOLT, pc_checkskill(sd, MG_FIREBOLT)))) {
						unit_skilluse_ifable(&sd->bl, foundtargetID2, MG_FIREBOLT, pc_checkskill(sd, MG_FIREBOLT));
					}
				}
			}
			if (foundtargetID2 > -1) if (canskill(sd)) if ((pc_checkskill(sd, MG_COLDBOLT) > 0) && (pc_checkskill(sd, MG_COLDBOLT) >= pc_checkskill(sd, MG_FIREBOLT))
				&& (pc_checkskill(sd, MG_COLDBOLT) >= pc_checkskill(sd, MG_LIGHTNINGBOLT))) {
				if ((sd->state.autopilotmode == 2) && (Dangerdistance > 900)) {
					if (elemallowed(targetmd, skill_get_ele(MG_COLDBOLT, pc_checkskill(sd, MG_COLDBOLT)))) {
						unit_skilluse_ifable(&sd->bl, foundtargetID2, MG_COLDBOLT, pc_checkskill(sd, MG_COLDBOLT));
					}
				}
			}
			if (foundtargetID2 > -1) if (canskill(sd)) if ((pc_checkskill(sd, MG_LIGHTNINGBOLT) > 0) && (pc_checkskill(sd, MG_LIGHTNINGBOLT) >= pc_checkskill(sd, MG_COLDBOLT))
				&& (pc_checkskill(sd, MG_LIGHTNINGBOLT) >= pc_checkskill(sd, MG_FIREBOLT))) {
				if ((sd->state.autopilotmode == 2) && (Dangerdistance > 900)) {
					if (elemallowed(targetmd, skill_get_ele(MG_LIGHTNINGBOLT, pc_checkskill(sd, MG_LIGHTNINGBOLT)))) {
						unit_skilluse_ifable(&sd->bl, foundtargetID2, MG_LIGHTNINGBOLT, pc_checkskill(sd, MG_LIGHTNINGBOLT));
					}
				}
			}
			// ninja bolts, use highest level
			if (foundtargetID2 > -1) if (pc_rightside_atk(sd)*1.5 < sd->battle_status.matk_min) { // Note : if skill is unmodded, a higher multiplier is needed
				if (canskill(sd)) if ((pc_checkskill(sd, NJ_HUUJIN) > 0) && (pc_checkskill(sd, NJ_HUUJIN) >= pc_checkskill(sd, NJ_HYOUSENSOU))
					&& (pc_checkskill(sd, NJ_HUUJIN) >= pc_checkskill(sd, NJ_KOUENKA))) {
					if ((sd->state.autopilotmode == 2) && (Dangerdistance > 900)) {
						if (elemallowed(targetmd, skill_get_ele(NJ_HUUJIN, pc_checkskill(sd, NJ_HUUJIN)))) {
							unit_skilluse_ifable(&sd->bl, foundtargetID2, NJ_HUUJIN, pc_checkskill(sd, NJ_HUUJIN));
						}
					}
				}
				if (foundtargetID2 > -1) if (canskill(sd)) if ((pc_checkskill(sd, NJ_HYOUSENSOU) > 0) && (pc_checkskill(sd, NJ_HYOUSENSOU) >= pc_checkskill(sd, NJ_HUUJIN))
					&& (pc_checkskill(sd, NJ_HYOUSENSOU) >= pc_checkskill(sd, NJ_KOUENKA))) {
					if ((sd->state.autopilotmode == 2) && (Dangerdistance > 900)) {
						if (elemallowed(targetmd, skill_get_ele(NJ_HYOUSENSOU, pc_checkskill(sd, NJ_HYOUSENSOU)))) {
							unit_skilluse_ifable(&sd->bl, foundtargetID2, NJ_HYOUSENSOU, pc_checkskill(sd, NJ_HYOUSENSOU));
						}
					}
				}
				if (foundtargetID2 > -1) if (canskill(sd)) if ((pc_checkskill(sd, NJ_KOUENKA) > 0) && (pc_checkskill(sd, NJ_KOUENKA) >= pc_checkskill(sd, NJ_HYOUSENSOU))
					&& (pc_checkskill(sd, NJ_KOUENKA) >= pc_checkskill(sd, NJ_HUUJIN))) {
					if ((sd->state.autopilotmode == 2) && (Dangerdistance > 900)) {
						if (elemallowed(targetmd, skill_get_ele(NJ_KOUENKA, pc_checkskill(sd, NJ_KOUENKA)))) {
							unit_skilluse_ifable(&sd->bl, foundtargetID2, NJ_KOUENKA, pc_checkskill(sd, NJ_KOUENKA));
						}
					}
				}
			}
			// Soul Strike
			if (foundtargetID2 > -1) if (canskill(sd)) if ((pc_checkskill(sd, MG_SOULSTRIKE) > 0)) {
				if ((sd->state.autopilotmode == 2)) {
					if (elemallowed(targetmd, skill_get_ele(MG_SOULSTRIKE, pc_checkskill(sd, MG_SOULSTRIKE)))) {
						unit_skilluse_ifable(&sd->bl, foundtargetID2, MG_SOULSTRIKE, pc_checkskill(sd, MG_SOULSTRIKE));
					}
				}
			}
			// Arrow Vulcan
			if (foundtargetRA > -1) if (canskill(sd) && ((sd->status.weapon == W_WHIP) || (sd->status.weapon == W_MUSICAL))) if ((pc_checkskill(sd, CG_ARROWVULCAN) > 0)) {
				if (rangeddist <= 9) if ((sd->state.autopilotmode == 2) && (Dangerdistance > 900)) {
					if (elemallowed(targetRAmd, skill_get_ele(CG_ARROWVULCAN, pc_checkskill(sd, CG_ARROWVULCAN)))) {
						arrowchange(sd, targetmd);
						unit_skilluse_ifable(&sd->bl, foundtargetRA, CG_ARROWVULCAN, pc_checkskill(sd, CG_ARROWVULCAN));
					}
				}
			}
			// Musical strike
			if (foundtargetRA > -1) if (canskill(sd) && ((sd->status.weapon == W_WHIP) || (sd->status.weapon == W_MUSICAL))) if ((pc_checkskill(sd, BA_MUSICALSTRIKE) > 0)) {
				if (rangeddist <= 9) if ((sd->state.autopilotmode == 2) && (Dangerdistance > 900)) {
					if (elemallowed(targetRAmd, skill_get_ele(BA_MUSICALSTRIKE, pc_checkskill(sd, BA_MUSICALSTRIKE)))) {
						arrowchange(sd, targetmd);
						unit_skilluse_ifable(&sd->bl, foundtargetRA, BA_MUSICALSTRIKE, pc_checkskill(sd, BA_MUSICALSTRIKE));
					}
				}
			}
			// Throw Arrow
			if (foundtargetRA > -1) if (canskill(sd) && ((sd->status.weapon == W_WHIP) || (sd->status.weapon == W_MUSICAL))) if ((pc_checkskill(sd, DC_THROWARROW) > 0)) {
				if (rangeddist <= 9) if ((sd->state.autopilotmode == 2) && (Dangerdistance > 900)) {
					if (elemallowed(targetRAmd, skill_get_ele(DC_THROWARROW, pc_checkskill(sd, DC_THROWARROW)))) {
						arrowchange(sd, targetmd);
						unit_skilluse_ifable(&sd->bl, foundtargetRA, DC_THROWARROW, pc_checkskill(sd, DC_THROWARROW));
					}
				}
			}

			// Holy Light
			if (foundtargetID2 > -1) if (canskill(sd)) if ((pc_checkskill(sd, AL_HOLYLIGHT) > 0)) {
				if ((sd->state.autopilotmode == 2) && (Dangerdistance > 900)) {
					if (elemallowed(targetmd, skill_get_ele(AL_HOLYLIGHT, pc_checkskill(sd, AL_HOLYLIGHT)))) {
						unit_skilluse_ifable(&sd->bl, foundtargetID2, AL_HOLYLIGHT, pc_checkskill(sd, AL_HOLYLIGHT));
					}
				}
			}

			// If we still failed to pick a skill, the enemy is probably dark or holy and resists all elements so we have to compromise and use whatever.
			if (foundtargetID2 > -1) if (canskill(sd)) if ((targetmd->status.def_ele == ELE_DARK) || ((targetmd->status.def_ele == ELE_HOLY)))
			{ // Gravity Field
				// High Wizards can pull this ace out of their sleeve if they have gems and aren't under attack
				if (canskill(sd)) if ((pc_checkskill(sd, HW_GRAVITATION) > 0) && (Dangerdistance > 900) && (pc_search_inventory(sd, ITEMID_BLUE_GEMSTONE)>=0)) {
					unit_skilluse_ifablexy(&sd->bl, foundtargetID2, HW_GRAVITATION, pc_checkskill(sd, HW_GRAVITATION));
				}
				// Storm Gust - this can at least freeze things and keep them under control, as well as change them to water element
				// However use at lowest level for maximal freezing efficiency and faster cast time - damage isn't going to be good anyway
				if (!((targetmd->status.def_ele == ELE_HOLY) || (targetmd->status.def_ele < 4)))
				if (canskill(sd)) if ((pc_checkskill(sd, WZ_STORMGUST) > 0) && ((Dangerdistance > 900) && (!duplicateskill(p, WZ_STORMGUST)))) {
					if ((!(targetmd->status.def_ele == ELE_UNDEAD)) && (!((status_get_class_(targetbl) == CLASS_BOSS))))
					unit_skilluse_ifablexy(&sd->bl, foundtargetID2, WZ_STORMGUST, 1);
					else // Immune to freezing, use highest level!
						unit_skilluse_ifablexy(&sd->bl, foundtargetID2, WZ_STORMGUST, pc_checkskill(sd, WZ_STORMGUST));
				}
				//  Use JT if SG isn't an option, best single target damage
				if (!((targetmd->status.def_ele == ELE_HOLY) || (targetmd->status.def_ele < 4)))
					if (canskill(sd)) if ((pc_checkskill(sd, WZ_JUPITEL) > 0) && ((Dangerdistance > 900) || (sd->special_state.no_castcancel))) {
					unit_skilluse_ifable(&sd->bl, foundtargetID2, WZ_JUPITEL, pc_checkskill(sd, WZ_JUPITEL));
				}
				// If we are under attack, we have to cast something faster...
if (canskill(sd)) if ((pc_checkskill(sd, HW_MAGICCRASHER) > 0) && (elemallowed(targetmd, sd->battle_status.rhw.ele))) {
	unit_skilluse_ifable(&sd->bl, foundtargetID2, HW_MAGICCRASHER, pc_checkskill(sd, HW_MAGICCRASHER));
}
// Not everyone is a high wizard
if (!((targetmd->status.def_ele == ELE_HOLY) || (targetmd->status.def_ele < 4)))
	if (canskill(sd)) if ((pc_checkskill(sd, MG_SOULSTRIKE) > 0)) {
		unit_skilluse_ifable(&sd->bl, foundtargetID2, MG_SOULSTRIKE, pc_checkskill(sd, MG_SOULSTRIKE));
	}
			};

			// Throw Shuriken
			if (foundtargetRA > -1) if (canskill(sd)) if ((pc_checkskill(sd, NJ_SYURIKEN) > 0)) if (rangeddist <= 9) {
				shurikenchange(sd, targetRAmd);
				unit_skilluse_ifable(&sd->bl, foundtargetRA, NJ_SYURIKEN, pc_checkskill(sd, NJ_SYURIKEN));
			}

			// Do normal attack if ranged
			if (foundtargetRA > -1) if ((sd->battle_status.rhw.range >= 5) && (sd->state.autopilotmode > 1) && (sd->battle_status.rhw.range >= rangeddist))
				// If leader is running away, stop and follow them instead!
				if (leaderdistance < 12) {
					if (sd->status.weapon == W_BOW) { arrowchange(sd, targetRAmd); }
					ammochange2(sd, targetRAmd);
					aspdpotion(sd);
					clif_parse_ActionRequest_sub(sd, 7, foundtargetRA, gettick());
				}

			// Tanking mode is set
			if (sd->state.autopilotmode == 1) {

				/////////////////////////////////////////////////////////////////////
				// Tanking mode skills that actually help to tank
				/////////////////////////////////////////////////////////////////////
				// Ninja Aura to enable Mirror Image
				if (pc_checkskill(sd, NJ_NEN) > 0)
					if (pc_checkskill(sd, NJ_BUNSINJYUTSU) > 0)
						if ((sd->special_state.no_castcancel) || (Dangerdistance > 900)) {
							if (!(sd->sc.data[SC_NEN])) {
								unit_skilluse_ifable(&sd->bl, SELF, NJ_NEN, pc_checkskill(sd, NJ_NEN));
							}
						}

				// Mirror Image
				// Use this for tanking instead of cicada because no backwards movement.
				// Interruptable though so need Phen or equipvalent while actually tanking a monster.
				if (pc_checkskill(sd, NJ_BUNSINJYUTSU) > 0)
					if (pc_search_inventory(sd, 7524) >= 0) // requires Shadow Orb
						if ((sd->special_state.no_castcancel) || (Dangerdistance > 900)) {
							if (!(sd->sc.data[SC_BUNSINJYUTSU])) {
								unit_skilluse_ifable(&sd->bl, SELF, NJ_BUNSINJYUTSU, pc_checkskill(sd, NJ_BUNSINJYUTSU));
							}
						}
				/////////////////////////////////////////////////////////////////////
				// Skills that can be used while tanking only, for supporting others
				/////////////////////////////////////////////////////////////////////
				// Provoke
				if (pc_checkskill(sd, SM_PROVOKE) > 0) {
					resettargets();
					map_foreachinrange(provokethis, &sd->bl, 9, BL_MOB, sd);
					if (foundtargetID > -1) {
						unit_skilluse_ifable(&sd->bl, foundtargetID, SM_PROVOKE, pc_checkskill(sd, SM_PROVOKE));
					}
				}
				// Throw Stone
				if (pc_checkskill(sd, TF_THROWSTONE) > 0) {
					resettargets();
					map_foreachinrange(provokethis, &sd->bl, 9, BL_MOB, sd);
					if (foundtargetID > -1) {
						unit_skilluse_ifable(&sd->bl, foundtargetID, TF_THROWSTONE, pc_checkskill(sd, TF_THROWSTONE));
					}
				}

				// Reject Sword
				if (pc_checkskill(sd, ST_REJECTSWORD) > 0) {
					if (!(sd->sc.data[SC_REJECTSWORD])) {
						unit_skilluse_ifable(&sd->bl, SELF, ST_REJECTSWORD, pc_checkskill(sd, ST_REJECTSWORD));
					}
				}


				// Find nearest enemy
				resettargets();
				// No leader then closest to ourselves we can see
				//if (leaderID == -1) {
				if ((!p) || (leaderID == sd->bl.id)) {
					map_foreachinrange(targetnearestwalkto, &sd->bl, MAX_WALKPATH, BL_MOB, sd);
				}
				// but if leader exists, then still closest to us but in leader's range
				// If leader does not exist, we are not leader, and we are in party, then leader is on another map. Do not attack things, follow them.
				else if (leaderID > -1) {
					map_foreachinrange(targetnearestwalkto, leaderbl, AUTOPILOT_RANGE_CAP, BL_MOB, sd);
					// have to walk too many tiles means the target is probably behind some wall. Don't try to engage it, even if maxpath allows.
					// should be obsolete, now targeting checks for walking distance
					if (targetdistance > 29) { foundtargetID = -1; }
				}

				// attack nearest thing in range if available
				if (foundtargetID > -1) {
					//if ((foundtargetID > -1) && ((leaderdistance<=14) || (leaderID==-1))) {

						/////////////////////////////////////////////////////////////////////
						// Skills that can be used while tanking only, on tanked enemy 
						/////////////////////////////////////////////////////////////////////

						//
						// Star Gladiator Heat
						//
					bool isbosstarget = (status_get_class_(targetbl) == CLASS_BOSS);
					if (canskill(sd)) if ((pc_checkskill(sd, SG_SUN_WARM) > 0)) {
						i = SG_SUN_WARM - SG_SUN_WARM;
						// Need Sun Map or Miracle
						if ((sd->bl.m == sd->feel_map[i].m) || (sd->sc.data[SC_MIRACLE])) {
							if ((!(sd->sc.data[SC_WARM]) && isbosstarget))
								unit_skilluse_ifable(&sd->bl, SELF, SG_SUN_WARM, pc_checkskill(sd, SG_SUN_WARM));
					}
					}
					if (canskill(sd)) if ((pc_checkskill(sd, SG_MOON_WARM) > 0)) {
						i = SG_MOON_WARM - SG_SUN_WARM;
						// Need Sun Map or Miracle
						if ((sd->bl.m == sd->feel_map[i].m) || (sd->sc.data[SC_MIRACLE])) {
							if ((!(sd->sc.data[SC_WARM]) && isbosstarget))
								unit_skilluse_ifable(&sd->bl, SELF, SG_MOON_WARM, pc_checkskill(sd, SG_MOON_WARM));
						}
					}
					if (canskill(sd)) if ((pc_checkskill(sd, SG_STAR_WARM) > 0)) {
						i = SG_STAR_WARM - SG_SUN_WARM;
						// Need Sun Map or Miracle
						if ((sd->bl.m == sd->feel_map[i].m) || (sd->sc.data[SC_MIRACLE])) {
							if ((!(sd->sc.data[SC_WARM]) && isbosstarget))
								unit_skilluse_ifable(&sd->bl, SELF, SG_STAR_WARM, pc_checkskill(sd, SG_STAR_WARM));
						}
					}
					if (canskill(sd)) if ((pc_checkskill(sd, SG_FUSION) > 0)) if (havepriest) {
						if (!(sd->sc.data[SC_FUSION])) {
							unit_skilluse_ifable(&sd->bl, SELF, SG_FUSION, pc_checkskill(sd, SG_FUSION));
						}
					}
						// Sacrifice
			// At least 90% HP and must be allowed to use neutral element on target
			// Also don't do it if it won't actually kill at least one enemy
			if (canskill(sd)) if ((pc_checkskill(sd, PA_SACRIFICE) > 0)) {
				if ((sd->battle_status.hp>0.9*sd->battle_status.hp) && (sd->battle_status.hp*.45*(0.9 + 0.1*pc_checkskill(sd, PA_SACRIFICE)) >= targetmd->status.hp))
					// At least 4 enemies in range (or 3 if weak to element)
					if (elemallowed(targetmd,ELE_NEUTRAL))
						unit_skilluse_ifable(&sd->bl, SELF, PA_SACRIFICE, pc_checkskill(sd, PA_SACRIFICE));
			}
			// Berserk
			if (pc_checkskill(sd, LK_BERSERK) > 0) if (sd->state.specialtanking) {
				if (!(sd->sc.data[SC_BERSERK])) {
					unit_skilluse_ifable(&sd->bl, SELF, LK_BERSERK, pc_checkskill(sd, LK_BERSERK));
				}
			}

			// Hammerfall
			// If 4 or more things are attacking us and the nearest is in range and can be stunned
			// Don't bother at low skill levels, at least require 50% chance to stun
			if (canskill(sd)) if (pc_checkskill(sd, BS_HAMMERFALL) >= 3) {
				if ((sd->status.weapon == W_MACE) || (sd->status.weapon == W_1HAXE) || (sd->status.weapon == W_2HAXE))
					if (Dangerdistance <= 2) {
					if (dangercount>=4) {
						if (!isdisabled(dangermd))
							if (!(dangermd->status.def_ele == ELE_UNDEAD)) {
								if (!((status_get_class_(dangerbl) == CLASS_BOSS)))
									unit_skilluse_ifablexy(&sd->bl, founddangerID, BS_HAMMERFALL, pc_checkskill(sd, BS_HAMMERFALL));
							}
					}
				}
			}


			// Grand Cross
			// Must have at least 54% HP remaining to risk using this
			if (canskill(sd)) if ((pc_checkskill(sd, CR_GRANDCROSS) > 0)) {
				if (sd->battle_status.hp>0.54*sd->battle_status.hp)
					// Are we in the build to use this?
					if (sd->battle_status.int_ + sd->battle_status.str>=1.2*sd->status.base_level)
				// At least 4 enemies in range (or 3 if weak to element)
				if (map_foreachinrange(AOEPriority, bl, 2, BL_MOB, skill_get_ele(CR_GRANDCROSS, pc_checkskill(sd, CR_GRANDCROSS))) >= 8)
					unit_skilluse_ifable(&sd->bl, SELF, CR_GRANDCROSS, pc_checkskill(sd, CR_GRANDCROSS));
			}
			// Magnum Break
			if (canskill(sd)) if ((pc_checkskill(sd, SM_MAGNUM) > 0)) {
					// At least 3 enemies in range (or 2 if weak to element)
					if (map_foreachinrange(AOEPriority, bl, 2, BL_MOB, skill_get_ele(SM_MAGNUM, pc_checkskill(sd, SM_MAGNUM))) >= 6)
						unit_skilluse_ifable(&sd->bl, SELF, SM_MAGNUM, pc_checkskill(sd, SM_MAGNUM));
			}

			// Steal skill
			// Use in any mode above 50% SP only, and never use below 1/3 health, that's not the time for messing around.
			if (canskill(sd)) if (pc_checkskill(sd, TF_STEAL)>0)
				// Do not steal if can do it automaticaly for free
				if ((pc_checkskill(sd, RG_SNATCHER) <= 0) || (sd->battle_status.rhw.range>3))
			{
				if ((status_get_sp(bl) >= status_get_max_sp(bl) / 2) && (status_get_hp(bl) >  status_get_max_hp(bl) / 3)) {
					if (!(((targetmd->state.steal_flag == UCHAR_MAX) || (targetmd->sc.opt1 && targetmd->sc.opt1 != OPT1_BURNING))))
					{
						unit_skilluse_ifable(&sd->bl, foundtargetID, TF_STEAL, pc_checkskill(sd, TF_STEAL));
					}
				}
			}
			// MUG skill
			if (canskill(sd)) if (pc_checkskill(sd, RG_STEALCOIN) > 0) {
				if ((status_get_sp(bl) >= status_get_max_sp(bl) / 2) && (status_get_hp(bl) > status_get_max_hp(bl) / 3)) {
					if (!(((targetmd->state.steal_coin_flag == UCHAR_MAX) || (targetmd->sc.data[SC_STONE]) || targetmd->sc.data[SC_FREEZE])))
					{
						unit_skilluse_ifable(&sd->bl, foundtargetID, RG_STEALCOIN, pc_checkskill(sd, RG_STEALCOIN));
					}
				}
			}

			// Sonic Blow skill
			// Note the AI ignores the +50% damage dealt on low health targets. It can't judge when it's worth waiting for other players to deal damage first and save SP.
			// If you really want to take advantage of that on bosses, the best bet is to simply refill the SinX's SP after the boss drops below 50%.
			if (canskill(sd)) if (pc_checkskill(sd, AS_SONICBLOW)>0) if (sd->status.weapon == W_KATAR) {
				// Use like other skills, but also always use if EDP enabled, that's not the time to conserve SP
				if ((targetmd->status.hp > (12 - (sd->battle_status.sp * 10 / sd->battle_status.max_sp)) * pc_rightside_atk(sd))
					|| (status_get_hp(bl) < status_get_max_hp(bl) / 3) || (sd->sc.data[SC_EDP])) {
					unit_skilluse_ifable(&sd->bl, foundtargetID, AS_SONICBLOW, pc_checkskill(sd, AS_SONICBLOW));
				}
			}

			// Envenom skill
			if (canskill(sd)) if (pc_checkskill(sd, TF_POISON)>0) {
				// Not if already poisoned
				if (!(targetmd->sc.data[SC_POISON]))
				{ // Always use if critically wounded otherwise use on mobs that will take longer to kill only if sp is lower
					if ((targetmd->status.hp > (12 - (sd->battle_status.sp * 10 / sd->battle_status.max_sp)) * pc_rightside_atk(sd))
						|| (status_get_hp(bl) < status_get_max_hp(bl) / 3)){
						if (!((status_get_class_(targetbl) == CLASS_BOSS))) {
							if (!(targetmd->status.def_ele==ELE_UNDEAD)) {
								unit_skilluse_ifable(&sd->bl, foundtargetID, TF_POISON, pc_checkskill(sd, TF_POISON));
							}
						}
					}
				}
			}

			// Chain Combo skill
			if (canskill(sd)) if (pc_checkskill(sd, MO_CHAINCOMBO) > 0) {
				if (sd->sc.data[SC_COMBO] && (sd->sc.data[SC_COMBO]->val1 == MO_TRIPLEATTACK)) {
					unit_skilluse_ifable(&sd->bl, SELF, MO_CHAINCOMBO, pc_checkskill(sd, MO_CHAINCOMBO));
				}
			}
			// Combo Finish
			if (canskill(sd)) if (pc_checkskill(sd, MO_COMBOFINISH) > 0) if (sd->spiritball>0) {
				if (sd->sc.data[SC_COMBO] && (sd->sc.data[SC_COMBO]->val1 == MO_CHAINCOMBO)) {
					unit_skilluse_ifable(&sd->bl, SELF, MO_COMBOFINISH, pc_checkskill(sd, MO_COMBOFINISH));
				}
			}
			// Tiger Fist
			if (canskill(sd)) if (pc_checkskill(sd, CH_TIGERFIST) > 0) if (sd->spiritball>0) {
				if (sd->sc.data[SC_COMBO] && (sd->sc.data[SC_COMBO]->val1 == MO_COMBOFINISH)) {
					unit_skilluse_ifable(&sd->bl, SELF, CH_TIGERFIST, pc_checkskill(sd, CH_TIGERFIST));
				}
			}
			// Chain Crush
			if (canskill(sd)) if (pc_checkskill(sd, CH_CHAINCRUSH) > 0) if (sd->spiritball>1) {
				if (sd->sc.data[SC_COMBO] && ((sd->sc.data[SC_COMBO]->val1 == MO_COMBOFINISH) || (sd->sc.data[SC_COMBO]->val1 == CH_TIGERFIST))) {
					unit_skilluse_ifable(&sd->bl, SELF, CH_CHAINCRUSH, pc_checkskill(sd, CH_CHAINCRUSH));
				}
			}

			// Tornado Kick
			if (canskill(sd)) if (pc_checkskill(sd, TK_STORMKICK) > 0) {
				if (sd->sc.data[SC_COMBO] && ((sd->sc.data[SC_COMBO]->val1 == TK_STORMKICK))) {
					unit_skilluse_ifable(&sd->bl, SELF, TK_STORMKICK, pc_checkskill(sd, TK_STORMKICK));
				}
			}

			// Heel drop
			if (canskill(sd)) if (pc_checkskill(sd, TK_DOWNKICK) > 0) {
				if (sd->sc.data[SC_COMBO] && ((sd->sc.data[SC_COMBO]->val1 == TK_DOWNKICK))) {
					unit_skilluse_ifable(&sd->bl, SELF, TK_DOWNKICK, pc_checkskill(sd, TK_DOWNKICK));
				}
			}

			// Roundhouse Kick
			if (canskill(sd)) if (pc_checkskill(sd, TK_TURNKICK) > 0) {
				if (sd->sc.data[SC_COMBO] && ((sd->sc.data[SC_COMBO]->val1 == TK_TURNKICK))) {
					unit_skilluse_ifable(&sd->bl, SELF, TK_TURNKICK, pc_checkskill(sd, TK_TURNKICK));
				}
			}

			// Roundhouse Kick
			if (canskill(sd)) if (pc_checkskill(sd, TK_COUNTER) > 0) {
				if (sd->sc.data[SC_COMBO] && ((sd->sc.data[SC_COMBO]->val1 == TK_COUNTER))) {
					unit_skilluse_ifable(&sd->bl, SELF, TK_COUNTER, pc_checkskill(sd, TK_COUNTER));
				}
			}

			// Investigate skill
			// Avoid if combo skill requiring sphere is available, combo is better.
			if (canskill(sd)) if (pc_checkskill(sd, MO_INVESTIGATE)>0) if ((sd->spiritball>0) && (pc_checkskill(sd, MO_COMBOFINISH) < 3)) {
				// Always use if critically wounded otherwise use on mobs that will take longer to kill only if sp is lower
				if ((targetmd->status.hp > (12 - (sd->battle_status.sp * 10 / sd->battle_status.max_sp)) * pc_rightside_atk(sd))
					|| (status_get_hp(bl) < status_get_max_hp(bl) / 3)){
					unit_skilluse_ifable(&sd->bl, foundtargetID, MO_INVESTIGATE, pc_checkskill(sd, MO_INVESTIGATE));
				}
			}

			// Charge Attack skill
			// **Note** I modded this to not knock back the target, but should likely have same AI use without that change.
			if (canskill(sd)) if (pc_checkskill(sd, KN_CHARGEATK)>0) {
				if (targetdistance>=8){
					unit_skilluse_ifable(&sd->bl, foundtargetID, KN_CHARGEATK, pc_checkskill(sd, KN_CHARGEATK));
				}
			}


			// Cart Revolution skill
			if (canskill(sd)) if (pc_checkskill(sd, MC_CARTREVOLUTION)>0) if (pc_iscarton(sd)) {
				// Always use if critically wounded or mobbed otherwise use on mobs that will take longer to kill only if sp is lower
				if ((targetmd->status.hp > (12 - (sd->battle_status.sp * 10 / sd->battle_status.max_sp)) * pc_rightside_atk(sd))
					|| (status_get_hp(bl) < status_get_max_hp(bl) / 3) || (dangercount>3)) {
					unit_skilluse_ifable(&sd->bl, foundtargetID, MC_CARTREVOLUTION, pc_checkskill(sd, MC_CARTREVOLUTION));
				}
			}
			// Cart Termination skill
			if (canskill(sd)) if (pc_checkskill(sd, WS_CARTTERMINATION)>0) if (sd->state.specialtanking)
				if (sd->sc.data[SC_CARTBOOST]) {
					// Always use if critically wounded otherwise use on mobs that will take longer to kill only if sp is lower
					if ((targetmd->status.hp > (12 - (sd->battle_status.sp * 10 / sd->battle_status.max_sp)) * pc_rightside_atk(sd))
						|| (status_get_hp(bl) < status_get_max_hp(bl) / 3)){
						unit_skilluse_ifable(&sd->bl, foundtargetID, WS_CARTTERMINATION, pc_checkskill(sd, WS_CARTTERMINATION));
					}
				}
			// Mammonite skill
			if (canskill(sd)) if (pc_checkskill(sd, MC_MAMMONITE)>0) if (sd->state.specialtanking) {
				// Always use if critically wounded otherwise use on mobs that will take longer to kill only if sp is lower
				if ((targetmd->status.hp > (12 - (sd->battle_status.sp * 10 / sd->battle_status.max_sp)) * pc_rightside_atk(sd))
					|| (status_get_hp(bl) < status_get_max_hp(bl) / 3)){
					unit_skilluse_ifable(&sd->bl, foundtargetID, MC_MAMMONITE, pc_checkskill(sd, MC_MAMMONITE));
				}
			}

			// Holy Cross skill
			if (canskill(sd)) if (pc_checkskill(sd, CR_HOLYCROSS)>0) {
				// Use like bash but ONLY if enemy is weak to holy, otherwise damage isn't that much better and Stun is superior to Blind
				if (elemstrong(targetmd, skill_get_ele(CR_HOLYCROSS, pc_checkskill(sd, CR_HOLYCROSS))))
					if ((targetmd->status.hp > (12 - (sd->battle_status.sp * 10 / sd->battle_status.max_sp)) * pc_rightside_atk(sd))
					|| (status_get_hp(bl) < status_get_max_hp(bl) / 3)){
					unit_skilluse_ifable(&sd->bl, foundtargetID, CR_HOLYCROSS, pc_checkskill(sd, CR_HOLYCROSS));
				}
			}

			// Pierce skill
			if (canskill(sd)) if (pc_checkskill(sd, KN_PIERCE)>0) if ((dangercount<3) || ((pc_checkskill(sd, KN_BOWLINGBASH) == 0) && (pc_checkskill(sd, KN_BRANDISHSPEAR))))
				if ((sd->status.weapon == W_1HSPEAR) || (sd->status.weapon == W_2HSPEAR))
				// Use on LARGE enemies only, otherwise bash/bowling bash is more cost effective.
					if (targetmd->status.size==SZ_BIG) {
				// Always use if critically wounded otherwise use on mobs that will take longer to kill only if sp is lower
				if ((targetmd->status.hp > (12 - (sd->battle_status.sp * 10 / sd->battle_status.max_sp)) * pc_rightside_atk(sd))
					|| (status_get_hp(bl) < status_get_max_hp(bl) / 3)){
					unit_skilluse_ifable(&sd->bl, foundtargetID, KN_PIERCE, pc_checkskill(sd, KN_PIERCE));
				}
			}
			
			// Brandish Spear skill
			// prefer to bowling bash if spear and peco equipped
			// **Note** This assumes the skill actually does better damage. I believe that should also be
			// the case on default settings after the 2nd job skill update
			// however, I haven't changed this to count as a ranged type.
			// So if you use official settings and have that update (at the time of writing this, it's PR 4072, not yet merged)
			// then you need to add a check here for ranged attacks to be valid (no pneuma on target)
			if (canskill(sd)) if (pc_checkskill(sd, KN_BRANDISHSPEAR) > 0) if (pc_isriding(sd))
				if ((sd->status.weapon == W_2HSPEAR) || (sd->status.weapon == W_1HSPEAR)) {
				// Always use if critically wounded or mobbed otherwise use on mobs that will take longer to kill only if sp is lower
				if ((targetmd->status.hp > (12 - (sd->battle_status.sp * 10 / sd->battle_status.max_sp)) * pc_rightside_atk(sd))
					|| (status_get_hp(bl) < status_get_max_hp(bl) / 3) || (dangercount >= 3)) {
					unit_skilluse_ifable(&sd->bl, foundtargetID, KN_BRANDISHSPEAR, pc_checkskill(sd, KN_BRANDISHSPEAR));
				}
			}
			// Bowling Bash skill
			if (canskill(sd)) if (pc_checkskill(sd, KN_BOWLINGBASH)>0) {
				// Always use if critically wounded or mobbed otherwise use on mobs that will take longer to kill only if sp is lower
				if ((targetmd->status.hp > (12 - (sd->battle_status.sp * 10 / sd->battle_status.max_sp)) * pc_rightside_atk(sd))
					|| (status_get_hp(bl) < status_get_max_hp(bl) / 3) || (dangercount>=3)) {
					unit_skilluse_ifable(&sd->bl, foundtargetID, KN_BOWLINGBASH, pc_checkskill(sd, KN_BOWLINGBASH));
				}
			}

			// Backstab skill
			if (canskill(sd)) if (pc_checkskill(sd, RG_BACKSTAP) > 0) {
				// Assumes the update to allow using this from the front is already included
				if ((targetmd->status.hp > (12 - (sd->battle_status.sp * 10 / sd->battle_status.max_sp)) * pc_rightside_atk(sd))
					|| (status_get_hp(bl) < status_get_max_hp(bl) / 3)) {
					unit_skilluse_ifable(&sd->bl, foundtargetID, RG_BACKSTAP, pc_checkskill(sd, RG_BACKSTAP));
				}
			}


			// Bash skill
			if (canskill(sd)) if (pc_checkskill(sd, SM_BASH)>0) if (pc_checkskill(sd, KN_BOWLINGBASH)<pc_checkskill(sd, SM_BASH)) {
			// Do not use if Bowling Bash is known at equal or higher level, as it's strictly better
			// Always use if critically wounded otherwise use on mobs that will take longer to kill only if sp is lower
					if ((targetmd->status.hp > (12 - (sd->battle_status.sp * 10 / sd->battle_status.max_sp)) * pc_rightside_atk(sd))
						|| (status_get_hp(bl) < status_get_max_hp(bl) / 3)){
								unit_skilluse_ifable(&sd->bl, foundtargetID, SM_BASH, pc_checkskill(sd, SM_BASH));
					}
			}

			// Correct code
			if ((sd->battle_status.rhw.range >= targetdistance) && (targetdistance<3)) {
				aspdpotion(sd);
				unit_attack(&sd->bl, foundtargetID, 1);
			} else
			{	struct walkpath_data wpd1;
				if (path_search(&wpd1, sd->bl.m, bl->x, bl->y, targetbl->x, targetbl->y, 0, CELL_CHKNOPASS, MAX_WALKPATH))
					newwalk(&sd->bl, bl->x + dirx[wpd1.path[0]], bl->y + diry[wpd1.path[0]], 8);
				return 0;
			}
			
		}
		else {
			///////////////////////////////////////////////////////////
			// Skills to use while not in battle only
			///////////////////////////////////////////////////////////
			// Skills used when idle, like pick stone or aqua benedicta
			if (canskill(sd)) skillwhenidle(sd);

			// If leader is sitting, also sit down
			if (leaderID>-1) if (pc_issit(leadersd) && (leaderdistance<=14)) {
				if (!pc_issit(sd)) {
					sitdown(sd);
					return 0;
				}
			} 

			// If we can't move this ends here
			if (sd->state.block_action & PCBLOCK_MOVE) return 0;

			// If there is a leader and we haven't found a target in their area, stay near them.
			// Note : maxwalkpath needs to be very high otherwise we fail to follow!
			if ((leaderID > -1) && (leaderID != sd->bl.id)) {
				// We are in the tanking branch, this isn't possible
				/*				if (sd->state.autopilotmode != 1) {
					if (leaderdistance >= 2) {
						newwalk(&sd->bl, leaderbl->x + rand() % 3 - 1, leaderbl->y + rand() % 3 - 1, 8);
					}
				} // If tanking mode, try to get slightly ahead of leader
				else {*/
					int tankdestinationx = leaderbl->x + 2* dirx[leadersd->ud.dir];
					int tankdestinationy = leaderbl->y + 2* diry[leadersd->ud.dir];
					if ((abs(tankdestinationx - sd->bl.x) >= 2) || (abs(tankdestinationy - sd->bl.y) >= 2)) {
						newwalk(&sd->bl, tankdestinationx + rand() % 3 - 1, tankdestinationy + rand() % 3 - 1, 8);
					//}
				}
			}
				else if ((p) && (leaderID != sd->bl.id)) {
					resettargets();
					// leader wasn't on map, target nearest NPC. Hopefully it's the warp the leader entered.
					// However don't if there was no party, means we are soloing!
					map_foreachinmap(targetnearestwarp, sd->bl.m, BL_NPC, sd);
					if (foundtargetID > -1) {
						newwalk(&sd->bl, targetbl->x, targetbl->y, 8);
					}
				}
	// }
			if ((leaderID==sd->bl.id) || (!p)) {
				// seek next enemy outside range if nothing else to do and we are the leader or party doesn't exist (solo)
				// Note : client.conf maxwalkpath needs to be very high otherwise we fail to move if enemy is too far!
				// for same reason, disabling official walkpath and raising MAX_WALK_PATH in source is necessary
				// However, excessively large max walkpath might cause lagging so don't expect this to seek out enemies on the other side of the map.
				// The feature isn't meant for botting, it's meant for controlling secondary characters. So it's ok if the leader gets stuck if no enemies left nearby.
				resettargets();
				map_foreachinrange(targetnearestwalkto, &sd->bl, MAX_WALKPATH, BL_MOB, sd);
				//			ShowError("No target found, moving?");
				if (foundtargetID > -1) {
					//				ShowError("No target found, moving!");
										newwalk(&sd->bl, targetbl->x, targetbl->y, 8);
						//bl(&sd->bl, targetbl, 2, 2);
				}
			}
		}
	}
	// Not tanking mode so follow the party leader
	// Can't move while already using a skill
	else { 
		// Skills to use when doing nothing important
		if (canskill(sd)) skillwhenidle(sd);

		// If we can't move this ends here
		if (sd->state.block_action & PCBLOCK_MOVE) return 0;

		// Follow the leader
		if (leaderID > -1) { 
			
			followleader:
			Dangerdistance = inDangerLeader(leadersd);

			// If party leader not under attack, get in range of 2
			if (Dangerdistance >= 900) {
				if ((abs(sd->bl.x - leaderbl->x) > 2) || abs(sd->bl.y - leaderbl->y) > 2) {
					if (!leadersd->ud.walktimer)
					newwalk(&sd->bl, leaderbl->x + rand() % 5 - 2, leaderbl->y + rand() % 5 - 2, 8);
					else newwalk(&sd->bl, leaderbl->x, leaderbl->y, 8); // ignore the random area if leader is still moving!
					// This is necessary because going diagonally is slower so if the random is included the AI will follow slower and gets left behind!
					return 0;
				}
			}
				// but if they are under attack, as we are not in tanking mode, maintain a distance of 6 by taking only 1 step at a time closer
				else
				{
					// If either leader or nearest monster attacking them is not directly shootable, go closer
					// This is necessary to avoid the party stuck behind a corner, unable to attack 
					if ((abs(sd->bl.x - leaderbl->x) > 6) || (abs(sd->bl.y - leaderbl->y) > 6) 
						|| !(path_search_long(NULL, leadersd->bl.m, bl->x, bl->y, leaderbl->x, leaderbl->y, CELL_CHKNOPASS,7))
						|| !(path_search_long(NULL, leadersd->bl.m, bl->x, bl->y, dangerbl->x, dangerbl->y, CELL_CHKNOPASS,7))
						) {

						struct walkpath_data wpd1; 
						if (path_search(&wpd1, leadersd->bl.m, bl->x, bl->y, leaderbl->x, leaderbl->y, 0, CELL_CHKNOPASS))
							newwalk(&sd->bl, bl->x + dirx[wpd1.path[0]], bl->y + diry[wpd1.path[0]], 8);
						return 0;
					}

				}
			Dangerdistance = inDanger(sd);

			if pc_issit(leadersd) {
					if (!pc_issit(sd)) {
						sitdown(sd);
						return 0;
					}
			}
		//	unit_walktobl(&sd->bl, targetbl, 2, 0); 
		}
		// Party leader left map?
		else if (p) {
			resettargets();
			// target nearest NPC. Hopefully it's the warp the leader entered.
			map_foreachinrange(targetnearestwarp, &sd->bl, MAX_WALKPATH, BL_NPC, sd);
			if (foundtargetID > -1) {
				newwalk(&sd->bl, targetbl->x, targetbl->y, 8);
			}

		}
	}

	return 0;
}

/**
 * Initialization function for unit on map start
 * called in map::do_init
 */
void do_init_unit(void){
	add_timer_func_list(unit_attack_timer,  "unit_attack_timer");
	add_timer_func_list(unit_walktoxy_timer,"unit_walktoxy_timer");
	add_timer_func_list(unit_walktobl_sub, "unit_walktobl_sub");
	add_timer_func_list(unit_delay_walktoxy_timer,"unit_delay_walktoxy_timer");
	add_timer_func_list(unit_delay_walktobl_timer,"unit_delay_walktobl_timer");
	add_timer_func_list(unit_teleport_timer,"unit_teleport_timer");
	add_timer_func_list(unit_step_timer,"unit_step_timer");
	
	add_timer_func_list(unit_autopilot_timer, "unit_autopilot_timer");
//	add_timer_func_list(unit_autopilot_homunculus_timer, "unit_autopilot_homunculus_timer");

}

/**
 * Unit module destructor, (thing to do before closing the module)
 * called in map::do_final
 * @return 0
 */
void do_final_unit(void){
	// Nothing to do
}
