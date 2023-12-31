// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2015-2023, Intel Corporation */

/*
 * pminvaders.c -- example usage of non-tx allocations
 */

#include <stddef.h>
#include <ncurses.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libpmem.h>
#include <libpmemobj.h>

#define LAYOUT_NAME "pminvaders"

#define PMINVADERS_POOL_SIZE	(100 * 1024 * 1024) /* 100 megabytes */

#define GAME_WIDTH	30
#define GAME_HEIGHT	30

#define RRAND(min, max)	(rand() % ((max) - (min) + 1) + (min))

#define STEP	50

#define PLAYER_Y (GAME_HEIGHT - 1)
#define MAX_GSTATE_TIMER 10000
#define MIN_GSTATE_TIMER 5000

#define MAX_ALIEN_TIMER	1000

#define MAX_PLAYER_TIMER 1000
#define MAX_BULLET_TIMER 500

enum colors {
	C_UNKNOWN,
	C_PLAYER,
	C_ALIEN,
	C_BULLET,

	MAX_C
};

struct game_state {
	uint32_t timer; /* alien spawn timer */
	uint16_t score;
	uint16_t high_score;
};

struct alien {
	uint16_t x;
	uint16_t y;
	uint32_t timer; /* movement timer */
};

struct player {
	uint16_t x;
	uint16_t padding; /* to 8 bytes */
	uint32_t timer; /* weapon cooldown */
};

struct bullet {
	uint16_t x;
	uint16_t y;
	uint32_t timer; /* movement timer */
};

/*
 * Layout definition
 */
POBJ_LAYOUT_BEGIN(pminvaders);
POBJ_LAYOUT_ROOT(pminvaders, struct game_state);
POBJ_LAYOUT_TOID(pminvaders, struct player);
POBJ_LAYOUT_TOID(pminvaders, struct alien);
POBJ_LAYOUT_TOID(pminvaders, struct bullet);
POBJ_LAYOUT_END(pminvaders);

static PMEMobjpool *pop;
static struct game_state *gstate;

/*
 * create_alien -- constructor for aliens, spawn at random position
 */
static int
create_alien(PMEMobjpool *_pop, void *ptr, void *arg)
{
	struct alien *a = ptr;
	a->y = 1;
	a->x = RRAND(2, GAME_WIDTH - 2);
	a->timer = 1;

	pmemobj_persist(_pop, a, sizeof(*a));

	return 0;
}

/*
 * create_player -- constructor for the player, spawn in the middle of the map
 */
static int
create_player(PMEMobjpool *_pop, void *ptr, void *arg)
{
	struct player *p = ptr;
	p->x = GAME_WIDTH / 2;
	p->timer = 1;

	pmemobj_persist(_pop, p, sizeof(*p));

	return 0;
}

/*
 * create_bullet -- constructor for bullets, spawn at the position of the player
 */
static int
create_bullet(PMEMobjpool *_pop, void *ptr, void *arg)
{
	struct bullet *b = ptr;
	struct player *p = arg;

	b->x = p->x;
	b->y = PLAYER_Y - 1;
	b->timer = 1;

	pmemobj_persist(_pop, b, sizeof(*b));

	return 0;
}

static void
draw_border(void)
{
	for (int x = 0; x <= GAME_WIDTH; ++x) {
		mvaddch(0, x, ACS_HLINE);
		mvaddch(GAME_HEIGHT, x, ACS_HLINE);
	}
	for (int y = 0; y <= GAME_HEIGHT; ++y) {
		mvaddch(y, 0, ACS_VLINE);
		mvaddch(y, GAME_WIDTH, ACS_VLINE);
	}
	mvaddch(0, 0, ACS_ULCORNER);
	mvaddch(GAME_HEIGHT, 0, ACS_LLCORNER);
	mvaddch(0, GAME_WIDTH, ACS_URCORNER);
	mvaddch(GAME_HEIGHT, GAME_WIDTH, ACS_LRCORNER);
}

static void
draw_alien(const TOID(struct alien) a)
{
	mvaddch(D_RO(a)->y, D_RO(a)->x, ACS_DIAMOND|COLOR_PAIR(C_ALIEN));
}

static void
draw_player(const TOID(struct player) p)
{
	mvaddch(PLAYER_Y, D_RO(p)->x, ACS_DIAMOND|COLOR_PAIR(C_PLAYER));
}

static void
draw_bullet(const TOID(struct bullet) b)
{
	mvaddch(D_RO(b)->y, D_RO(b)->x, ACS_BULLET|COLOR_PAIR(C_BULLET));
}

static void
draw_score(void)
{
	mvprintw(1, 1, "Score: %u | %u\n", gstate->score, gstate->high_score);
}

/*
 * timer_tick -- very simple persistent timer
 */
static int
timer_tick(uint32_t *timer)
{
	int ret = *timer == 0 || ((*timer)--) == 0;
	pmemobj_persist(pop, timer, sizeof(*timer));
	return ret;
}

/*
 * update_score -- change player score and global high score
 */
static void
update_score(int m)
{
	if (m < 0 && gstate->score == 0)
		return;

	uint16_t score = gstate->score + m;
	uint16_t highscore = score > gstate->high_score ?
		score : gstate->high_score;
	struct game_state s = {
		.timer = gstate->timer,
		.score = score,
		.high_score = highscore
	};

	*gstate = s;
	pmemobj_persist(pop, gstate, sizeof(*gstate));
}

