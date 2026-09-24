// SPDX-License-Identifier: GPL-2.0

//! Configuration and whole-core allocation for LAVD's soft partitions.
//!
//! This module has no BPF or Linux dependencies so the allocation policy can be
//! tested independently of the scheduler.

use std::collections::HashSet;
use std::fs;
use std::path::Path;

use anyhow::Context;
use anyhow::Result;
use anyhow::bail;
use serde::Deserialize;

pub const MAX_PARTITIONS: usize = 16;
pub const MAX_PREFIXES: usize = 8;
pub const UNASSIGNED: u32 = u32::MAX;

#[derive(Clone, Debug, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Partition {
    pub name: String,
    #[serde(default)]
    pub comm_prefixes: Vec<String>,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Config {
    /// The default partition is always at index zero. Specific partitions keep
    /// their configuration order, which is also their matching precedence.
    pub partitions: Vec<Partition>,
    #[serde(default = "default_interval_ms")]
    pub interval_ms: u64,
}

fn default_interval_ms() -> u64 {
    1000
}

impl Config {
    pub fn load(path: &Path) -> Result<Self> {
        let contents = fs::read_to_string(path)
            .with_context(|| format!("reading partition config {}", path.display()))?;
        Self::parse(&contents)
            .with_context(|| format!("loading partition config {}", path.display()))
    }

    fn parse(contents: &str) -> Result<Self> {
        let mut config: Self =
            serde_json::from_str(contents).context("parsing partition config")?;
        if config.partitions.is_empty() {
            bail!("partition config must contain at least one partition");
        }
        if !(100..=60_000).contains(&config.interval_ms) {
            bail!("partition interval_ms must be between 100 and 60000");
        }

        let mut names = HashSet::new();
        let mut default = None;
        for (index, partition) in config.partitions.iter().enumerate() {
            if partition.name.trim().is_empty() {
                bail!("partition names must not be empty");
            }
            if !names.insert(partition.name.as_str()) {
                bail!("duplicate partition name {:?}", partition.name);
            }
            if partition.comm_prefixes.len() > MAX_PREFIXES {
                bail!(
                    "partition {:?} has more than {MAX_PREFIXES} comm prefixes",
                    partition.name
                );
            }
            if partition.comm_prefixes.is_empty() && default.replace(index).is_some() {
                bail!("partition config contains multiple default partitions");
            }
            for prefix in &partition.comm_prefixes {
                if prefix.is_empty()
                    || prefix.len() > 15
                    || !prefix.is_ascii()
                    || prefix.contains('\0')
                {
                    bail!(
                        "partition {:?} comm prefixes must contain 1 to 15 non-NUL ASCII bytes",
                        partition.name
                    );
                }
            }
        }

        if default.is_none() && names.contains("default") {
            bail!("the implicit default partition conflicts with partition name \"default\"");
        }
        let default = match default {
            Some(index) => config.partitions.remove(index),
            None => Partition {
                name: "default".to_owned(),
                comm_prefixes: Vec::new(),
            },
        };
        config.partitions.insert(0, default);
        if config.partitions.len() > MAX_PARTITIONS {
            bail!(
                "partition config has more than {MAX_PARTITIONS} partitions including the default"
            );
        }
        Ok(config)
    }
}

