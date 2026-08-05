# Dump one modular airport's live state from a running OpenTTD process:
# tile layout with taxi masks, computed helicopter pad, holding-loop gates, and
# every aircraft inside the airport's bounding box.
#
# Usage (dumps all modular airports):
#   lldb -p $(pgrep -f 'build/openttd') --batch \
#     -o 'command script import scripts/lldb_dump_airport.py' \
#     -o 'detach' -o 'quit'
#
# Narrow to one airport with either environment variable:
#   OTTD_AIRPORT_TILE=29289   any tile belonging to the airport
#   OTTD_AIRPORT_STATION=20   station pool index
#
# Output: /tmp/openttd_airport_dump.txt
#
# Read-only: field reads via SBValue only, no expression evaluation, no calls
# into game code. See skills/lldb_game_state_inspection.md for the drilling
# rules this script encodes, and skills/stuck_plane_debugging.md for how to read
# the result.

import os

import lldb

OUT_PATH = '/tmp/openttd_airport_dump.txt'

# Only the piece ids that come up in movement debugging; others print as pNN.
PIECE_NAMES = {
    0: 'APRON', 3: 'STAND', 21: 'ROUND_TERMINAL', 23: 'BUILDING_1', 24: 'DEPOT_SE',
    29: 'EMPTY', 31: 'RADAR_GRASS_FENCE_SW', 32: 'RADIO_TOWER_FENCE_NE',
    45: 'RUNWAY_END', 46: 'RUNWAY_5', 47: 'TOWER',
}
# IsModularRunwayPiece: APT_RUNWAY_1-4, _5, _END, and the three SMALL_* pieces.
# runway_flags is only meaningful on these; elsewhere it holds RUF_DEFAULT noise.
RUNWAY_PIECES = {14, 15, 16, 17, 40, 41, 42, 45, 46}
# Taxi direction / edge-block bits are the same four bits in both masks.
DIR_BITS = {0x1: 'N', 0x2: 'E', 0x4: 'S', 0x8: 'W'}
RUF_BITS = {0x1: 'LAND', 0x2: 'TAKEOFF', 0x4: 'DIR_LOW', 0x8: 'DIR_HIGH'}
AIR_FLAG_BITS = {0: 'DestTooFar', 1: 'InMaxHeightCorr', 2: 'InMinHeightCorr', 3: 'HeliDirectDescent'}
VEH_STATE_BITS = {0: 'Hidden', 1: 'Stopped', 2: 'Unclickable', 3: 'DefaultPalette',
                  4: 'TrainSlowing', 5: 'Shadow', 6: 'AircraftBroken', 7: 'Crashed'}
SUBTYPE_NAMES = {0: 'HELI', 2: 'PLANE'}
INVALID = 0xFFFFFFFF


def idval(v):
    """Payload of a strong-ID wrapper (TileIndex, StationID, ...): child 'value'."""
    inner = v.GetChildMemberWithName('value')
    return inner.GetValueAsUnsigned(0) if inner.IsValid() else v.GetValueAsUnsigned(0)


def bitset_val(v, depth=3):
    """Raw bits of an EnumBitSet: the deep child named 'data'."""
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


def vector_ids(v):
    """List of strong-ID payloads held by a std::vector."""
    return [idval(v.GetChildAtIndex(i)) for i in range(v.GetNumChildren())]


def bits_str(bits, names):
    hit = [name for bit, name in sorted(names.items()) if bits & bit]
    return '0x%x(%s)' % (bits, '+'.join(hit) if hit else '-')


def flags_str(bits, names):
    hit = [name for bit, name in sorted(names.items()) if bits & (1 << bit)]
    return '0x%x[%s]' % (bits, '+'.join(hit))


