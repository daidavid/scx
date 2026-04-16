// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This software may be used and distributed according to the terms of the
// GNU General Public License version 2.

use std::env;
use std::fs::File;
use std::io::{Read, Seek};
use std::path::PathBuf;
use std::process::Command;

use scx_cargo::ClangInfo;

fn emit_git_build_id() {
    let manifest_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
    let repo_root = Command::new("git")
        .args(["-C", &manifest_dir, "rev-parse", "--show-toplevel"])
        .output()
        .ok()
        .filter(|out| out.status.success())
        .map(|out| String::from_utf8_lossy(&out.stdout).trim().to_string());

    let Some(repo_root) = repo_root else {
        return;
    };

    let git_dir = Command::new("git")
        .args(["-C", &repo_root, "rev-parse", "--git-dir"])
        .output()
        .ok()
        .filter(|out| out.status.success())
        .map(|out| String::from_utf8_lossy(&out.stdout).trim().to_string());

    if let Some(git_dir) = git_dir {
        let git_dir = if PathBuf::from(&git_dir).is_absolute() {
            PathBuf::from(git_dir)
        } else {
            PathBuf::from(&repo_root).join(git_dir)
        };
        println!("cargo:rerun-if-changed={}", git_dir.join("HEAD").display());
        println!("cargo:rerun-if-changed={}", git_dir.join("index").display());
    }

    let sha = Command::new("git")
        .args(["-C", &repo_root, "rev-parse", "--short", "HEAD"])
        .output()
        .ok()
        .filter(|out| out.status.success())
        .map(|out| String::from_utf8_lossy(&out.stdout).trim().to_string());
    if let Some(sha) = sha {
        println!("cargo:rustc-env=SCX_GIT_SHA={sha}");
    }

    let dirty = Command::new("git")
        .args(["-C", &repo_root, "status", "--porcelain"])
        .output()
        .ok()
        .filter(|out| out.status.success())
        .map(|out| !out.stdout.is_empty())
        .unwrap_or(false);
    if dirty {
        println!("cargo:rustc-env=SCX_GIT_DIRTY=1");
    }
}

fn gen_bindings() {
    let out_dir = env::var("OUT_DIR").unwrap();
    let clang = ClangInfo::new().unwrap();
    let kernel_target = clang.kernel_target().unwrap();

    let mut vmlinux_tar_zst = File::open("vmlinux.tar.zst").unwrap();

    let mut vmlinux_h = String::new();

    // vmlinux.h is a symlink. dereference it here.
    let search: PathBuf = format!("vmlinux/arch/{kernel_target}/vmlinux.h").into();

    let mut vmlinux_tar = ruzstd::decoding::StreamingDecoder::new(&mut vmlinux_tar_zst).unwrap();
    let mut archive = tar::Archive::new(&mut vmlinux_tar);
    let vmlinux_link_entry = archive
        .entries()
        .unwrap()
        .find(|x| x.as_ref().unwrap().path().unwrap() == search.as_path())
        .unwrap()
        .unwrap();

    let vmlinux_path = PathBuf::from(vmlinux_link_entry.path().unwrap())
        .parent()
        .unwrap()
        .join(vmlinux_link_entry.link_name().unwrap().unwrap());

    vmlinux_tar_zst.rewind().unwrap();
    let vmlinux_tar = ruzstd::decoding::StreamingDecoder::new(&mut vmlinux_tar_zst).unwrap();

    tar::Archive::new(vmlinux_tar)
        .entries()
        .unwrap()
        .find(|x| x.as_ref().unwrap().path().unwrap() == vmlinux_path.as_path())
        .unwrap()
        .unwrap()
        .read_to_string(&mut vmlinux_h)
        .unwrap();

    let bindings = bindgen::Builder::default()
        .header_contents(&search.to_string_lossy(), &vmlinux_h)
        .allowlist_type("scx_exit_kind")
        .allowlist_type("scx_consts")
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .generate()
        .expect("Unable to generate bindings");

    bindings
        .write_to_file(PathBuf::from(&out_dir).join("bindings.rs"))
        .expect("Couldn't write bindings");
}

fn main() {
    gen_bindings();
    emit_git_build_id();

    // Emit the target triple so build_id.rs can read it without vergen.
    // This is stable across commits so it won't invalidate the cache.
    println!(
        "cargo:rustc-env=SCX_TARGET_TRIPLE={}",
        env::var("TARGET").unwrap()
    );

    let bindings = bindgen::Builder::default()
        .header("perf_wrapper.h")
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .prepend_enum_name(false)
        .derive_default(true)
        .generate()
        .expect("Unable to generate bindings");

    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("perf_bindings.rs"))
        .expect("Couldn't write bindings!");
}
