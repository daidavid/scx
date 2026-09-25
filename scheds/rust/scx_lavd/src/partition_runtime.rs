// SPDX-License-Identifier: GPL-2.0

//! Userspace sampling and ownership planning for soft partitions.

use std::collections::BTreeMap;
use std::fs;
use std::time::Duration;
use std::time::Instant;

use anyhow::Context;
use anyhow::Result;
use anyhow::ensure;
use scx_utils::ROOT_PREFIX;
use scx_utils::read_cpulist;
use tracing::info;

use crate::cpu_order::CpuOrder;
use crate::cpu_order::{ComputeDomain, ComputeDomainId, CpuId};
use crate::partition::Config;
use crate::partition::Granularity;
use crate::partition::HomeUnit;
use crate::partition::UNASSIGNED;
use crate::partition::assign_grants;
use crate::partition::assign_homes;

const DEMAND_SCALE: f64 = 1024.0;

pub struct PartitionRuntime {
    pub config: Config,
    pub owners: Vec<u32>,
    pub cpdom_owners: Vec<u32>,
    units: Vec<HomeUnit>,
    homes: Vec<u32>,
    demand: Vec<u64>,
    last_counters: Vec<u64>,
    sampled_at: Instant,
    next_sample: Instant,
}

impl PartitionRuntime {
    pub fn new(config: Config, order: &CpuOrder, nr_cpu_ids: usize) -> Result<Self> {
        let units = home_units(order)?;
        let homes = assign_homes(&units, config.partitions.len())?;
        let mut cpdom_owners = vec![UNASSIGNED; order.nr_cpdoms];
        for (unit, &owner) in units.iter().zip(&homes) {
            for &domain in &unit.cpdom_ids {
                ensure!(
                    domain < cpdom_owners.len(),
                    "invalid compute domain {domain}"
                );
                cpdom_owners[domain] = owner;
            }
        }
        let demand = vec![0; config.partitions.len()];
        let online = online_cpus(nr_cpu_ids)?;
        let owners = assign_grants(
            &units,
            &homes,
            &demand,
            &vec![UNASSIGNED; nr_cpu_ids],
            &online,
            config.granularity,
        )?;
        let sampled_at = Instant::now();
        let next_sample = sampled_at + Duration::from_millis(config.interval_ms);
        Ok(Self {
            config,
            owners,
            cpdom_owners,
            units,
            homes,
            last_counters: vec![0; demand.len()],
            demand,
            sampled_at,
            next_sample,
        })
    }

    /// Start sampling after attachment, excluding time spent loading BPF.
    pub fn start(&mut self, counters: &[u64], now: Instant) {
        let count = self.last_counters.len();
        self.last_counters.copy_from_slice(&counters[..count]);
        self.sampled_at = now;
        self.next_sample = now + Duration::from_millis(self.config.interval_ms);
        self.log_homes();
        self.log_assignments();
    }

    pub fn until_update(&self, now: Instant) -> Duration {
        self.next_sample.saturating_duration_since(now)
    }

    /// Sample once per interval and return changed ownership for BPF to apply.
    /// Retain the old owners until the caller confirms successful publication.
    pub fn update(&mut self, counters: &[u64], now: Instant) -> Result<Option<Vec<u32>>> {
        if now < self.next_sample {
            return Ok(None);
        }

        let elapsed = now.duration_since(self.sampled_at);
        for ((demand, previous), &counter) in self
            .demand
            .iter_mut()
            .zip(&mut self.last_counters)
            .zip(counters)
        {
            *demand = smooth_demand(*demand, counter.wrapping_sub(*previous), elapsed);
            *previous = counter;
        }
        self.sampled_at = now;
        self.next_sample = now + Duration::from_millis(self.config.interval_ms);

        // Ignore newly discovered topology: LAVD restarts when a CPU that was
        // absent at initialization comes online and needs topology discovery.
        let online = online_cpus(self.owners.len())?;
        let owners = assign_grants(
            &self.units,
            &self.homes,
            &self.demand,
            &self.owners,
            &online,
            self.config.granularity,
        )?;
        Ok((owners != self.owners).then_some(owners))
    }

