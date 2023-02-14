/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include "list.h"
#include <assert.h>
#include <malloc.h>
struct genlist *list_new(void)
{
    struct genlist *ret =
        (struct genlist *) malloc(sizeof(struct genlist));
    if (!ret)
        return ret;
    ret->key = 0;
    ret->data = 0;
    ret->next = ret;
    ret->prev = ret;
    return ret;
}

struct genlist *list_lookup(struct genlist *l, long key)
{
    struct genlist *curr = l->next;
    while (curr != l && curr->key != key)
        curr = curr->next;
    return curr == l ? 0 : curr;
}

struct genlist *list_insert(struct genlist *l, long key, void *data)
{
    struct genlist *ret =
        (struct genlist *) malloc(sizeof(struct genlist));
    if (!ret)
        return ret;
    ret->key = key;
    ret->data = data;
    ret->next = l->next;
    ret->prev = l;
    l->next->prev = ret;
    l->next = ret;
    return ret;
}

void list_remove(struct genlist *l)
{
    assert(l->prev != l && l->next != l && "Attempt to remove node from empty list?");
    assert(l->prev->next == l && "corrupt list");
    assert(l->next->prev == l && "corrupt list");
    l->next->prev = l->prev;
    l->prev->next = l->next;
    free(l);
}