/// Assign each online physical core to a soft partition.
///
/// `cores` contains the online logical CPU IDs of each core, in topology order.
/// `demand` is indexed by partition ID. `previous` and the returned array are
/// indexed by logical CPU ID; CPUs absent from `cores` become `UNASSIGNED`.
///
/// Each partition receives one core when possible. Remaining cores are divided
/// by demand using largest remainders, with partition ID breaking ties. If all
/// demands are zero, allocation is equal. With fewer cores than partitions,
/// give one core to each of the highest-demand partitions; the scheduler must
/// still service partitions which have no owned core.
///
/// Retain existing whole-core assignments up to each partition's new quota.
/// Thus a quota change moves only cores that must move. When a partition must
/// donate cores, retain those with the most previously assigned logical CPUs.
/// New cores and released cores fill remaining quotas in topology order.
pub fn assign_cores(cores: &[Vec<usize>], demand: &[u64], previous: &[u32]) -> Result<Vec<u32>> {
    if demand.is_empty() || demand.len() > MAX_PARTITIONS {
        bail!("core allocation requires 1 to {MAX_PARTITIONS} partitions");
    }
    let mut seen = vec![false; previous.len()];
    for core in cores {
        if core.is_empty() {
            bail!("core allocation contains an empty core");
        }
        for &cpu in core {
            let Some(present) = seen.get_mut(cpu) else {
                bail!("CPU {cpu} exceeds the owner array size {}", previous.len());
            };
            if *present {
                bail!("CPU {cpu} occurs in more than one core slot");
            }
            *present = true;
        }
    }

    let mut remaining = core_targets(cores.len(), demand);
    let mut core_owners = vec![UNASSIGNED; cores.len()];
    let mut candidates = vec![Vec::new(); demand.len()];
    for (index, core) in cores.iter().enumerate() {
        // An online SMT sibling may have just appeared. Ignore unknown previous
        // ownership so it can join its existing sibling without moving a core.
        let owners: HashSet<_> = core
            .iter()
            .map(|&cpu| previous[cpu])
            .filter(|&owner| (owner as usize) < demand.len())
            .collect();
        if owners.len() == 1 {
            let owner = *owners.iter().next().unwrap() as usize;
            let retained = core
                .iter()
                .filter(|&&cpu| previous[cpu] as usize == owner)
                .count();
            candidates[owner].push((index, retained));
        }
    }
    for (owner, candidate_cores) in candidates.iter_mut().enumerate() {
        candidate_cores.sort_by_key(|&(index, retained)| (std::cmp::Reverse(retained), index));
        for &(index, _) in candidate_cores.iter().take(remaining[owner]) {
            core_owners[index] = owner as u32;
        }
        remaining[owner] = remaining[owner].saturating_sub(candidate_cores.len());
    }

    let mut next_owner = 0;
    for owner in &mut core_owners {
        if *owner != UNASSIGNED {
            continue;
        }
        while remaining[next_owner] == 0 {
            next_owner += 1;
        }
        *owner = next_owner as u32;
        remaining[next_owner] -= 1;
    }

    let mut owners = vec![UNASSIGNED; previous.len()];
    for (core, owner) in cores.iter().zip(core_owners) {
        for &cpu in core {
            owners[cpu] = owner;
        }
    }
    Ok(owners)
}

fn core_targets(nr_cores: usize, demand: &[u64]) -> Vec<usize> {
    let nr_partitions = demand.len();
    if nr_cores < nr_partitions {
        let mut order: Vec<_> = (0..nr_partitions).collect();
        order.sort_by_key(|&id| (std::cmp::Reverse(demand[id]), id));
        let mut targets = vec![0; nr_partitions];
        for id in order.into_iter().take(nr_cores) {
            targets[id] = 1;
        }
        return targets;
    }

    let mut targets = vec![1; nr_partitions];
    let distributable = nr_cores - nr_partitions;
    let total: u128 = demand.iter().map(|&value| u128::from(value)).sum();
    let denominator = if total == 0 {
        nr_partitions as u128
    } else {
        total
    };
    let mut remainders = Vec::with_capacity(nr_partitions);
    let mut assigned = 0;
    for (id, &weight) in demand.iter().enumerate() {
        let weight = if total == 0 { 1 } else { u128::from(weight) };
        let numerator = weight * distributable as u128;
        let count = (numerator / denominator) as usize;
        targets[id] += count;
        assigned += count;
        remainders.push((id, numerator % denominator));
    }
    remainders.sort_by_key(|&(id, remainder)| (std::cmp::Reverse(remainder), id));
    for &(id, _) in remainders.iter().take(distributable - assigned) {
        targets[id] += 1;
    }
    targets
}

#[cfg(test)]
mod tests {
    use super::*;

    fn config(partitions: serde_json::Value) -> String {
        serde_json::json!({ "partitions": partitions }).to_string()
    }

