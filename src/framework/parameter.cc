//--------------------------------------------------------------------------
// Copyright (C) 2014-2022 Cisco and/or its affiliates. All rights reserved.
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
// parameter.cc author Russ Combs <rucombs@cisco.com>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "parameter.h"

#include <cassert>

#include <iomanip>
#include <sstream>
#include <vector>

#include "utils/dnet_header.h"

#include "value.h"

using namespace snort;
using namespace std;

//--------------------------------------------------------------------------
// helpers
//--------------------------------------------------------------------------

static bool is_sep(char c)
{ return !c || c == '|' || isspace(c); }

static const char* find(const char* r, const char* s)
{
    size_t n = strlen(s);

    if ( !n )
        return nullptr;

    const char* t = strstr(r, s);

    while ( t )
    {
        if ( (t == r || is_sep(t[-1])) && is_sep(t[n]) )
            return t;

        t = strstr(t+n, s);
    }
    return nullptr;
}

static unsigned get_index(const char* r, const char* t)
{
    unsigned idx = 0;
    const char* p = strchr(r, '|');

    while ( p && p < t )
    {
        ++idx;
        p = strchr(p+1, '|');
    }
    return idx;
}

#define delim " \t\n"

static size_t split(const string& txt, vector<string>& strs)
{
    size_t last = txt.find_first_not_of(delim);
    size_t pos = txt.find_first_of(delim, last);
    strs.clear();

    while ( pos != string::npos )
    {
        if ( last != pos )
            strs.emplace_back(txt.substr(last, pos - last));

        last = txt.find_first_not_of(delim, pos + 1);
        pos = txt.find_first_of(delim, last);
    }

    // add the last one
    if ( last != string::npos )
        strs.emplace_back(txt.substr(last, min(pos, txt.size()) - last));

    return strs.size();
}

int64_t Parameter::get_int(const char* r)
{
    if ( *r == 'm' )
    {
        if ( !strncmp(r, "maxSZ", 5) )
            r = (sizeof(size_t) == 4) ? "max32" : "max53";

        if ( !strncmp(r, "max31", 5) )
            return 2147483647;

        if ( !strncmp(r, "max32", 5) )
            return 4294967295;

        if ( !strncmp(r, "max53", 5) )
            return 9007199254740992;
    }
    char* end = nullptr;
    int64_t i = (int64_t)strtoll(r, &end, 0);
    assert(!*end or *end == ':');

    return i;
}

//--------------------------------------------------------------------------
// validation methods
//--------------------------------------------------------------------------

static bool valid_bool(const Value& v, const char*)
{
    return v.get_type() == Value::VT_BOOL;
}

// FIXIT-L allow multiple , separated ranges
static bool valid_int(const Value& v, const char* r)
{
    if ( v.get_type() != Value::VT_NUM )
        return false;

    if ( v.get_real() != v.get_int64() )
        return false;

    if ( !r )
        return true;

    int64_t d = v.get_int64();

    // require no leading or trailing whitespace
    // and either # | #: | :# | #:#
    // where # is a valid pos or neg dec, hex, or octal number

    const char* t = strchr(r, ':');

    if ( *r != ':' )
    {
        int64_t low = Parameter::get_int(r);

        if ( d < low )
            return false;

        if ( !t )
            return d == low;
    }

    if ( t and *++t )
    {
        int64_t hi = Parameter::get_int(t);

        if ( d > hi )
            return false;
    }
    return true;
}

// interval is a special case because we support a<>b and a<=>b for convenience.
// if not for that, then dsize:1<>10; would be dsize:>1, <10; (2 parameters) but
// that is the same as dsize:>1; dsize:<10; which is arguably easier to read and
// not significantly worse performance and which we also, obviously, already
// support.  and note that <> and <=> are non-standard Snort-isms.  so, we wind
// up with a multivalued parameter which is best handled as a string.  validation
// must be done by the user.  the advantage of using an interval instead of string
// is that we can document the type in one place and the parameters can focus on
// their actual, specific semantics instead of trying to explain the syntax.  this
// also ensures that an int-type range is not applied to a string.

static bool valid_interval(const Value&, const char*)
{ return true; }

