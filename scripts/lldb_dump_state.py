# Dump live OpenTTD game state (towns + stations) from a running process.
#
# Usage:
#   lldb -p $(pgrep -f 'build/openttd') --batch \
#     -o 'command script import scripts/lldb_dump_state.py' \
#     -o 'detach' -o 'quit'
#
# Output: /tmp/openttd_state_dump.txt
#
# Read-only: field reads via SBValue only, no expression evaluation, no calls
# into game code. See skills/lldb_game_state_inspection.md for the drilling
# rules this script encodes and how to extend it.

import lldb

OUT_PATH = '/tmp/openttd_state_dump.txt'

FACILITY_NAMES = {0x01: 'Train', 0x02: 'Truck', 0x04: 'Bus', 0x08: 'Airport', 0x10: 'Dock', 0x80: 'Waypoint'}
AT_OILRIG = 9


def strval(v):
    """String payload of a std::string SBValue ('' if empty/unreadable)."""
    s = v.GetSummary()
    return s.strip('"') if s else ''


def idval(v):
    """Payload of a strong-ID wrapper (TileIndex, StationID, ...): child 'value'."""
    inner = v.GetChildMemberWithName('value')
    return inner.GetValueAsUnsigned(0) if inner.IsValid() else v.GetValueAsUnsigned(0)


def bitset_val(v, depth=3):
    """Raw bits of an EnumBitSet/CompanyMask: the deep child named 'data'."""
    if depth < 0 or not v.IsValid():
        return 0
    d = v.GetChildMemberWithName('data')
    if d.IsValid():
        return d.GetValueAsUnsigned(0)
    for i in range(v.GetNumChildren()):
        val = bitset_val(v.GetChildAtIndex(i), depth - 1)
        if val != 0:
            return val
    return 0


def array_elems(v):
    """Element SBValues of a TypedIndexContainer<std::array<...>>."""
    # TypedIndexContainer -> std::array -> __elems_ -> elements
    cur = v
    for _ in range(4):
        if not cur.IsValid():
            return []
        if cur.GetNumChildren() > 1:
            return [cur.GetChildAtIndex(i) for i in range(cur.GetNumChildren())]
        cur = cur.GetChildAtIndex(0)
    return []


def walk_pool(target, pool_name):
    """Yield (index, dereferenced item) for live entries of an OpenTTD pool."""
    pool = target.FindFirstGlobalVariable(pool_name)
    if not pool.IsValid():
        return
    data = pool.GetChildMemberWithName('data')  # std::vector<T*>
    for i in range(data.GetNumChildren()):
        ptr = data.GetChildAtIndex(i)
        if ptr.GetValueAsUnsigned(0) == 0:  # never drill a null pointer
            continue
        yield i, ptr.Dereference()


def facilities_str(bits):
    names = [name for bit, name in sorted(FACILITY_NAMES.items()) if bits & bit]
    return '0x%x(%s)' % (bits, '+'.join(names) if names else 'none')


def dump_settings(target, out):
    settings = target.FindFirstGlobalVariable('_settings_game')
    econ = settings.GetChildMemberWithName('economy')
    diff = settings.GetChildMemberWithName('difficulty')
    noise_on = econ.GetChildMemberWithName('station_noise_level').GetValueAsUnsigned(0)
    tolerance = diff.GetChildMemberWithName('town_council_tolerance').GetValueAsUnsigned(0)
    tnp = econ.GetChildMemberWithName('town_noise_population')
    tnp_vals = [tnp.GetChildAtIndex(i).GetValueAsUnsigned(0) for i in range(tnp.GetNumChildren())]
    tol_names = {0: 'lenient', 1: 'tolerant', 2: 'hostile', 3: 'permissive'}
    out.write('station_noise_level=%d town_council_tolerance=%d(%s) town_noise_population=%s\n' % (
        noise_on, tolerance, tol_names.get(tolerance, '?'), tnp_vals))
    out.write('local_company=%d\n' % idval(target.FindFirstGlobalVariable('_local_company')))


def dump_towns(target, out):
    out.write('\n=== TOWNS ===\n')
    for i, t in walk_pool(target, '_town_pool'):
        name = strval(t.GetChildMemberWithName('cached_name')) or strval(t.GetChildMemberWithName('name'))
        noise = t.GetChildMemberWithName('noise_reached').GetValueAsUnsigned(0)
        pop = t.GetChildMemberWithName('cache').GetChildMemberWithName('population').GetValueAsUnsigned(0)
        have = bitset_val(t.GetChildMemberWithName('have_ratings'))
        ratings = array_elems(t.GetChildMemberWithName('ratings'))
        rated = {c: ratings[c].GetValueAsSigned(0) for c in range(len(ratings)) if have & (1 << c)}
        out.write("town[%d] '%s' xy=%d pop=%d noise_reached=%d ratings=%s\n" % (
            i, name, idval(t.GetChildMemberWithName('xy')), pop, noise, rated))


def dump_stations(target, out):
    out.write('\n=== STATIONS ===\n')
    for i, st in walk_pool(target, '_station_pool'):
        fac = bitset_val(st.GetChildMemberWithName('facilities'))
        airport = st.GetChildMemberWithName('airport')
        airport_type = airport.GetChildMemberWithName('type').GetValueAsUnsigned(0)

        pieces = {}
        mtd = airport.GetChildMemberWithName('modular_tile_data')  # raw std::vector* — may be null
        if mtd.GetValueAsUnsigned(0) != 0:
            vec = mtd.Dereference()
            for k in range(vec.GetNumChildren()):
                pt = vec.GetChildAtIndex(k).GetChildMemberWithName('piece_type').GetValueAsUnsigned(0)
                pieces[pt] = pieces.get(pt, 0) + 1

        town_ptr = st.GetChildMemberWithName('town')
        town_idx = idval(town_ptr.Dereference().GetChildMemberWithName('index')) if town_ptr.GetValueAsUnsigned(0) != 0 else -1
        line = "station[%d] '%s' xy=%d town=%d owner=%d facilities=%s" % (
            i, strval(st.GetChildMemberWithName('name')),
            idval(st.GetChildMemberWithName('xy')),
            town_idx,
            idval(st.GetChildMemberWithName('owner')),
            facilities_str(fac))
        if fac & 0x08:
            line += ' airport_type=%d%s' % (airport_type, '(OILRIG)' if airport_type == AT_OILRIG else '')
        if pieces:
            line += ' modular_pieces=%s' % dict(sorted(pieces.items()))
        out.write(line + '\n')


def __lldb_init_module(debugger, internal_dict):
    target = debugger.GetSelectedTarget()
    with open(OUT_PATH, 'w') as out:
        dump_settings(target, out)
        dump_towns(target, out)
        dump_stations(target, out)
    print('state dump written to ' + OUT_PATH)
