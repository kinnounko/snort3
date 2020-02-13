//--------------------------------------------------------------------------
// Copyright (C) 2014-2020 Cisco and/or its affiliates. All rights reserved.
// Copyright (C) 2003-2013 Sourcefire, Inc.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------

/*
     hashfcn.c

     Each hash table allocates it's own GHash struct, using the number of
     rows in the hash table to modulo the random values.

     Updates:

     8/31/2006 - man - changed to use sfprimetable.c
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "hashfcn.h"

#include <cassert>

#include "main/snort_config.h"
#include "utils/util.h"

#include "primetable.h"

using namespace snort;

static bool hashfcn_key_compare(const void* k1, const void* k2, size_t len)
{
    if ( memcmp(k1, k2, len ) == 0 )
        return true;
    else
        return false;
}

HashFnc* hashfcn_new(int m)
{
    HashFnc* p;
    static int one = 1;

    if ( one ) /* one time init */
    {
        srand( (unsigned)time(nullptr) );
        one = 0;
    }

    p = (HashFnc*)snort_calloc(sizeof(*p));

    if ( SnortConfig::static_hash() )
    {
        p->seed     = 3193;
        p->scale    = 719;
        p->hardener = 133824503;
    }
    else
    {
        p->seed = nearest_prime( (rand() % m) + 3191);
        p->scale = nearest_prime( (rand() % m) + 709);
        p->hardener = ((unsigned) rand() * rand()) + 133824503;
    }

    p->hash_fcn = &hashfcn_hash;
    p->keycmp_fcn = &hashfcn_key_compare;

    return p;
}

void hashfcn_free(HashFnc* p)
{
    if ( p )
        snort_free(p);
}

unsigned hashfcn_hash(HashFnc* p, const unsigned char* d, int n)
{
    unsigned hash = p->seed;
    while ( n )
    {
        hash *=  p->scale;
        hash += *d++;
        n--;
    }
    return hash ^ p->hardener;
}

/**
 * Make hashfcn use a separate set of opcodes for the backend.
 *
 * @param h hashfcn ptr
 * @param hash_fcn user specified hash function
 * @param keycmp_fcn user specified key comparison function
 */
void hashfcn_set_keyops(HashFnc* h, hash_func hash_fcn, keycmp_func keycmp_fcn)
{
    assert(h && hash_fcn && keycmp_fcn);

    h->hash_fcn = hash_fcn;
    h->keycmp_fcn = keycmp_fcn;
}

namespace snort
{
void mix_str(uint32_t& a, uint32_t& b, uint32_t& c, const char* s, unsigned n)
{
    unsigned i, j;

    if ( !n )
        n = strlen(s);

    for ( i=0,j=0; i<n; i+=4 )
    {
        uint32_t tmp = 0;
        unsigned k = n - i;

        if (k > 4)
            k=4;

        for (unsigned l=0; l<k; l++)
            tmp |= (unsigned char) s[i + l] << l*8;

        switch (j)
        {
        case 0:
            a += tmp;
            break;
        case 1:
            b += tmp;
            break;
        case 2:
            c += tmp;
            break;
        }
        j++;

        if (j == 3)
        {
            mix(a,b,c);
            j = 0;
        }
    }

    if (j != 0)
        mix(a,b,c);
}

uint32_t str_to_hash(const uint8_t *str, size_t length)
{
    uint32_t a = 0, b = 0, c = 0;

    for (size_t i = 0, j = 0; i < length; i += 4)
    {
        uint32_t tmp = 0;
        size_t k = length - i;

        if (k > 4)
            k = 4;

        for (size_t m = 0; m < k; m++)
            tmp |= *(str + i + m) << m * 8;

        switch (j)
        {
        case 0:
            a += tmp;
            break;
        case 1:
            b += tmp;
            break;
        case 2:
            c += tmp;
            break;
        }
        j++;

        if (j == 3)
        {
            mix(a, b, c);
            j = 0;
        }
    }

    finalize(a, b, c);
    return c;
}
} //namespace snort