// FIXIT-L allow multiple , separated ranges
static bool valid_real(const Value& v, const char* r)
{
    if ( v.get_type() != Value::VT_NUM )
        return false;

    if ( !r )
        return true;

    double d = v.get_real();

    // require no leading or trailing whitespace
    // and either # | #: | :# | #:#
    // where # is a valid pos or neg dec, hex, or octal number

    const char* t = strchr(r, ':');

    if ( *r != ':' )
    {
        double low = strtod(r, nullptr);

        if ( d < low )
            return false;

        if ( !t )
            return d == low;
    }

    if ( t && *++t )
    {
        double hi = strtod(t, nullptr);

        if ( d > hi )
            return false;
    }
    return true;
}

static bool valid_string(const Value& v, const char* r)
{
    if ( v.get_type() != Value::VT_STR )
        return false;

    if ( r && !strcmp(r, "(optional)") )
        return true;

    size_t len = strlen(v.get_string());

    if ( !r )
        return len > 0;

    size_t max = strtoul(r, nullptr, 0);
    return len <= max;
}

static bool valid_select(const Value& v, const char* r)
{
    if ( v.get_type() != Value::VT_STR )
        return false;

    if ( !r )
        return false;

    const char* s = v.get_string();
    const char* t = find(r, s);

    if ( !t )
        return false;

    return true;
}

static bool valid_enum(Value& v, const char* r)
{
    if ( v.get_type() != Value::VT_STR )
        return false;

    if ( !r )
        return false;

    const char* s = v.get_string();
    const char* t = find(r, s);

    if ( !t )
        return false;

    unsigned idx = get_index(r, t);

    v.set_enum(idx);
    return true;
}

static bool valid_multi(Value& v, const char* r)
{
    if ( v.get_type() != Value::VT_STR )
        return false;

    if ( !r )
        return false;

    string s = v.get_string();
    vector<string> list;
    split(s, list);

    uint64_t mask = 0;

    for ( const auto& p : list )
    {
        const char* t = find(r, p.c_str());
        if ( !t )
            return false;

        uint64_t idx = get_index(r, t);

        if ( idx < Value::mask_bits )
            mask |= (1ULL << idx);
    }
    v.set_aux(mask);
    return true;
}

static bool valid_mac(Value& v, const char*)
{
    if ( v.get_type() != Value::VT_STR )
        return false;

    struct addr a;

    if ( addr_pton(v.get_string(), &a) )
        return false;

    if ( a.addr_type == ADDR_TYPE_ETH )
        v.set(a.addr_data8, 6);

    else
        return false;

    return true;
}

static bool valid_ip4(Value& v, const char*)
{
    if ( v.get_type() != Value::VT_STR )
        return false;

    uint32_t ip4 = inet_addr(v.get_string());

    if ( ip4 == INADDR_NONE )
        return false;

    v.set((double)ip4);
    return true;
}

static bool valid_addr(Value& v, const char*)
{
    if ( v.get_type() != Value::VT_STR )
        return false;

    struct addr a;

    if ( addr_pton(v.get_string(), &a) )
        return false;

    if ( a.addr_type == ADDR_TYPE_IP )
        v.set(a.addr_data8, 4);

    else if ( a.addr_type == ADDR_TYPE_IP6 )
        v.set(a.addr_data8, 16);

    else
        return false;

    return true;
}

static bool valid_bit_list(Value& v, const char* r)
{
    if ( v.get_type() != Value::VT_STR )
        return false;

    string pl = v.get_string();
    string bs;

    size_t max = r ? strtoul(r, nullptr, 0) : 0;
    assert(max > 0);

    if ( pl == "any" )
    {
        bs.assign(max+1, '1');
        v.set(bs.c_str());
        return true;
    }
    stringstream ss(pl);
    ss >> setbase(0);

    bs.assign(max+1, '0');
    int bit;

    while ( ss >> bit )
    {
        if ( bit < 0 || (size_t)bit > max )
            return false;

        bs[bit] = '1';
    }
    if ( !ss.eof() )
        return false;

    v.set(bs.c_str());
    return true;
}

//--------------------------------------------------------------------------
// Parameter methods
//--------------------------------------------------------------------------

