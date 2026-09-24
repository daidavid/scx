// SPDX-License-Identifier: GPL-2.0

//! Userspace sampling and ownership planning for soft partitions.

use std::collections::BTreeMap;
use std::collections::HashSet;
use std::fs;
use std::time::Duration;
use std::time::Instant;

use anyhow::Context;
use anyhow::Result;
use scx_utils::ROOT_PREFIX;
use scx_utils::read_cpulist;
use tracing::info;

use crate::cpu_order::CpuOrder;
use crate::partition::Config;
use crate::partition::UNASSIGNED;
use crate::partition::assign_cores;

const DEMAND_SCALE: f64 = 1024.0;

pub struct PartitionRuntime {
    pub config: Config,
    pub owners: Vec<u32>,
    known_cores: Vec<Vec<usize>>,
    demand: Vec<u64>,
    last_counters: Vec<u64>,
    sampled_at: Instant,
    next_sample: Instant,
}

impl PartitionRuntime {
    pub fn new(config: Config, order: &CpuOrder, nr_cpu_ids: usize) -> Result<Self> {
        let mut cores = BTreeMap::<_, Vec<_>>::new();
        for cpu in &order.cpuids {
            cores
                .entry((cpu.numa_adx, cpu.llc_adx, cpu.core_rdx))
                .or_default()
                .push(cpu.cpu_adx);
        }
        let known_cores: Vec<_> = cores.into_values().collect();
        let demand = vec![0; config.partitions.len()];
        let online = online_cores(&known_cores)?;
        let owners = assign_cores(&online, &demand, &vec![UNASSIGNED; nr_cpu_ids])?;
        let sampled_at = Instant::now();
        let next_sample = sampled_at + Duration::from_millis(config.interval_ms);
        Ok(Self {
            config,
            owners,
            known_cores,
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
        let cores = online_cores(&self.known_cores)?;
        let owners = assign_cores(&cores, &self.demand, &self.owners)?;
        Ok((owners != self.owners).then_some(owners))
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
                "Soft partition {:?}: CPUs {:?}, demand {:.2} CPUs",
                partition.name,
                cpus,
                self.demand[id] as f64 / DEMAND_SCALE,
            );
        }
    }
}

fn online_cores(known_cores: &[Vec<usize>]) -> Result<Vec<Vec<usize>>> {
    let path = format!("{}/sys/devices/system/cpu/online", *ROOT_PREFIX);
    let cpulist = fs::read_to_string(&path).with_context(|| format!("reading {path}"))?;
    let online: HashSet<_> = read_cpulist(&cpulist)
        .context("parsing online CPU list for soft partitions")?
        .into_iter()
        .collect();
    Ok(known_cores
        .iter()
        .map(|core| {
            core.iter()
                .copied()
                .filter(|cpu| online.contains(cpu))
                .collect::<Vec<_>>()
        })
        .filter(|core| !core.is_empty())
        .collect())
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
