// SPDX-License-Identifier: GPL-2.0
#include "glist.h"

#include <stdlib.h>
#include <stdio.h>

/* insereaza la sfarsitul listei un element */
void insert_last(void *element, list *l)
{
	list temp = NULL;

	temp = malloc(sizeof(struct cell));
	if (temp == NULL) {
		fprintf(stderr, "Failed to allocate memory for new element!\n");
		exit(0);
	}
	temp->element = element;
	/* conventia e ca ultimul element din lista sa nu aiba succesor */
	temp->next = NULL;

	/* daca lista era goala, elementul nou creat devine capul listei */
	if (*l == NULL) {
		*l = temp;
	} else {
		/* adaugam elementul la sfarsit */
		temp->prev = (*l)->prev;
		if ((*l)->prev != NULL)
			(*l)->prev->next = temp;
		else
			(*l)->next = temp;
	}
	/* primul element din lista are ultimul element ca predecesor */
	(*l)->prev = temp;
}

/* sterge primul element din lista si il returneaza */
void *remove_first(list *l)
{
	void *element = NULL;

	if (*l != NULL) {
		list new_first = (*l)->next;
		list last = (*l)->prev;
		/* primul element din lista are ultimul element ca predecesor */
		if (new_first)
			new_first->prev = last;
		element = (*l)->element;
		free(*l);
		*l = new_first;
	} else {
		fprintf(stderr, "List is already empty! Cannot delete!\n");
	}
	return element;
}

/* verifica daca lista e goala sau nu(returneaza 1/0) */
int is_empty(list l)
{
	if (l)
		return 0;
	else
		return 1;
}
