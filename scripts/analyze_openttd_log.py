#!/usr/bin/env python3
import sys
import re
from collections import defaultdict, Counter

LOG_PATH = '/tmp/openttd.log'

def analyze():
    print(f"Analyzing {LOG_PATH}...")

    invariants = Counter()
    fallbacks = Counter()
    stuck_by_type = Counter()
    stuck_no_path = defaultdict(int)
    stuck_reserve = defaultdict(lambda: {'max_wait': 0, 'count': 0, 'reasons': Counter(), 'last_line': ''})
    landing_fails = Counter()
    takeoff_fails = Counter()

    # We will also keep track of the most recent lines (last 50k lines)
    recent_lines = []

    total_lines = 0
    with open(LOG_PATH, 'r', errors='replace') as f:
        for line in f:
            total_lines += 1

            if 'invariant' in line:
                if 'runway-transit-invariant' in line:
                    invariants['runway-transit-invariant'] += 1
                elif 'landing-chain-invariant' in line:
                    invariants['landing-chain-invariant'] += 1
                elif 'runway-rest-invariant' in line:
                    invariants['runway-rest-invariant'] += 1
                else:
                    invariants['other-invariant'] += 1

            if '[FALLBACK]' in line:
                # extract fallback type
                m = re.search(r'\[FALLBACK\]\s*([^:\s]+)', line)
                fb_type = m.group(1) if m else 'unknown'
                fallbacks[fb_type] += 1

            if 'stuck(' in line:
                if 'stuck(no-path)' in line:
                    stuck_by_type['stuck(no-path)'] += 1
                    # Extract vehicle and tile
                    m = re.search(r'(V\d+)\s+(unit#\d+)?.*tile=(\d+)\s+goal=(\d+)\s+tgt=(\d+)', line)
                    if m:
                        vid = m.group(1)
                        unit = m.group(2) or ''
                        tile = m.group(3)
                        goal = m.group(4)
                        tgt = m.group(5)
                        stuck_no_path[f"{vid} {unit} (tile={tile} goal={goal} tgt={tgt})"] += 1
                elif 'stuck(reserve)' in line:
                    stuck_by_type['stuck(reserve)'] += 1
                    m = re.search(r'(V\d+)\s+(unit#\d+)?.*wait=(\d+).*deny=([^\s]+).*deny_tile=(\d+).*deny_by=([^\s]+)', line)
                    if m:
                        vid = m.group(1)
                        unit = m.group(2) or ''
                        wait = int(m.group(3))
                        deny = m.group(4)
                        dtile = m.group(5)
                        dby = m.group(6)
                        entry = stuck_reserve[f"{vid} {unit}"]
                        entry['count'] += 1
                        if wait > entry['max_wait']:
                            entry['max_wait'] = wait
                        entry['reasons'][f"deny={deny} tile={dtile} by={dby}"] += 1
                        entry['last_line'] = line.strip()
                elif 'stuck(occupied)' in line:
                    stuck_by_type['stuck(occupied)'] += 1

            if 'landing-chain fail' in line:
                m = re.search(r'reason=([^\s]+)', line)
                reason = m.group(1) if m else 'unknown'
                landing_fails[reason] += 1

            if 'takeoff' in line and ('FindRunway=INVALID' in line or 'takeoff-path invalid' in line or 'takeoff-skip' in line):
                if 'FindRunway=INVALID' in line:
                    takeoff_fails['FindRunway=INVALID'] += 1
                elif 'takeoff-path invalid' in line:
                    takeoff_fails['takeoff-path invalid'] += 1
                elif 'takeoff-skip' in line:
                    takeoff_fails['takeoff-skip'] += 1

    print(f"Total lines analyzed: {total_lines}\n")

    print("=== 1. INVARIANTS ===")
    if invariants:
        for k, v in invariants.items():
            print(f"  {k}: {v}")
    else:
        print("  None (0 invariant violations found)")
    print()

    print("=== 2. FALLBACKS ===")
    if fallbacks:
        for k, v in fallbacks.items():
            print(f"  {k}: {v}")
    else:
        print("  None (0 fallbacks triggered)")
    print()

    print("=== 3. STUCK TYPES ===")
    for k, v in stuck_by_type.items():
        print(f"  {k}: {v}")
    print()

    print("=== 4. STUCK(NO-PATH) TOP AIRCRAFT ===")
    if stuck_no_path:
        for k, v in sorted(stuck_no_path.items(), key=lambda x: x[1], reverse=True)[:15]:
            print(f"  {k}: {v} occurrences")
    else:
        print("  None")
    print()

    print("=== 5. STUCK(RESERVE) HIGHEST WAIT COUNTERS ===")
    high_waits = sorted(stuck_reserve.items(), key=lambda x: x[1]['max_wait'], reverse=True)
    if high_waits:
        for k, v in high_waits[:15]:
            print(f"  {k} -> max_wait={v['max_wait']}, total_reports={v['count']}")
            print(f"     Last report: {v['last_line']}")
            top_reason = v['reasons'].most_common(2)
            print(f"     Main reasons: {top_reason}")
    else:
        print("  None")
    print()

    print("=== 6. LANDING CHAIN FAILURES ===")
    if landing_fails:
        for k, v in landing_fails.most_common(10):
            print(f"  {k}: {v}")
    else:
        print("  None")
    print()

    print("=== 7. TAKEOFF ISSUES ===")
    if takeoff_fails:
        for k, v in takeoff_fails.items():
            print(f"  {k}: {v}")
    else:
        print("  None")
    print()

if __name__ == '__main__':
    analyze()