    #[test]
    fn default_is_zero_and_specific_precedence_is_preserved() {
        let parsed = Config::parse(&config(serde_json::json!([
            { "name": "render", "comm_prefixes": ["Render"] },
            { "name": "rest", "comm_prefixes": [] },
            { "name": "worker", "comm_prefixes": ["worker", "pool"] }
        ])))
        .unwrap();
        assert_eq!(parsed.interval_ms, 1000);
        assert_eq!(
            parsed
                .partitions
                .iter()
                .map(|p| p.name.as_str())
                .collect::<Vec<_>>(),
            ["rest", "render", "worker"]
        );
        assert_eq!(parsed.partitions[2].comm_prefixes, ["worker", "pool"]);
    }

    #[test]
    fn implicit_default_is_added() {
        let parsed = Config::parse(&config(serde_json::json!([
            { "name": "render", "comm_prefixes": ["Render"] }
        ])))
        .unwrap();
        assert_eq!(parsed.partitions.len(), 2);
        assert_eq!(parsed.partitions[0].name, "default");
        assert!(parsed.partitions[0].comm_prefixes.is_empty());
    }

    #[test]
    fn rejects_ambiguous_and_invalid_configs() {
        let invalid = [
            r#"{"partitions":[]}"#,
            r#"{"partitions":[{"name":""}]}"#,
            r#"{"partitions":[{"name":"  "}]}"#,
            r#"{"partitions":[{"name":"one"},{"name":"two"}]}"#,
            r#"{"partitions":[{"name":"one"},{"name":"one","comm_prefixes":["x"]}]}"#,
            r#"{"partitions":[{"name":"default","comm_prefixes":["x"]}]}"#,
            r#"{"partitions":[{"name":"x","comm_prefixes":[""]}]}"#,
            r#"{"partitions":[{"name":"x","comm_prefixes":["1234567890123456"]}]}"#,
            r#"{"partitions":[{"name":"x","comm_prefixes":["é"]}]}"#,
            r#"{"partitions":[{"name":"x","comm_prefixes":["a\u0000b"]}]}"#,
            r#"{"partitions":[{"name":"x","comm_prefix":["x"]}]}"#,
            r#"{"partitions":[{"name":"x"}],"interval":1000}"#,
            r#"{"partitions":[{"name":"x"}],"interval_ms":99}"#,
            r#"{"partitions":[{"name":"x"}],"interval_ms":60001}"#,
        ];
        for input in invalid {
            assert!(Config::parse(input).is_err(), "accepted {input}");
        }
    }

    #[test]
    fn validates_bounds_including_implicit_default() {
        let partitions: Vec<_> = (0..MAX_PARTITIONS)
            .map(|id| serde_json::json!({"name": format!("p{id}"), "comm_prefixes": ["x"]}))
            .collect();
        assert!(
            Config::parse(&config(serde_json::json!(
                &partitions[..MAX_PARTITIONS - 1]
            )))
            .is_ok()
        );
        assert!(Config::parse(&config(serde_json::json!(partitions))).is_err());
        for count in [MAX_PREFIXES, MAX_PREFIXES + 1] {
            let input = config(serde_json::json!([
                {"name": "p", "comm_prefixes": vec!["123456789012345"; count]}
            ]));
            assert_eq!(Config::parse(&input).is_ok(), count == MAX_PREFIXES);
        }
        for interval in [100, 60_000] {
            assert!(
                Config::parse(&format!(
                    r#"{{"partitions":[{{"name":"rest"}}],"interval_ms":{interval}}}"#
                ))
                .is_ok()
            );
        }
    }

    fn cores(count: usize) -> Vec<Vec<usize>> {
        (0..count).map(|cpu| vec![cpu * 2, cpu * 2 + 1]).collect()
    }

    fn counts(cores: &[Vec<usize>], owners: &[u32], nr_partitions: usize) -> Vec<usize> {
        let mut counts = vec![0; nr_partitions];
        for core in cores {
            let owner = owners[core[0]];
            assert!(
                core.iter().all(|&cpu| owners[cpu] == owner),
                "split SMT core"
            );
            counts[owner as usize] += 1;
        }
        counts
    }

    #[test]
    fn weights_remainder_after_protected_floors() {
        let cores = cores(10);
        let owners = assign_cores(&cores, &[0, 3, 1], &[UNASSIGNED; 20]).unwrap();
        assert_eq!(counts(&cores, &owners, 3), [1, 6, 3]);
        assert_eq!(core_targets(8, &[0, 0, 0]), [3, 3, 2]);
        assert_eq!(core_targets(10, &[u64::MAX, u64::MAX]), [5, 5]);
    }