    /// BPF core mode reads a single owner word for both SMT siblings. Keep that
    /// word populated when the primary sibling is offline; the public owner
    /// array and preference masks still contain online CPUs only.
    pub fn staged_owners(&self, owners: &[u32]) -> Vec<u32> {
        stage_owners(&self.units, owners, self.config.granularity)
    }

    pub fn applied(&mut self, owners: Vec<u32>) {
        self.owners = owners;
        self.log_assignments();
    }

    fn log_assignments(&self) {
        for (id, partition) in self.config.partitions.iter().enumerate() {
            let cpus: Vec<_> = self
                .owners
                .iter()
                .enumerate()
                .filter_map(|(cpu, &owner)| (owner == id as u32).then_some(cpu))
                .collect();
            info!(
                "Soft partition {:?}: CPUs {:?}, demand {:.2} CPUs, granularity {}",
                partition.name,
                cpus,
                self.demand[id] as f64 / DEMAND_SCALE,
                self.config.granularity.as_str(),
            );
        }
    }

    fn log_homes(&self) {
        for (id, partition) in self.config.partitions.iter().enumerate() {
            let units: Vec<_> = self
                .units
                .iter()
                .zip(&self.homes)
                .filter_map(|(unit, &owner)| (owner == id as u32).then_some(unit))
                .collect();
            let domains: Vec<_> = units.iter().flat_map(|unit| &unit.cpdom_ids).collect();
            let cpus: Vec<_> = units
                .iter()
                .flat_map(|unit| unit.cores.iter().flatten())
                .collect();
            let llcs: Vec<_> = units
                .iter()
                .map(|unit| (unit.numa_id, unit.llc_id))
                .collect();
            info!(
                "Soft partition {:?}: home compute domains {:?}, home CPUs {:?}, topology LLCs {:?}",
                partition.name, domains, cpus, llcs,
            );
        }
    }
}

fn home_units(order: &CpuOrder) -> Result<Vec<HomeUnit>> {
    build_home_units(&order.cpuids, &order.cpdom_map)
}

fn build_home_units(
    cpuids: &[CpuId],
    cpdom_map: &BTreeMap<ComputeDomainId, ComputeDomain>,
) -> Result<Vec<HomeUnit>> {
    let mut units = BTreeMap::new();
    let mut cores = BTreeMap::<_, BTreeMap<usize, Vec<usize>>>::new();
    for (key, domain) in cpdom_map {
        let unit = units
            .entry((key.numa_adx, key.llc_adx))
            .or_insert_with(|| HomeUnit {
                numa_id: key.numa_adx,
                llc_id: key.llc_adx,
                physical_llc_id: key.llc_kernel_id,
                cpdom_ids: Vec::new(),
                cores: Vec::new(),
            });
        ensure!(
            unit.physical_llc_id == key.llc_kernel_id,
            "one topology LLC spans physical LLCs"
        );
        unit.cpdom_ids.push(domain.cpdom_id);
    }
    for cpu in cpuids {
        cores
            .entry((cpu.numa_adx, cpu.llc_adx))
            .or_default()
            .entry(cpu.core_rdx)
            .or_default()
            .push(cpu.cpu_adx);
    }
    for (key, mut unit_cores) in cores {
        let unit = units.get_mut(&key).context("CPU has no compute domain")?;
        for core in unit_cores.values_mut() {
            core.sort_unstable();
        }
        unit.cores = unit_cores.into_values().collect();
        unit.cpdom_ids.sort_unstable();
    }
    Ok(units.into_values().collect())
}

fn stage_owners(units: &[HomeUnit], owners: &[u32], granularity: Granularity) -> Vec<u32> {
    let mut staged = owners.to_vec();
    if granularity == Granularity::Core {
        for core in units.iter().flat_map(|unit| &unit.cores) {
            if let (Some(&primary), Some(owner)) = (
                core.iter().min(),
                core.iter()
                    .map(|&cpu| owners[cpu])
                    .find(|&owner| owner != UNASSIGNED),
            ) {
                staged[primary] = owner;
            }
        }
    }
    staged
}

fn online_cpus(nr_cpu_ids: usize) -> Result<Vec<bool>> {
    let path = format!("{}/sys/devices/system/cpu/online", *ROOT_PREFIX);
    let cpulist = fs::read_to_string(&path).with_context(|| format!("reading {path}"))?;
    let mut online = vec![false; nr_cpu_ids];
    for cpu in read_cpulist(&cpulist).context("parsing online CPU list for soft partitions")? {
        if let Some(entry) = online.get_mut(cpu) {
            *entry = true;
        }
    }
    Ok(online)
}

