/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _GLIST_
#define _GLIST_

typedef struct cell *list;

struct cell {
	void *element;
	list next, prev;
};

void insert_last(void *element, list *l);
void *remove_first(list *l);
int is_empty(list l);

#endif