    #[test]
    fn repeated_allocation_is_stable_and_moves_only_the_quota_deficit() {
        let cores = cores(10);
        let owners = assign_cores(&cores, &[1, 1], &[UNASSIGNED; 20]).unwrap();
        assert_eq!(assign_cores(&cores, &[1, 1], &owners).unwrap(), owners);
        let shifted = assign_cores(&cores, &[3, 1], &owners).unwrap();
        assert_eq!(counts(&cores, &shifted, 2), [7, 3]);
        let moved = cores
            .iter()
            .filter(|core| owners[core[0]] != shifted[core[0]])
            .count();
        assert_eq!(moved, 2);
    }

    #[test]
    fn sparse_cpu_ids_and_hotplug_keep_siblings_together() {
        let initial = vec![vec![1, 9], vec![3, 11], vec![5, 13]];
        let owners = assign_cores(&initial, &[1, 1], &[UNASSIGNED; 16]).unwrap();
        assert_eq!(counts(&initial, &owners, 2), [2, 1]);
        assert_eq!(owners[0], UNASSIGNED);
        let online = vec![vec![1, 9], vec![5, 13]];
        let reduced = assign_cores(&online, &[1, 1], &owners).unwrap();
        assert_eq!(counts(&online, &reduced, 2), [1, 1]);
        assert_eq!((reduced[3], reduced[11]), (UNASSIGNED, UNASSIGNED));
        assert_eq!(reduced[1], owners[1]);
        assert_eq!(reduced[5], owners[5]);
        let new_sibling = vec![vec![1, 9, 15], vec![5, 13]];
        let expanded = assign_cores(&new_sibling, &[1, 1], &reduced).unwrap();
        assert_eq!(expanded[15], reduced[1]);
        assert_eq!(counts(&new_sibling, &expanded, 2), [1, 1]);
    }

    #[test]
    fn too_few_cores_have_deterministic_owners() {
        let cores = cores(2);
        let owners = assign_cores(&cores, &[0, 9, 3, 3], &[UNASSIGNED; 4]).unwrap();
        assert_eq!(counts(&cores, &owners, 4), [0, 1, 1, 0]);
        assert_eq!(core_targets(2, &[0, 0, 0]), [1, 1, 0]);
        assert_eq!(
            assign_cores(&[], &[1, 1], &[0, 1]).unwrap(),
            [UNASSIGNED; 2]
        );
    }

    #[test]
    fn rejects_invalid_topologies() {
        assert!(assign_cores(&[vec![]], &[1], &[0]).is_err());
        assert!(assign_cores(&[vec![0], vec![0]], &[1], &[0]).is_err());
        assert!(assign_cores(&[vec![0, 0]], &[1], &[0]).is_err());
        assert!(assign_cores(&[vec![2]], &[1], &[0]).is_err());
        assert!(assign_cores(&[vec![0]], &[], &[0]).is_err());
        assert!(assign_cores(&[vec![0]], &[1; MAX_PARTITIONS + 1], &[0]).is_err());
    }

    #[test]
    fn allocations_conserve_capacity_and_preserve_floors() {
        // Cover skew, zero demand, and changing quotas across many small layouts.
        for count in 0..32 {
            let topology = cores(count);
            for groups in 1..=MAX_PARTITIONS {
                let mut previous = vec![UNASSIGNED; count * 2];
                for step in 0..8 {
                    let demand: Vec<_> = (0..groups)
                        .map(|id| ((id * 13 + step * 7) % 19) as u64)
                        .collect();
                    let owners = assign_cores(&topology, &demand, &previous).unwrap();
                    let assigned = counts(&topology, &owners, groups);
                    assert_eq!(assigned.iter().sum::<usize>(), count);
                    if count >= groups {
                        assert!(assigned.iter().all(|&value| value >= 1));
                    }
                    assert_eq!(assign_cores(&topology, &demand, &owners).unwrap(), owners);
                    previous = owners;
                }
            }
        }
    }
}