fn smooth_demand(previous: u64, delta: u64, elapsed: Duration) -> u64 {
    // BPF integrates a Q10 demand over nanoseconds. Counter subtraction uses
    // wrapping arithmetic; division restores Q10 CPU units. Widen the EWMA
    // product so an extreme sample cannot overflow userspace arithmetic.
    let sample = u128::from(delta) / elapsed.as_nanos().max(1);
    ((3 * u128::from(previous) + sample) / 4) as u64
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::{Cell, RefCell};

    #[test]
    fn mixed_core_types_share_one_topology_home() {
        let mut domains = BTreeMap::new();
        let mut cpus = Vec::new();
        // Two compute domains in the first physical LLC, then a vLLC in
        // the same physical cache. Neither core type nor a DSQ is a home unit.
        for (id, llc, big) in [(0, 0, true), (1, 0, false), (2, 1, true)] {
            domains.insert(
                ComputeDomainId {
                    numa_adx: 0,
                    llc_adx: llc,
                    llc_rdx: llc,
                    llc_kernel_id: 10,
                    is_big: big,
                },
                ComputeDomain {
                    cpdom_id: id,
                    cpdom_alt_id: Cell::new(id),
                    cpu_ids: vec![id],
                    neighbor_map: RefCell::new(BTreeMap::new()),
                },
            );
            cpus.push(CpuId {
                numa_adx: 0,
                pd_adx: 0,
                llc_adx: llc,
                llc_rdx: llc,
                llc_kernel_id: 10,
                core_rdx: id,
                cpu_rdx: 0,
                cpu_adx: id,
                smt_level: 0,
                cache_size: 0,
                cpu_cap: 1024,
                big_core: big,
                turbo_core: false,
                cpu_sibling: id,
            });
        }
        let units = build_home_units(&cpus, &domains).unwrap();
        assert_eq!(units.len(), 2);
        assert_eq!(units[0].cpdom_ids, [0, 1]);
        assert_eq!(units[0].cores, [vec![0], vec![1]]);
        assert_eq!(units[1].cpdom_ids, [2]);
        assert_eq!(units[0].physical_llc_id, units[1].physical_llc_id);
        assert_eq!(assign_homes(&units, 2).unwrap(), [0, 1]);
        assert!(assign_homes(&units, 3).is_err());
    }

    #[test]
    fn offline_primary_keeps_core_owner_word_but_cpu_mode_does_not() {
        let units = vec![HomeUnit {
            numa_id: 0,
            llc_id: 0,
            physical_llc_id: 0,
            cpdom_ids: vec![0],
            cores: vec![vec![1, 5], vec![3, 7]],
        }];
        let mut owners = vec![UNASSIGNED; 8];
        owners[5] = 2;
        let staged = stage_owners(&units, &owners, Granularity::Core);
        assert_eq!(staged[1], 2);
        assert_eq!(staged[5], 2);
        assert_eq!(staged[3], UNASSIGNED);
        assert_eq!(owners[1], UNASSIGNED);
        assert_eq!(stage_owners(&units, &owners, Granularity::Cpu), owners);
    }

    #[test]
    fn sampling_uses_elapsed_time_and_counter_wrap() {
        let previous = u64::MAX - 1024;
        let current: u64 = 1023;
        let delta = current.wrapping_sub(previous);
        assert_eq!(delta, 2048);
        assert_eq!(smooth_demand(1024, delta, Duration::from_nanos(2)), 1024);
        assert_eq!(smooth_demand(0, delta, Duration::from_nanos(2)), 256);
        assert_eq!(smooth_demand(0, delta, Duration::from_nanos(4)), 128);
    }

    #[test]
    fn idle_demand_decays_to_zero_without_overflow() {
        let mut demand = 1024;
        for _ in 0..32 {
            demand = smooth_demand(demand, 0, Duration::from_secs(1));
        }
        assert_eq!(demand, 0);
        assert_eq!(
            smooth_demand(u64::MAX, u64::MAX, Duration::from_nanos(1)),
            u64::MAX
        );
    }
}
