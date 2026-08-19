import lldb
import os

OUT_PATH = '/tmp/openttd_airport_inspection.txt'

FTA_STATES = {
    0: "HANGAR",
    1: "TERM1",
    2: "TERM2",
    3: "TERM3",
    4: "TERM4",
    5: "TERM5",
    6: "TERM6",
    7: "TERM7",
    8: "HELI1",
    9: "HELI2",
    10: "HELI3",
    11: "TAKEOFF",
    12: "STARTTAKEOFF",
    13: "ENDTAKEOFF",
    14: "LANDING",
    15: "ENDLANDING",
    16: "HELITAKEOFF",
    17: "HELIENDTAKEOFF",
    18: "HELILANDING",
    19: "HELIENDLANDING",
    20: "FLYING",
}

MGT_NAMES = {
    0: "NONE",
    1: "TERMINAL",
    2: "HELIPAD",
    3: "HANGAR",
    4: "RUNWAY_TAKEOFF",
    5: "ROLLOUT",
}

def idval(v):
    inner = v.GetChildMemberWithName('value')
    return inner.GetValueAsUnsigned(0) if inner.IsValid() else v.GetValueAsUnsigned(0)

def strval(v):
    s = v.GetSummary()
    return s.strip('"') if s else ''

def bitset_val(v, depth=3):
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
    pool = target.FindFirstGlobalVariable(pool_name)
    if not pool.IsValid():
        return
    data = pool.GetChildMemberWithName('data')
    for i in range(data.GetNumChildren()):
        ptr = data.GetChildAtIndex(i)
        if ptr.GetValueAsUnsigned(0) == 0:
            continue
        yield i, ptr.Dereference()

