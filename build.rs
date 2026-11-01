use std::{
    env,
    fs,
    path::PathBuf,
    process::Command,
};

fn cmake_executable() -> PathBuf {
    let executable_name = if cfg!(windows) { "cmake.exe" } else { "cmake" };
    if let Some(path) = env::var_os("CMAKE_COMMAND").map(PathBuf::from) {
        if path.is_file() {
            return path;
        }
    }

    if let Some(paths) = env::var_os("PATH") {
        for directory in env::split_paths(&paths) {
            let candidate = directory.join(executable_name);
            if candidate.is_file() {
                return candidate;
            }
        }
    }

    if cfg!(windows) {
        let mut visual_studio_roots = Vec::new();
        if let Some(path) = env::var_os("VSINSTALLDIR").map(PathBuf::from) {
            visual_studio_roots.push(path);
        }
        for variable in ["ProgramFiles(x86)", "ProgramFiles"] {
            if let Some(path) = env::var_os(variable).map(PathBuf::from) {
                visual_studio_roots.push(path.join("Microsoft Visual Studio"));
            }
        }

        for root in visual_studio_roots {
            let installations = fs::read_dir(&root)
                .into_iter()
                .flatten()
                .flatten()
                .map(|entry| entry.path())
                .collect::<Vec<_>>();
            for installation in installations {
                let editions = fs::read_dir(&installation)
                    .into_iter()
                    .flatten()
                    .flatten()
                    .map(|entry| entry.path())
                    .collect::<Vec<_>>();
                for edition in editions {
                    let candidate = edition
                        .join("Common7")
                        .join("IDE")
                        .join("CommonExtensions")
                        .join("Microsoft")
                        .join("CMake")
                        .join("CMake")
                        .join("bin")
                        .join(executable_name);
                    if candidate.is_file() {
                        return candidate;
                    }
                }

                let candidate = installation
                    .join("Common7")
                    .join("IDE")
                    .join("CommonExtensions")
                    .join("Microsoft")
                    .join("CMake")
                    .join("CMake")
                    .join("bin")
                    .join(executable_name);
                if candidate.is_file() {
                    return candidate;
                }
            }
        }
    }

    panic!(
        "Could not locate CMake. Add cmake.exe to PATH or set CMAKE_COMMAND to its full path."
    );
}

fn run(command: &mut Command, description: &str) {
    let status = command.status().unwrap_or_else(|error| {
        panic!("Could not {description}: {error}");
    });
    assert!(status.success(), "Could not {description} (exit code {status})");
}

fn build_windows_virtual_camera() {
    let manifest_directory = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let native_directory = manifest_directory
        .parent()
        .unwrap()
        .join("native")
        .join("MiniMixVcam");
    let output_directory = PathBuf::from(env::var("OUT_DIR").unwrap()).join("minimix-vcam");
    let resource_directory = manifest_directory.join("target").join("vcam-runtime");
    let cmake = cmake_executable();
    let architecture = match env::var("CARGO_CFG_TARGET_ARCH").as_deref() {
        Ok("x86") => "Win32",
        Ok("aarch64") => "ARM64",
        _ => "x64",
    };

    println!(
        "cargo:rerun-if-changed={}",
        manifest_directory.join("windows/installer.nsh").display()
    );
    for file in [
        "CMakeLists.txt",
        "include/minimix/frame_protocol.h",
        "include/minimix/virtual_camera.h",
        "src/shared_memory_win.h",
        "src/shared_memory_win.cpp",
        "src/virtual_camera_source.cpp",
        "src/registrar_win.cpp",
        "tests/protocol_smoke.cpp",
        "tests/activation_smoke.cpp",
    ] {
        println!("cargo:rerun-if-changed={}", native_directory.join(file).display());
    }

    run(
        Command::new(&cmake)
            .arg("-S")
            .arg(&native_directory)
            .arg("-B")
            .arg(&output_directory)
            .arg("-A")
            .arg(architecture)
            .arg("-DBUILD_TESTING=ON")
            .arg("-DMINIMIX_VCAM_BUILD_SMOKE_TESTS=ON"),
        "configure the MiniMix virtual-camera runtime",
    );
    run(
        Command::new(&cmake)
            .arg("--build")
            .arg(&output_directory)
            .arg("--config")
            .arg("Release")
            .arg("--target")
            .arg("MiniMixVcam")
            .arg("MiniMixVcamRegistrar")
            .arg("minimix_vcam_smoke")
            .arg("minimix_vcam_activation_smoke"),
        "build the MiniMix virtual-camera runtime",
    );
    run(
        Command::new(cmake.with_file_name(if cfg!(windows) { "ctest.exe" } else { "ctest" }))
            .arg("--test-dir")
            .arg(&output_directory)
            .arg("--build-config")
            .arg("Release")
            .arg("--no-tests=error")
            .arg("--output-on-failure"),
        "verify the MiniMix virtual-camera runtime",
    );

    for name in ["MiniMixVcam.dll", "MiniMixVcamRegistrar.exe"] {
        let source = output_directory.join("Release").join(name);
        let destination = PathBuf::from(env::var("OUT_DIR").unwrap()).join(name);
        fs::copy(&source, &destination).unwrap_or_else(|error| {
            panic!("Could not copy {} to Cargo output: {error}", source.display());
        });
        fs::create_dir_all(&resource_directory).unwrap_or_else(|error| {
            panic!(
                "Could not create {}: {error}",
                resource_directory.display()
            );
        });
        let bundle_destination = resource_directory.join(name);
        fs::copy(&source, &bundle_destination).unwrap_or_else(|error| {
            panic!(
                "Could not copy {} to installer resources: {error}",
                source.display()
            );
        });
    }
}

fn main() {
    if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        build_windows_virtual_camera();
    }
    tauri_build::build()
}
