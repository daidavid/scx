#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Bounded functional checks for LAVD soft partitions on a private Linux host.

Run as root with no scheduler attached:
  sudo python3 tests/soft_partition.py --binary /path/to/scx_lavd

Workers alone enroll in SCHED_EXT. The test creates its own configuration and
retains logs/results in a new temporary directory (or --output). It does not
change CPU online state, frequency policy, cgroups, or host affinity. It checks
progress and placement, not throughput or latency guarantees. A full online
CPU affinity and at least three online physical cores are required. To bound
test load, hosts above 32 online logical CPUs are skipped. The zero-owner test
is skipped above 14 cores because configuration supports at most 16 partitions.
"""
import argparse
import ctypes
import json
import multiprocessing as mp
import os
import re
from pathlib import Path
import signal
import subprocess
import time
import tempfile

CTX = mp.get_context('fork')
STATE = Path('/sys/kernel/sched_ext/state')
LIBC = ctypes.CDLL(None, use_errno=True)


def name(value):
    assert LIBC.prctl(15, value.encode(), 0, 0, 0) == 0


def worker(conn, initial_name, cpus, rename=False):
    try:
        os.sched_setaffinity(0, cpus)
        name(initial_name)
        os.sched_setscheduler(0, 7, os.sched_param(0))
        assert os.sched_getscheduler(0) == 7
        conn.send({'ready': True, 'pid': os.getpid()})
        seconds = conn.recv()
        start = time.monotonic()
        start_cpu = time.process_time()
        seen = set()
        rounds = 0
        renamed = False
        while time.monotonic() - start < seconds:
            sum(i*i for i in range(1500))
            seen.add(LIBC.sched_getcpu())
            rounds += 1
            if rename and not renamed and time.monotonic() - start > seconds/2:
                name('lavd-beta')
                renamed = True
        conn.send({'ok': True, 'name': initial_name, 'renamed': renamed,
                   'cpus': sorted(seen), 'rounds': rounds,
                   'cpu_seconds': time.process_time()-start_cpu,
                   'affinity': sorted(os.sched_getaffinity(0))})
    except BaseException as exc:
        conn.send({'ok': False, 'error': repr(exc)})
        raise
    finally:
        conn.close()


def run_group(specs, seconds):
    children = []
    try:
        for task_name, cpus, rename in specs:
            conn, child_conn = CTX.Pipe()
            child = CTX.Process(target=worker, args=(child_conn, task_name, cpus, rename))
            child.start()
            child_conn.close()
            children.append((child, conn))
        for child, conn in children:
            assert conn.poll(12), 'worker failed to enroll'
            assert conn.recv().get('ready'), 'worker enrollment error'
        for child, conn in children:
            conn.send(seconds)
        results = []
        deadline = time.monotonic() + seconds + 12
        for child, conn in children:
            assert conn.poll(max(0, deadline-time.monotonic())), 'worker starved'
            result = conn.recv()
            assert result.get('ok'), result
            assert result['rounds'] > 0 and result['cpu_seconds'] > 0, result
            assert set(result['cpus']) <= set(result['affinity']), result
            results.append(result)
            child.join(2)
            assert child.exitcode == 0
        return results
    finally:
        for child, conn in children:
            if child.is_alive():
                child.kill()
                child.join(2)
            conn.close()


def session(binary, config, output, cases):
    assert STATE.read_text().strip() == 'disabled'
    output.mkdir(parents=True, exist_ok=False)
    log = (output/'scheduler.log').open('w')
    scheduler = subprocess.Popen([binary, '--partial', '--performance', '--no-freq-scaling',
                                  '--warm-cpu-us=0', '--partition-config', str(config)],
                                 stdout=log, stderr=subprocess.STDOUT)
    results = {'cases': {}}
    try:
        deadline = time.monotonic()+20
        while STATE.read_text().strip() != 'enabled':
            assert scheduler.poll() is None, 'attach failed; see scheduler.log'
            assert time.monotonic()<deadline, 'attach timed out'
            time.sleep(0.05)
        for case, specs, seconds in cases:
            print('RUN',case,flush=True)
            results['cases'][case] = run_group(specs, seconds)
            assert scheduler.poll() is None and STATE.read_text().strip()=='enabled'
        results['result'] = 'PASS'
    finally:
        if scheduler.poll() is None:
            scheduler.send_signal(signal.SIGINT)
            try:
                scheduler.wait(15)
            except subprocess.TimeoutExpired:
                scheduler.kill()
                scheduler.wait(5)
        log.close()
        results['scheduler_exit'] = scheduler.returncode
        results['final_sched_ext_state'] = STATE.read_text().strip()
        (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    assert scheduler.returncode == 0
    assert results['final_sched_ext_state']=='disabled'
    return results


def cpulist(value):
    cpus = set()
    for item in value.strip().split(','):
        bounds = [int(v) for v in item.split('-')]
        cpus.update(range(bounds[0], bounds[-1] + 1))
    return cpus


def topology():
    cpus = cpulist(Path('/sys/devices/system/cpu/online').read_text())
    assert set(os.sched_getaffinity(0)) == cpus, 'test requires full online CPU affinity'
    cores = set()
    for cpu in cpus:
        siblings = Path(f'/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list')
        cores.add(frozenset(cpulist(siblings.read_text()) & cpus))
    return cpus, cores


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--binary', required=True)
    parser.add_argument('--output', type=Path, help='new directory for retained logs/results')
    args=parser.parse_args()
    assert os.geteuid() == 0, 'requires root inside a private Linux test host'
    assert STATE.read_text().strip() == 'disabled', 'another scheduler is attached'
    args.binary = str(Path(args.binary).resolve(strict=True))
    cpus, cores = topology()
    if len(cores) < 3 or len(cpus) > 32:
        print('SKIP: requires at least 3 physical cores and at most 32 logical CPUs')
        return
    if args.output:
        args.output.mkdir(parents=True, exist_ok=False)
    else:
        args.output = Path(tempfile.mkdtemp(prefix='lavd-soft-partition-'))
    print('Results:', args.output, flush=True)
    (args.output/'environment.json').write_text(json.dumps({
        'uname': list(os.uname()), 'online_cpus': sorted(cpus),
        'online_cores': [sorted(core) for core in sorted(cores, key=min)],
        'binary': args.binary,
    }, indent=2)+'\n')
    config=args.output/'three.json'
    config.write_text(json.dumps({'interval_ms':200,'partitions':[
        {'name':'default'}, {'name':'alpha','comm_prefixes':['lavd-alpha']},
        {'name':'beta','comm_prefixes':['lavd-beta']}]}))
    cpu0={min(cpus)}
    cases=[
        ('one_busy_partition_borrows', [('lavd-alpha',cpus,False)]*(len(cpus)+2), 3),
        ('other_partition_returns', [('lavd-alpha',cpus,False)]*4+[('lavd-beta',cpus,False)]*4, 3),
        ('rename_reclassifies', [('lavd-alpha',cpus,True)]*4, 3),
        ('affinity_bypasses_ownership', [('lavd-alpha',cpu0,False)]*2+[('lavd-beta',cpus,False)]*4, 2),
    ]
    solo=session(args.binary,config,args.output/'solo-rename',[
        ('solo_cpu_bound_rename', [('lavd-alpha',cpus,True)], 8)])
    owner_logs=(args.output/'solo-rename'/'scheduler.log').read_text()
    # Three cores leave no reallocatable capacity beyond the three floors.
    for part in (['alpha', 'beta'] if len(cores) > 3 else []):
        allocations=re.findall(r'Soft partition "'+part+r'": CPUs \[([^\]]*)\]', owner_logs)
        owner_sets=[{int(cpu) for cpu in value.split(',') if cpu.strip()} for value in allocations]
        counts=[sum(core <= owners for core in cores) for owners in owner_sets]
        assert max(counts,default=0)==len(cores)-2, (part,'solo demand did not claim available cores',counts)
    first=session(args.binary,config,args.output/'three-partitions',cases)
    borrowed=set().union(*(set(item['cpus']) for item in first['cases']['one_busy_partition_borrows']))
    # Each of the other two partitions retains one physical core, even
    # after demand-based rebalancing. Exceeding this bound proves borrowing.
    protected = sum(sorted(len(core) for core in cores)[:2])
    assert len(borrowed)>len(cpus)-protected, ('no evidence of borrowed capacity',borrowed)
    summary = {'result': 'PASS', 'borrowed_cpu_union': sorted(borrowed)}
    if len(cores) == 3:
        summary['rename_allocation'] = 'SKIP: three cores leave no spare ownership to move'
    if len(cores) <= 14:
        rescue = args.output/'rescue.json'
        parts = [{'name': 'default'}] + [
            {'name': f'group{i}', 'comm_prefixes': [f'lavd-p{i:02d}']}
            for i in range(len(cores)+1)]
        rescue.write_text(json.dumps({'interval_ms': 200, 'partitions': parts}))
        specs = [(f'lavd-p{i:02d}', cpus, False) for i in range(len(cores)+1)]*2
        specs += [('lavd-default', cpus, False)]*2
        second = session(args.binary, rescue, args.output/'more-partitions-than-cores', [
            ('all_partitions_progress', specs, 4)])
        summary['rescue_workers'] = len(second['cases']['all_partitions_progress'])
    else:
        summary['rescue'] = 'SKIP: more than 14 physical cores'
    (args.output/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    print(json.dumps(summary, indent=2))


if __name__=='__main__':
    main()
