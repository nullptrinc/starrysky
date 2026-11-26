/* SPDX-License-Identifier: MIT */
/* What the TV home screen (tv.c) and the TV app menu (tvmenu.c) share:
 * card sizes, the lift and its springs, and the card art. */
#ifndef STARRY_TV_H
#define STARRY_TV_H

#include <stdbool.h>
#include "starboard.h"

#define MARGIN_X 96 /* overscan-safe area: 5% of 1920 */
#define CARD_W 300
#define CARD_H 180
#define CARD_RADIUS 20
#define CARD_GAP 44
#define LIFT 1.1
#define LIFTED_W 330 /* CARD_W * LIFT: the size cards are drawn at */
#define LIFTED_H 198
#define BLUR 36 /* the lifted card's shadow */
#define LEAN 8 /* px the focused card leans after a move */
#define REPEAT_MAX 10 /* focus moves per second while an arrow is held */
#define TEXT 0xfff2f4f8
#define TEXT_SECONDARY 0xff9aa3b2
/* The focus lift: slightly underdamped, it overshoots by about 9%. */
#define LIFT_STIFFNESS 260
#define LIFT_DAMPING 19.4

/* One damped spring, stepped for dt seconds; false once at rest on target. */
bool tv_spring(double *x, double *v, double target, double stiffness, double damping, double dt);
/* An app's card, drawn at its lifted size: its colour, its icon or initial. */
struct canvas *tv_card_art(struct starboard *sb, struct sb_app *app);

#endif