/*
 * process_aliens -- process spawn and movement of the aliens
 */
static void
process_aliens(void)
{
	/* alien spawn timer */
	if (timer_tick(&gstate->timer)) {
		gstate->timer = RRAND(MIN_GSTATE_TIMER, MAX_GSTATE_TIMER);
		pmemobj_persist(pop, gstate, sizeof(*gstate));
		POBJ_NEW(pop, NULL, struct alien, create_alien, NULL);
	}

	TOID(struct alien) iter, next;
	POBJ_FOREACH_SAFE_TYPE(pop, iter, next) {
		if (timer_tick(&D_RW(iter)->timer)) {
			D_RW(iter)->timer = MAX_ALIEN_TIMER;
			D_RW(iter)->y++;
		}
		pmemobj_persist(pop, D_RW(iter), sizeof(struct alien));
		draw_alien(iter);

		/* decrease the score if the ship wasn't intercepted */
		if (D_RO(iter)->y > GAME_HEIGHT - 1) {
			POBJ_FREE(&iter);
			update_score(-1);
		}
	}
}

/*
 * process_collision -- search for any aliens on the position of the bullet
 */
static int
process_collision(const TOID(struct bullet) b)
{
	TOID(struct alien) iter;
	POBJ_FOREACH_TYPE(pop, iter) {
		if (D_RO(b)->x == D_RO(iter)->x &&
			D_RO(b)->y == D_RO(iter)->y) {
			update_score(1);
			POBJ_FREE(&iter);
			return 1;
		}
	}

	return 0;
}

/*
 * process_bullets -- process bullets movement and collision
 */
static void
process_bullets(void)
{
	TOID(struct bullet) iter, next;

	POBJ_FOREACH_SAFE_TYPE(pop, iter, next) {
		/* bullet movement timer */
		if (timer_tick(&D_RW(iter)->timer)) {
			D_RW(iter)->timer = MAX_BULLET_TIMER;
			D_RW(iter)->y--;
		}
		pmemobj_persist(pop, D_RW(iter), sizeof(struct bullet));

		draw_bullet(iter);
		if (D_RO(iter)->y == 0 || process_collision(iter))
			POBJ_FREE(&iter);
	}
}

/*
 * process_player -- handle player actions
 */
static void
process_player(int input)
{
	TOID(struct player) plr = POBJ_FIRST(pop, struct player);

	/* weapon cooldown tick */
	timer_tick(&D_RW(plr)->timer);
	int plr_updated = 0;

	switch (input) {
	case KEY_LEFT:
	case 'o':
		{
			uint16_t dstx = D_RO(plr)->x - 1;
			if (dstx != 0) {
				D_RW(plr)->x = dstx;
				plr_updated = 1;
			}
		}
		break;

	case KEY_RIGHT:
	case 'p':
		{
			uint16_t dstx = D_RO(plr)->x + 1;
			if (dstx != GAME_WIDTH - 1) {
				D_RW(plr)->x = dstx;
				plr_updated = 1;
			}
		}
		break;

	case ' ':
		if (D_RO(plr)->timer == 0) {
			D_RW(plr)->timer = MAX_PLAYER_TIMER;
			plr_updated = 1;
			POBJ_NEW(pop, NULL, struct bullet,
					create_bullet, D_RW(plr));
		}
		break;

	default:
		break;
	}

	if (plr_updated)
		pmemobj_persist(pop, D_RW(plr), sizeof(struct player));

	draw_player(plr);
}

/*
 * game_loop -- process drawing and logic of the game
 */
static void
game_loop(int input)
{
	erase();
	draw_score();
	draw_border();
	process_aliens();
	process_bullets();
	process_player(input);
	usleep(STEP);
	refresh();
}

int
main(int argc, char *argv[])
{
	if (argc != 2) {
		printf("usage: %s file-name\n", argv[0]);
		return 1;
	}

	const char *path = argv[1];

	pop = NULL;

	srand(time(NULL));

	if (access(path, F_OK) != 0) {
		if ((pop = pmemobj_create(path, POBJ_LAYOUT_NAME(pminvaders),
		    PMINVADERS_POOL_SIZE, S_IWUSR | S_IRUSR)) == NULL) {
			printf("failed to create pool\n");
			return 1;
		}

		/* create the player and initialize with a constructor */
		POBJ_NEW(pop, NULL, struct player, create_player, NULL);
	} else {
		if ((pop = pmemobj_open(path, LAYOUT_NAME)) == NULL) {
			printf("failed to open pool\n");
			return 1;
		}
	}

	/* global state of the game is kept in the root object */
	TOID(struct game_state) game_state = POBJ_ROOT(pop, struct game_state);

	gstate = D_RW(game_state);

	initscr();
	start_color();
	init_pair(C_PLAYER, COLOR_GREEN, COLOR_BLACK);
	init_pair(C_ALIEN, COLOR_RED, COLOR_BLACK);
	init_pair(C_BULLET, COLOR_YELLOW, COLOR_BLACK);
	nodelay(stdscr, true);
	curs_set(0);
	keypad(stdscr, true);

	int in;
	while ((in = getch()) != 'q') {
		game_loop(in);
	}

	pmemobj_close(pop);

	endwin();

	return 0;
}