bool Parameter::validate(Value& v) const
{
    switch ( type )
    {
    // bool values
    case PT_BOOL:
        return valid_bool(v, (const char*)range);

    // num values
    case PT_PORT:
        if ( !range )
            return valid_int(v, "0:65535");
        // fall through
    case PT_INT:
        return valid_int(v, (const char*)range);
    case PT_INTERVAL:
        return valid_interval(v, (const char*)range);
    case PT_REAL:
        return valid_real(v, (const char*)range);

    // string values
    case PT_STRING:
        return valid_string(v, (const char*)range);
    case PT_SELECT:
        return valid_select(v, (const char*)range);
    case PT_MULTI:
        return valid_multi(v, (const char*)range);
    case PT_ENUM:
        return valid_enum(v, (const char*)range);
    case PT_DYNAMIC:
        return valid_select(v, (*((const RangeQuery*)range))());

    // address values
    case PT_MAC:
        return valid_mac(v, (const char*)range);
    case PT_IP4:
        return valid_ip4(v, (const char*)range);
    case PT_ADDR:
        return valid_addr(v, (const char*)range);

    // list values
    case PT_BIT_LIST:
        return valid_bit_list(v, (const char*)range);

    case PT_ADDR_LIST:
    case PT_IMPLIED:
        return true;

    default:
        break;
    }
    return false;
}

static const char* const pt2str[Parameter::PT_MAX] =
{
    "table", "list", "dynamic",
    "bool", "int", "interval", "real", "port",
    "string", "select", "multi", "enum",
    "mac", "ip4", "addr",
    "bit_list", "addr_list", "implied"
};

const char* Parameter::get_type() const
{
    assert(type < Parameter::PT_MAX);
    return pt2str[type];
}

const char* Parameter::get_range() const
{
    switch ( type )
    {
    case PT_TABLE:
    case PT_LIST:
        return nullptr;

    case PT_DYNAMIC:
        return (*((const RangeQuery*)range))();

    default:
        break;
    }
    return (const char*)range;
}

bool Parameter::get_bool() const
{
    if ( !deflt )
        return false;

    return ( strchr(deflt, 't') || strchr(deflt, 'T') );
}

double Parameter::get_number() const
{
    if ( !deflt )
        return 0;

    return strtod(deflt, nullptr);
}

const char* Parameter::get_string() const
{
    return deflt ? deflt : "";
}

const Parameter* Parameter::find(const Parameter* p, const char* s)
{
    if ( !p )
        return nullptr;

    while ( p->name )
    {
        if ( !strcmp(p->name, s) || p->is_wild_card() )
            return p;
        ++p;
    }
    return nullptr;
}

int Parameter::index(const char* r, const char* s)
{
    const char* t = ::find(r, s);

    if ( !t )
        return -1;

    unsigned idx = get_index(r, t);
    return (int)idx;
}

//--------------------------------------------------------------------------
// valid_* tests
// we only test validation here
// side effects applied to value are tested elsewhere
//--------------------------------------------------------------------------

#ifdef CATCH_TEST_BUILD

#include "catch/catch.hpp"

TEST_CASE("bool", "[Parameter]")
{
    Value v(true);
    CHECK(valid_bool(v, nullptr));
}

struct
{
    bool expected;
    bool (*validate)(const Value&, const char*);
    double value;
    const char* range;
}
num_tests[] =
{
// __STRDUMP_DISABLE__
    { true, valid_int, 0, nullptr },
    { true, valid_int, 0, "" },
    { true, valid_int, 0, "0" },
    { true, valid_int, 0, "0:" },
    { true, valid_int, 0, ":0" },
    { true, valid_int, 0, ":1" },
    { true, valid_int, 0, "-1:1" },
    { true, valid_int, 0, "-1:" },

    { false, valid_int, 1, "0" },
    { true, valid_int, 1, "0:" },
    { false, valid_int, 1, ":0" },

    { false, valid_int, 1.5, ":0" },

    { true, valid_int, -10, "-11:-9" },
    { true, valid_int, 10, "9:11" },
    { true, valid_int, 10, "0xA:11" },

    { true, valid_interval, 0, nullptr },

    { true, valid_real, 0.0, nullptr },
    { true, valid_real, 0.0, "" },
    { true, valid_real, 0.0, "0.0" },
    { true, valid_real, 0.0, ":0" },

    { true, valid_real, 0.1, "0:" },
    { true, valid_real, 0.1, ":0.9" },
    { true, valid_real, 0.1, "-0.9:0.9" },
    { true, valid_real, 0.1, "-0.9:" },

    { false, valid_real, 1, "0.9" },
    { true, valid_real, 1, "0.9:" },
    { false, valid_real, 1, ":0.9" },

    { true, valid_real, -10, "-11.1:-9.9" },
    { true, valid_real, 10, "9.9:11.1" },
    { false, valid_real, 10, "011:11" },
    { true, valid_real, 10, "0xA:11" },

    { false, nullptr, 0, nullptr }
// __STRDUMP_ENABLE__
};