def __lldb_init_module(debugger, internal_dict):
    target = debugger.GetSelectedTarget()
    with open(OUT_PATH, 'w') as out:
        pause_mode = target.FindFirstGlobalVariable('_pause_mode').GetValueAsUnsigned(0)
        out.write(f"=== GAME STATUS ===\n_pause_mode = {pause_mode}\n\n")

        # Map station index to info
        stations = {}
        out.write("=== AIRPORT STATIONS ===\n")
        for i, st in walk_pool(target, '_station_pool'):
            fac = bitset_val(st.GetChildMemberWithName('facilities'))
            if not (fac & 0x08): # not airport
                continue
            name = strval(st.GetChildMemberWithName('cached_name')) or strval(st.GetChildMemberWithName('name'))
            airport = st.GetChildMemberWithName('airport')
            airport_type = airport.GetChildMemberWithName('type').GetValueAsUnsigned(0)
            xy = idval(st.GetChildMemberWithName('xy'))
            
            # Check modular tiles
            mtd = airport.GetChildMemberWithName('modular_tile_data')
            modular_count = 0
            if mtd.GetValueAsUnsigned(0) != 0:
                vec = mtd.Dereference()
                modular_count = vec.GetNumChildren()
            
            stations[i] = {
                'name': name,
                'type': airport_type,
                'xy': xy,
                'modular_count': modular_count
            }
            out.write(f"Station ID {i}: '{name}' xy={xy} airport_type={airport_type} modular_tiles={modular_count}\n")
        
        out.write("\n=== AIRCRAFT VEHICLES ===\n")
        ac_type = target.FindFirstType('Aircraft')
        
        aircraft_count = 0
        stuck_count = 0
        stopped_count = 0
        
        pool = target.FindFirstGlobalVariable('_vehicle_pool')
        data = pool.GetChildMemberWithName('data')
        out.write(f"Pool size: {data.GetNumChildren()}\n")
        
        for i in range(data.GetNumChildren()):
            ptr = data.GetChildAtIndex(i)
            if ptr.GetValueAsUnsigned(0) == 0:
                continue
            
            # Cast ptr to Aircraft*
            if ac_type.IsValid():
                ac_ptr = ptr.Cast(ac_type.GetPointerType())
                ac = ac_ptr.Dereference()
            else:
                ac = ptr.Dereference()
            
            # Check type
            type_val = ac.GetChildMemberWithName('type')
            if not type_val.IsValid():
                bv = ac.GetChildMemberWithName('BaseVehicle')
                if bv.IsValid():
                    type_val = bv.GetChildMemberWithName('type')
            
            t_int = type_val.GetValueAsUnsigned(999) if type_val.IsValid() else 999
            if t_int != 3: # VEH_AIRCRAFT == 3
                continue
            
            subtype_val = ac.GetChildMemberWithName('subtype')
            st_int = subtype_val.GetValueAsUnsigned(999) if subtype_val.IsValid() else 0
            if st_int != 0:
                continue
            
            aircraft_count += 1
            
            unitnum = ac.GetChildMemberWithName('unitnumber').GetValueAsUnsigned(0)
            vehstatus = bitset_val(ac.GetChildMemberWithName('vehstatus'))
            tile = idval(ac.GetChildMemberWithName('tile'))
            speed = ac.GetChildMemberWithName('cur_speed').GetValueAsUnsigned(0)
            subspeed = ac.GetChildMemberWithName('subspeed').GetValueAsUnsigned(0)
            x_pos = ac.GetChildMemberWithName('x_pos').GetValueAsSigned(0)
            y_pos = ac.GetChildMemberWithName('y_pos').GetValueAsSigned(0)
            z_pos = ac.GetChildMemberWithName('z_pos').GetValueAsSigned(0)
            state = ac.GetChildMemberWithName('state').GetValueAsUnsigned(0)
            targetairport = idval(ac.GetChildMemberWithName('targetairport'))
            
            # Aircraft specific fields
            wait_ctr = ac.GetChildMemberWithName('taxi_wait_counter').GetValueAsUnsigned(0)
            path_idx = ac.GetChildMemberWithName('taxi_path_index').GetValueAsUnsigned(0)
            mgt = ac.GetChildMemberWithName('modular_ground_target').GetValueAsUnsigned(0)
            goal = idval(ac.GetChildMemberWithName('ground_path_goal'))
            crashed = ac.GetChildMemberWithName('crashed_counter').GetValueAsUnsigned(0)
            
            # Reserved tiles
            res_tiles = ac.GetChildMemberWithName('taxi_reserved_tiles')
            res_count = res_tiles.GetNumChildren() if res_tiles.IsValid() else 0
            res_list = []
            if res_tiles.IsValid() and res_count > 0:
                for r_idx in range(min(res_count, 10)):
                    elem = res_tiles.GetChildAtIndex(r_idx)
                    res_list.append(idval(elem))
            
            # Runway res
            rw_res = ac.GetChildMemberWithName('modular_runway_reservation')
            rw_count = rw_res.GetNumChildren() if rw_res.IsValid() else 0
            
            is_stopped = bool(vehstatus & 0x02)
            is_hidden = bool(vehstatus & 0x01)
            
            if is_stopped:
                stopped_count += 1
            if wait_ctr > 100:
                stuck_count += 1
            
            state_str = FTA_STATES.get(state, f"UNKNOWN({state})")
            mgt_str = MGT_NAMES.get(mgt, f"UNKNOWN({mgt})")
            
            st_name = stations.get(targetairport, {}).get('name', f"st#{targetairport}")
            stopped_flag = " [USER STOPPED]" if is_stopped else ""
            stuck_flag = f" [HIGH WAIT: {wait_ctr}]" if wait_ctr > 100 else ""
            
            res_str = f"res={res_count} ({res_list})" if res_count > 0 else "res=0"
            out.write(f"V{i} (Unit #{unitnum}){stopped_flag}{stuck_flag}: state={state_str} tile={tile} pos=({x_pos},{y_pos},{z_pos}) speed={speed} target_st='{st_name}' mgt={mgt_str} goal={goal} wait={wait_ctr} path_idx={path_idx} {res_str} rw_res={rw_count} vehstatus=0x{vehstatus:x}\n")
        
        out.write(f"\nSummary: {aircraft_count} aircraft total, {stopped_count} user-stopped, {stuck_count} with wait > 100\n")

    print(f"Inspection complete. Written to {OUT_PATH}")