def xy(tile, w):
    return 'INVALID' if tile == INVALID else '%d,%d' % (tile % w, tile // w)


def dump_layout(entries, w, out):
    """Per-tile metadata. one_way + user_mask together decide legal moves; a
    one-way tile can only be left in its flow direction, so a tile whose flow
    does not lead to the aircraft's goal is a dead end for it."""
    out.write('\n-- tiles --\n')
    out.write('%-8s %-9s %-22s %3s %5s %-12s %-12s %-14s %-12s %s\n' % (
        'tile', 'x,y', 'piece', 'rot', '1way', 'user_mask', 'auto_mask',
        'runway_flags', 'edge_block', 'res_owner'))
    for tile in sorted(entries):
        e = entries[tile]
        piece = e.GetChildMemberWithName('piece_type').GetValueAsUnsigned(0)
        owner = e.GetChildMemberWithName('reservation_owner').GetValueAsUnsigned(0)
        out.write('%-8d %-9s %-22s %3d %5d %-12s %-12s %-14s %-12s %s\n' % (
            tile, xy(tile, w), PIECE_NAMES.get(piece, 'p%d' % piece),
            e.GetChildMemberWithName('rotation').GetValueAsUnsigned(0),
            e.GetChildMemberWithName('one_way_taxi').GetValueAsUnsigned(0),
            bits_str(e.GetChildMemberWithName('user_taxi_dir_mask').GetValueAsUnsigned(0), DIR_BITS),
            bits_str(e.GetChildMemberWithName('auto_taxi_dir_mask').GetValueAsUnsigned(0), DIR_BITS),
            bits_str(e.GetChildMemberWithName('runway_flags').GetValueAsUnsigned(0), RUF_BITS)
                if piece in RUNWAY_PIECES else '-',
            bits_str(e.GetChildMemberWithName('edge_block_mask').GetValueAsUnsigned(0), DIR_BITS),
            'none' if owner >= 0xFFFFF else 'V%d' % owner))


def dump_holding(airport, w, out):
    """Holding loop gates. Colocated parallel runways share a wp_index — they are
    live at the same instant, so gate order and runway availability decide which
    one an arrival takes."""
    loop = airport.GetChildMemberWithName('modular_holding_loop')
    out.write('\n-- holding loop (dirty=%d) --\n' %
              airport.GetChildMemberWithName('modular_holding_loop_dirty').GetValueAsUnsigned(0))
    if loop.GetValueAsUnsigned(0) == 0:
        out.write('not computed yet\n')
        return
    lp = loop.Dereference()
    wps = lp.GetChildMemberWithName('waypoints')
    gates = lp.GetChildMemberWithName('gates')
    out.write('waypoints=%d gates=%d\n' % (wps.GetNumChildren(), gates.GetNumChildren()))
    for k in range(gates.GetNumChildren()):
        g = gates.GetChildAtIndex(k)
        rt = idval(g.GetChildMemberWithName('runway_tile'))
        wpi = g.GetChildMemberWithName('wp_index').GetValueAsUnsigned(0)
        wp = 'out-of-range'
        if wpi < wps.GetNumChildren():
            node = wps.GetChildAtIndex(wpi)
            wp = '%d,%d' % (node.GetChildMemberWithName('x').GetValueAsSigned(0),
                            node.GetChildMemberWithName('y').GetValueAsSigned(0))
        out.write('gate[%d] runway=%d (%s) wp_index=%d wp_px=(%s) approach_px=(%d,%d) '
                  'threshold_px=(%d,%d) dir=%d\n' % (
            k, rt, xy(rt, w), wpi, wp,
            g.GetChildMemberWithName('approach_x').GetValueAsSigned(0),
            g.GetChildMemberWithName('approach_y').GetValueAsSigned(0),
            g.GetChildMemberWithName('threshold_x').GetValueAsSigned(0),
            g.GetChildMemberWithName('threshold_y').GetValueAsSigned(0),
            g.GetChildMemberWithName('approach_dir').GetValueAsUnsigned(0)))


def dump_aircraft(target, bbox, w, out):
    """Aircraft filtered by physical position, not by targetairport — those
    fields go stale on aircraft that stopped being ticked."""
    out.write('\n-- aircraft in bbox (+2 margin) --\n')
    for i, v in walk_pool(target, '_vehicle_pool'):
        if v.GetChildMemberWithName('type').GetValueAsUnsigned(0) != 3:  # VEH_AIRCRAFT
            continue
        sub = v.GetChildMemberWithName('subtype').GetValueAsUnsigned(0)
        if sub not in SUBTYPE_NAMES:  # skip shadows and rotors
            continue
        tile = idval(v.GetChildMemberWithName('tile'))
        if tile == INVALID:
            continue
        x, y = tile % w, tile // w
        if not (bbox[0] - 2 <= x <= bbox[1] + 2 and bbox[2] - 2 <= y <= bbox[3] + 2):
            continue
        out.write('V%-5d unit#%-5d %-5s state=%-3d tile=%-7d (%-7s) vs=%-28s air_flags=%-24s\n'
                  '      mgt=%d goal=%s land_tile=%s land_goal=%s takeoff_tile=%s wait=%d\n'
                  '      taxi_res=%s runway_res=%s\n' % (
            i, v.GetChildMemberWithName('unitnumber').GetValueAsUnsigned(0),
            SUBTYPE_NAMES[sub], v.GetChildMemberWithName('state').GetValueAsUnsigned(0),
            tile, xy(tile, w),
            flags_str(bitset_val(v.GetChildMemberWithName('vehstatus')), VEH_STATE_BITS),
            flags_str(bitset_val(v.GetChildMemberWithName('flags')), AIR_FLAG_BITS),
            v.GetChildMemberWithName('modular_ground_target').GetValueAsUnsigned(0),
            xy(idval(v.GetChildMemberWithName('ground_path_goal')), w),
            xy(idval(v.GetChildMemberWithName('modular_landing_tile')), w),
            xy(idval(v.GetChildMemberWithName('modular_landing_goal')), w),
            xy(idval(v.GetChildMemberWithName('modular_takeoff_tile')), w),
            v.GetChildMemberWithName('taxi_wait_counter').GetValueAsUnsigned(0),
            vector_ids(v.GetChildMemberWithName('taxi_reserved_tiles')),
            vector_ids(v.GetChildMemberWithName('modular_runway_reservation'))))


def __lldb_init_module(debugger, internal_dict):
    target = debugger.GetSelectedTarget()
    want_tile = int(os.environ.get('OTTD_AIRPORT_TILE', '-1'))
    want_station = int(os.environ.get('OTTD_AIRPORT_STATION', '-1'))

    with open(OUT_PATH, 'w') as out:
        w = target.FindFirstGlobalVariable('Map::size_x').GetValueAsUnsigned(0)
        out.write('map_size_x=%d tick=%d\n' % (
            w, target.FindFirstGlobalVariable('TimerGameTick::counter').GetValueAsUnsigned(0)))

        found = 0
        for i, st in walk_pool(target, '_station_pool'):
            airport = st.GetChildMemberWithName('airport')
            mtd = airport.GetChildMemberWithName('modular_tile_data')
            if mtd.GetValueAsUnsigned(0) == 0:
                continue
            vec = mtd.Dereference()
            entries = {}
            for k in range(vec.GetNumChildren()):
                e = vec.GetChildAtIndex(k)
                entries[idval(e.GetChildMemberWithName('tile'))] = e
            if not entries:
                continue
            if want_station >= 0 and i != want_station:
                continue
            if want_tile >= 0 and want_tile not in entries:
                continue

            found += 1
            xs = [t % w for t in entries]
            ys = [t // w for t in entries]
            bbox = (min(xs), max(xs), min(ys), max(ys))
            heli_land = idval(airport.GetChildMemberWithName('modular_heli_landing_tile'))
            heli_take = idval(airport.GetChildMemberWithName('modular_heli_takeoff_tile'))

            out.write('\n=== station %d: %d modular tiles, bbox x%d-%d y%d-%d ===\n' % (
                i, len(entries), bbox[0], bbox[1], bbox[2], bbox[3]))
            # A one-way heli pad deadlocks the corridor it sits on; see
            # skills/reservations-design.md pitfall 7.
            out.write('heli_pad: landing=%d (%s) takeoff=%d (%s) dirty=%d\n' % (
                heli_land, xy(heli_land, w), heli_take, xy(heli_take, w),
                airport.GetChildMemberWithName('modular_heli_tiles_dirty').GetValueAsUnsigned(0)))

            dump_layout(entries, w, out)
            dump_holding(airport, w, out)
            dump_aircraft(target, bbox, w, out)

        if found == 0:
            out.write('\nno matching modular airport found\n')

    print('written ' + OUT_PATH)