TEST_CASE("num", "[Parameter]")
{
    auto test = num_tests;

    while ( test->validate )
    {
        Value v(test->value);
        bool result = test->validate(v, test->range);
        CHECK(result == test->expected);
        ++test;
    }
}

struct
{
    bool expected;
    bool (*validate)(const Value&, const char*);
    const char* value;
    const char* range;
}
const_string_tests[] =
{
// __STRDUMP_DISABLE__
    { true, valid_string, "green", "(optional)" },
    { true, valid_string, "green", nullptr },
    { true, valid_string, "green", "5" },
    { true, valid_string, "green", "6" },
    { false, valid_string, "green", "4" },

    { true, valid_select, "green", "red | green | yellow" },
    { false, valid_select, "blue", "red | green | yellow" },
    { false, valid_select, "green", nullptr },

    { false, nullptr, nullptr, nullptr }
// __STRDUMP_ENABLE__
};

struct
{
    bool expected;
    bool (*validate)(Value&, const char*);
    const char* value;
    const char* range;
}
string_tests[] =
{
// __STRDUMP_DISABLE__
    { true, valid_enum, "green", "red | green | yellow" },
    { false, valid_enum, "blue", "red | green | yellow" },
    { false, valid_enum, "green", nullptr },

    { true, valid_multi, "green", "red | green | yellow" },
    { true, valid_multi, "red yellow", "red | green | yellow" },
    { false, valid_multi, "redgreen", "red | green | yellow" },
    { false, valid_multi, "blue", nullptr },

    { true, valid_mac, "98:01:a7:9d:d8:41", nullptr },
    { false, valid_mac, ":01:a7:9d:d8:41", nullptr },
    { false, valid_mac, "01:a7:9d:d8:41", nullptr },
    { false, valid_mac, "98:01:a7:9d:d8:419", nullptr },
    { false, valid_mac, "98:01:a7:9d:d8:41x", nullptr },

    { true, valid_ip4, "1.2.3.4", nullptr },
    { true, valid_ip4, "1.2.3", nullptr },
    { false, valid_ip4, "1.2.3.", nullptr },
    { false, valid_ip4, "1.2.x", nullptr },

    { true, valid_addr, "1.2.3.4", nullptr },
    { true, valid_addr, "1.2.3.4/32", nullptr },
    { true, valid_addr, "1.2.3.4/0", nullptr },
    { false, valid_addr, "1.2.3.4/33", nullptr },
    { false, valid_addr, "1.2.0x.4/33", nullptr },

    { true, valid_addr, "2001:420:c0c4:1004::157", nullptr },
    { true, valid_addr, "2001:420:c0c4:1004::157/128", nullptr },
    { true, valid_addr, "2001:420:c0c4:1004::157/0", nullptr },
    { false, valid_addr, "2001:420:c0c4:1004:0x:157/256", nullptr },

    { true, valid_bit_list, "1 2", "3" },
    { true, valid_bit_list, "1 2 3", "3" },
    { false, valid_bit_list, "1 2 3 4", "3" },
    { false, valid_bit_list, "128", "3" },

    { false, nullptr, nullptr, nullptr }
// __STRDUMP_ENABLE__
};

TEST_CASE("string", "[Parameter]")
{
    auto ctest = const_string_tests;
    while ( ctest->validate )
    {
        Value v(ctest->value);
        bool result = ctest->validate(v, ctest->range);
        CHECK(result == ctest->expected);
        ++ctest;
    }

    auto test = string_tests;
    while ( test->validate )
    {
        Value v(test->value);
        bool result = test->validate(v, test->range);
        CHECK(result == test->expected);
        ++test;
    }
}

TEST_CASE("max", "[Parameter]")
{
    CHECK(Parameter::get_int("max31") == 2147483647);
    CHECK(Parameter::get_int("max32") == 4294967295);
    CHECK(Parameter::get_int("max53") == 9007199254740992);

    if ( sizeof(size_t) == 4 )
        CHECK(Parameter::get_int("maxSZ") == 4294967295);
    else
        CHECK(Parameter::get_int("maxSZ") == 9007199254740992);
}

#endif

